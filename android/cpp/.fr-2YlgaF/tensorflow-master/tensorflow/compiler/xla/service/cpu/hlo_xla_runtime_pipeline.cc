/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/cpu/hlo_xla_runtime_pipeline.h"

#include <utility>

#include "mlir/Dialect/Linalg/Transforms/TilingInterfaceImpl.h"
#include "mlir/Conversion/BufferizationToMemRef/BufferizationToMemRef.h"  // from @llvm-project
#include "mlir/Conversion/ComplexToStandard/ComplexToStandard.h"  // from @llvm-project
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"  // from @llvm-project
#include "mlir/Conversion/ShapeToStandard/ShapeToStandard.h"  // from @llvm-project
#include "mlir/Conversion/TensorToLinalg/TensorToLinalgPass.h"  // from @llvm-project
#include "mlir/Dialect/Arith/Transforms/BufferizableOpInterfaceImpl.h"  // from @llvm-project
#include "mlir/Dialect/Bufferization/Transforms/FuncBufferizableOpInterfaceImpl.h"  // from @llvm-project
#include "mlir/Dialect/Bufferization/Transforms/OneShotAnalysis.h"  // from @llvm-project
#include "mlir/Dialect/Bufferization/Transforms/Passes.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/Linalg/Passes.h"  // from @llvm-project
#include "mlir/Dialect/Linalg/Transforms/BufferizableOpInterfaceImpl.h"  // from @llvm-project
#include "mlir/Dialect/MemRef/Transforms/Passes.h"  // from @llvm-project
#include "mlir/Dialect/SCF/Transforms/BufferizableOpInterfaceImpl.h"  // from @llvm-project
#include "mlir/Dialect/Shape/Transforms/BufferizableOpInterfaceImpl.h"  // from @llvm-project
#include "mlir/Dialect/Shape/Transforms/Passes.h"  // from @llvm-project
#include "mlir/Dialect/SparseTensor/Transforms/Passes.h"  // from @llvm-project
#include "mlir/Dialect/Tensor/Transforms/BufferizableOpInterfaceImpl.h"  // from @llvm-project
#include "mlir/Dialect/Vector/Transforms/BufferizableOpInterfaceImpl.h"  // from @llvm-project
#include "mlir/Pass/PassManager.h"  // from @llvm-project
#include "mlir/Transforms/Passes.h"  // from @llvm-project
#include "tensorflow/compiler/xla/mlir/backends/cpu/transforms/passes.h"
#include "tensorflow/compiler/xla/mlir/framework/transforms/passes.h"
#include "tensorflow/compiler/xla/mlir/runtime/transforms/compiler.h"
#include "tensorflow/compiler/xla/mlir_hlo/deallocation/transforms/passes.h"
#include "tensorflow/compiler/xla/mlir_hlo/gml_st/interfaces/bufferizable_op_interface_impl.h"
#include "tensorflow/compiler/xla/mlir_hlo/gml_st/transforms/passes.h"
#include "tensorflow/compiler/xla/mlir_hlo/mhlo/interfaces/bufferizable_op_interface_impl.h"
#include "tensorflow/compiler/xla/mlir_hlo/mhlo/transforms/passes.h"
#include "tensorflow/compiler/xla/mlir_hlo/transforms/passes.h"
#include "tensorflow/compiler/xla/status.h"
#include "tensorflow/tsl/platform/errors.h"
#include "tensorflow/tsl/platform/logging.h"

namespace xla {
namespace cpu {
namespace {

using mlir::func::FuncOp;

mlir::bufferization::OneShotBufferizationOptions GetBufferizationOptions() {
  using mlir::bufferization::BufferizationOptions;
  using mlir::bufferization::LayoutMapOption;
  using mlir::bufferization::OneShotBufferizationOptions;

  OneShotBufferizationOptions options;
  options.bufferizeFunctionBoundaries = true;
  options.allowReturnAllocs = true;
  options.functionBoundaryTypeConversion = LayoutMapOption::IdentityLayoutMap;
  options.unknownTypeConverterFn = [](mlir::Value value,
                                      mlir::Attribute memorySpace,
                                      const BufferizationOptions& options) {
    return mlir::bufferization::getMemRefTypeWithStaticIdentityLayout(
        value.getType().cast<mlir::TensorType>(), memorySpace);
  };
  return options;
}

void AddSparsificationPasses(mlir::OpPassManager& pm) {
  pm.addNestedPass<FuncOp>(createSparseCustomCallToPackUnpackOpPass());
  pm.addNestedPass<FuncOp>(mlir::createLinalgGeneralizationPass());
  pm.addNestedPass<FuncOp>(
      mlir::bufferization::createEmptyTensorToAllocTensorPass());
  pm.addPass(mlir::createPreSparsificationRewritePass());
  pm.addPass(mlir::createSparsificationAndBufferizationPass(
      GetBufferizationOptions(), mlir::SparsificationOptions(),
      mlir::SparseTensorConversionOptions(), /*enableRuntimeLibrary=*/false,
      /*enableBufferInitialization=*/false,
      /*vectorLength=*/0,
      /*enableVLAVectorization=*/false,
      /*enableSIMDIndex32*/ false));
  pm.addNestedPass<mlir::func::FuncOp>(
      mlir::bufferization::createFinalizingBufferizePass());
}

}  // namespace

// -------------------------------------------------------------------------- //
// Assemble a HLO XLA Runtime pipeline to lower from HLO to Linalg on buffers.
// -------------------------------------------------------------------------- //

static Status CreateHloXlaPipeline(
    mlir::OpPassManager& pm, const HloXlaRuntimePipelineOptions& options) {
  // Resolve all shape constraints (e.g. broadcast constraints that can be
  // proved statically and changed to const witness) early to allow more
  // efficient broadcast operations moving.
  // Move up broadcasting operations to allow for more fusion opportunities.
  pm.addPass(mlir::createInlinerPass());
  pm.addPass(mlir::mhlo::createExpandHloTuplesPass("main"));
  // TODO(b/233771980): Remove once custom_call doesn't use tuples.
  pm.addNestedPass<mlir::func::FuncOp>(mlir::mhlo::createFlattenTuplePass());
  pm.addPass(createXlaAbiLegalizationPass());
  pm.addNestedPass<mlir::func::FuncOp>(
      mlir::mhlo::createLegalizeGeneralDotPass());
  pm.addNestedPass<mlir::func::FuncOp>(
      mlir::mhlo::createBroadcastPropagationPass());
  pm.addPass(mlir::createCSEPass());
  pm.addPass(mlir::createCanonicalizerPass());

  // Some early sparse rewriting rules.
  if (options.sparse_bufferization) {
    pm.addNestedPass<mlir::func::FuncOp>(
        mlir::mhlo::createSparseRewritingPass());
  }

  // Transform HLO operations to Linalg.
  pm.addNestedPass<mlir::func::FuncOp>(mlir::mhlo::createLegalizeSortPass());
  pm.addNestedPass<mlir::func::FuncOp>(
      mlir::mhlo::createLegalizeControlFlowPass());
  pm.addPass(::mlir::mhlo::createLegalizeToArithmeticPass());
  pm.addNestedPass<mlir::func::FuncOp>(
      xla::cpu::createLegalizeCollectiveOpsPass());
  pm.addNestedPass<mlir::func::FuncOp>(
      mlir::mhlo::createMhloExpandOpsSimplifierPass());
  pm.addNestedPass<mlir::func::FuncOp>(
      mlir::mhlo::createHloCanonicalizeScatterPass());
  pm.addNestedPass<FuncOp>(mlir::mhlo::createHloCanonicalizeDotPass());
  pm.addNestedPass<FuncOp>(mlir::mhlo::createGroupReductionDimensionsPass());
  // TODO(kramerb): Give THLO lowerings priority over linalg when it's ready for
  // concat, reduce and friends.
  pm.addNestedPass<mlir::func::FuncOp>(
      mlir::mhlo::createLegalizeHloToLinalgPass(
          options.enable_tiling_and_fusion));
  pm.addNestedPass<mlir::func::FuncOp>(
      mlir::mhlo::createLegalizeMHLOToTHLOPass());

  // Lower index cast on tensors to tensor.generate.
  pm.addNestedPass<mlir::func::FuncOp>(mlir::createLowerIndexCastPass());

  pm.addPass(mlir::mhlo::createConvertToSignlessPass());

  // Transform scatter ops.
  if (!options.enable_tiling_and_fusion) {
    pm.addNestedPass<mlir::func::FuncOp>(
        mlir::gml_st::createTransformScatterForCpuPass());
  }

  // Lower shape dialect to standard to enable linalg canonicalizations (e.g.
  // use linalg inputs instead of outputs for memref.dim operations).
  pm.addNestedPass<mlir::func::FuncOp>(mlir::mhlo::createShapeSimplification());
  pm.addNestedPass<mlir::func::FuncOp>(mlir::createShapeToShapeLowering());
  pm.addPass(mlir::createConvertShapeToStandardPass());
  pm.addNestedPass<mlir::func::FuncOp>(
      mlir::createConvertShapeConstraintsPass());

  // Fuse Linalg on tensors operations.
  pm.addPass(mlir::createCSEPass());
  pm.addPass(mlir::memref::createResolveShapedTypeResultDimsPass());
  pm.addPass(mlir::createCanonicalizerPass());
  if (options.enable_tiling_and_fusion) {
    mlir::gml_st::addDefaultCPUTilingPipeline(pm);
  } else {
    pm.addNestedPass<mlir::func::FuncOp>(
        mlir::createLinalgElementwiseOpFusionPass());
  }
  pm.addPass(mlir::createReconcileUnrealizedCastsPass());
  pm.addPass(mlir::createConvertTensorToLinalgPass());

  // Detensorize SCF iter args.
  pm.addNestedPass<mlir::func::FuncOp>(mlir::createDetensorizeScfOpsPass());
  // mhlo ops on unit tensors generate trivial linalg.generics, which
  // one-shot-bufferize generates unnecessary allocs for. The detensorize pass
  // replaces these linalg.generics with scalar ops.
  auto detensorize = mlir::createLinalgDetensorizePass();
  if (detensorize->initializeOptions("aggressive-mode=true").failed()) {
    return tsl::errors::Internal("Failed to set up detensorize pass.");
  }
  pm.addNestedPass<mlir::func::FuncOp>(std::move(detensorize));
  pm.addNestedPass<mlir::func::FuncOp>(mlir::gml_st::createScalarizationPass());
  pm.addNestedPass<mlir::func::FuncOp>(
      mlir::bufferization::createEmptyTensorToAllocTensorPass());

  // Always run canonicalizer (which does dead code removal) before
  // bufferizing anything.
  pm.addPass(mlir::createCanonicalizerPass());

  if (options.sparse_bufferization) {
    // Convert Sparse tensors.
    AddSparsificationPasses(pm);
  } else {
    if (options.experimental_deallocation) {
      // Experimental deallocation needs input IR without any buffer reuse to
      // work optimally. This pass ensures that's the case.
      pm.addNestedPass<FuncOp>(
          mlir::deallocation::createSplitAllocTensorsPass());
    }
    pm.addPass(mlir::hlo::createOneShotBufferizePass());
  }
  pm.addNestedPass<mlir::func::FuncOp>(createRewriteReallocToAllocPass());

  if (options.enable_tiling_and_fusion) {
    pm.addNestedPass<FuncOp>(mlir::gml_st::createVectorizeCopyPass());
    pm.addNestedPass<FuncOp>(mlir::gml_st::createSimplifyDeadCopyPass());
  }
  // Handle framework specific requirements for buffers and then insert
  // deallocations for temporary buffers.
  pm.addNestedPass<mlir::func::FuncOp>(mlir::createConvertLinalgToLoopsPass());
  pm.addNestedPass<mlir::func::FuncOp>(mlir::gml_st::createGmlStToScfPass());
  pm.addPass(mlir::createCSEPass());
  pm.addPass(mlir::createCanonicalizerPass());
  mlir::bufferization::BufferResultsToOutParamsOptions out_params_options;
  out_params_options.filterFn = [](mlir::func::FuncOp* func) {
    // Only transform the entry point.
    return func->getSymName() == "main";
  };
  pm.addPass(mlir::bufferization::createBufferResultsToOutParamsPass(
      out_params_options));
  if (options.outline_with_xla_framework) {
    pm.addPass(mlir::xla_framework::CreateOutlineWithXLAFrameworkPass());
  }
  pm.addPass(mlir::createInlinerPass());

  if (options.experimental_deallocation) {
    CHECK(!options.sparse_bufferization)
        << "Sparse bufferization and experimental deallocation are mutually "
           "exclusive.";
    pm.addNestedPass<FuncOp>(mlir::deallocation::createDeallocatePass());
    pm.addNestedPass<FuncOp>(mlir::createCanonicalizerPass());
    pm.addNestedPass<FuncOp>(mlir::deallocation::createBufferReusePass());
    pm.addNestedPass<FuncOp>(mlir::createCanonicalizerPass());
    pm.addNestedPass<FuncOp>(mlir::deallocation::createDeallocationToScfPass());
  } else {
    pm.addNestedPass<FuncOp>(
        mlir::bufferization::createPromoteBuffersToStackPass(nullptr));
    pm.addNestedPass<mlir::func::FuncOp>(
        mlir::bufferization::createBufferDeallocationPass());
    pm.addPass(mlir::createBufferizationToMemRefPass());
  }

  pm.addNestedPass<mlir::func::FuncOp>(
      xla::cpu::createRemoveCopiesToOutParamsPass());

  // Specialize linalg.matmul to linalg.dot, linalg.matvec or linalg.vecmat,
  // and immediately canonicalize to clean up not taken branches.
  // pm.addNestedPass<mlir::func::FuncOp>(CreateLinalgMatmulSpecializationPass());
  pm.addPass(mlir::createCanonicalizerPass());

  // TODO(tpopp): Move hits to mlir::hlo::createGenericHostToLLVMPass?
  pm.addNestedPass<mlir::func::FuncOp>(
      mlir::createConvertComplexToStandardPass());

  pm.addPass(mlir::createCSEPass());
  pm.addPass(mlir::createCanonicalizerPass());

  pm.addNestedPass<FuncOp>(
      mlir::gml_st::createLowerVectorsPass(options.enable_avx2));
  pm.addNestedPass<FuncOp>(xla::cpu::createLegalizeI1VectorTransferOpsPass());
  pm.addNestedPass<FuncOp>(
      xla::cpu::createConvertXlaCpuMemRefElementCastToLLVMPass());
  pm.addNestedPass<FuncOp>(
      mlir::deallocation::createConvertDeallocationOpsToLLVM());
  return OkStatus();
}

Status CreateHloXlaRuntimePipeline(
    xla::runtime::PassManager& passes,
    const HloXlaRuntimePipelineOptions& options) {
  return CreateHloXlaPipeline(*passes, options);
}

Status CreateDefaultHloXlaRuntimePipeline(xla::runtime::PassManager& passes) {
  HloXlaRuntimePipelineOptions options;
  return CreateHloXlaPipeline(*passes, options);
}

void RegisterHloXlaRuntimePipelineDialects(mlir::DialectRegistry& dialects) {
  mlir::arith::registerBufferizableOpInterfaceExternalModels(dialects);
  mlir::bufferization::func_ext::registerBufferizableOpInterfaceExternalModels(
      dialects);
  mlir::gml_st::registerBufferizableOpInterfaceExternalModels(dialects);
  mlir::linalg::registerBufferizableOpInterfaceExternalModels(dialects);
  mlir::linalg::registerTilingInterfaceExternalModels(dialects);
  mlir::mhlo::registerBufferizableOpInterfaceExternalModels(dialects);
  mlir::scf::registerBufferizableOpInterfaceExternalModels(dialects);
  mlir::shape::registerBufferizableOpInterfaceExternalModels(dialects);
  mlir::tensor::registerBufferizableOpInterfaceExternalModels(dialects);
  mlir::vector::registerBufferizableOpInterfaceExternalModels(dialects);
}

static mlir::PassPipelineRegistration<> hlo_xla_runtime_pipeline(
    "hlo-xla-runtime-pipeline",
    "Convert HLO dialect to XLA Runtime compatible dialects",
    [](mlir::OpPassManager& pm) {
      HloXlaRuntimePipelineOptions options;
      Status status = CreateHloXlaPipeline(pm, options);
      if (!status.ok()) {
        LOG(FATAL) << "HLO-XLA Runtime pipeline failed with: "
                   << status.error_message();
      }
    });

static mlir::PassPipelineRegistration<> sparsification_pipeline(
    "hlo-xla-runtime-sparsification",
    "Sparsification passes from HLO-XLA Runtime pipeline",
    AddSparsificationPasses);

}  // namespace cpu
}  // namespace xla
