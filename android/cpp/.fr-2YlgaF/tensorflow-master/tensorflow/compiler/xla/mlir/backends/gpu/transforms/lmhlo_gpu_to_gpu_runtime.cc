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

#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Arith/IR/Arith.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/MemRef/IR/MemRef.h"  // from @llvm-project
#include "mlir/Dialect/SCF/IR/SCF.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/ImplicitLocOpBuilder.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/PatternMatch.h"  // from @llvm-project
#include "mlir/IR/SymbolTable.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"  // from @llvm-project
#include "tensorflow/compiler/xla/mlir/backends/gpu/transforms/uid_generator.h"
#include "tensorflow/compiler/xla/mlir/runtime/utils/custom_calls.h"
#include "tensorflow/compiler/xla/mlir_hlo/lhlo_gpu/IR/lhlo_gpu_ops.h"
#include "tensorflow/compiler/xla/stream_executor/blas.h"

namespace xla {
namespace gpu {

#define GEN_PASS_DEF_CONVERTLMHLOGPUTOGPURUNTIMEPASS
#include "tensorflow/compiler/xla/mlir/backends/gpu/transforms/passes.h.inc"

using namespace mlir;  // NOLINT

using mlir::lmhlo_gpu::CholeskyOp;
using mlir::lmhlo_gpu::ConvBackwardFilterOp;
using mlir::lmhlo_gpu::ConvBackwardInputOp;
using mlir::lmhlo_gpu::ConvForwardFusedOp;
using mlir::lmhlo_gpu::ConvForwardFusedSideInputOp;
using mlir::lmhlo_gpu::ConvForwardOp;
using mlir::lmhlo_gpu::CublasLtMatmulF8Op;
using mlir::lmhlo_gpu::CublasLtMatmulOp;
using mlir::lmhlo_gpu::CudnnConvReorderFilterAndBiasOp;
using mlir::lmhlo_gpu::CudnnConvReorderFilterOp;
using mlir::lmhlo_gpu::GEMMOp;

using xla::runtime::CustomCallDeclarations;

class ConvertLmhloGpuToGpuRuntimePass
    : public impl::ConvertLmhloGpuToGpuRuntimePassBase<
          ConvertLmhloGpuToGpuRuntimePass> {
  void runOnOperation() override;

  void getDependentDialects(DialectRegistry& registry) const override {
    registry.insert<arith::ArithDialect, func::FuncDialect,
                    memref::MemRefDialect, scf::SCFDialect>();
  }
};

//===----------------------------------------------------------------------===//

class GemmOpLowering : public OpRewritePattern<GEMMOp> {
  static constexpr const char kCustomCallTarget[] = "xla.gpu.gemm";

 public:
  GemmOpLowering(MLIRContext* ctx, UidGenerator& uid,
                 CustomCallDeclarations& custom_calls)
      : OpRewritePattern<GEMMOp>(ctx), uid_(uid), custom_calls_(custom_calls) {}

  LogicalResult matchAndRewrite(GEMMOp op,
                                PatternRewriter& rewriter) const override {
    // Get or create a custom call function declaration.
    ImplicitLocOpBuilder b(op.getLoc(), rewriter);
    func::FuncOp callee = custom_calls_.GetOrCreate(b, kCustomCallTarget, op);

    // Convert Gemm to a function call.
    auto call = rewriter.create<func::CallOp>(op.getLoc(), callee.getName(),
                                              TypeRange(), op.getOperands());

    // Assign a unique id to this instance of a gemm operation.
    call->setAttr(b.getStringAttr("uid"), b.getI64IntegerAttr(uid_.uid()));

    // Copy backend specific attributes.
    auto algorithm_attr =
        op.getAlgorithm()
            ? op.getAlgorithmAttr()
            : b.getI64IntegerAttr(stream_executor::blas::kDefaultGemmAlgo);
    call->setAttr(b.getStringAttr("algorithm"), algorithm_attr);
    call->setAttr(b.getStringAttr("alpha_imag"), op.getAlphaImagAttr());
    call->setAttr(b.getStringAttr("alpha_real"), op.getAlphaRealAttr());
    call->setAttr(b.getStringAttr("beta"), op.getBetaAttr());
    call->setAttr(b.getStringAttr("dot_dims"), op.getDotDimensionNumbers());

    if (auto precisions = op.getPrecisionConfig()) {
      llvm::SmallVector<int32_t> values;
      for (auto precision : *precisions) {
        auto value = precision.cast<mhlo::PrecisionAttr>().getValue();
        values.push_back(static_cast<int32_t>(value));
      }
      call->setAttr(b.getStringAttr("precision"), b.getI32TensorAttr(values));
    } else {
      call->setAttr(b.getStringAttr("precision"), b.getI32TensorAttr({0, 0}));
    }

    // Erase the original gemm operation.
    rewriter.eraseOp(op);

    return success();
  }

 private:
  UidGenerator& uid_;
  CustomCallDeclarations& custom_calls_;
};

//===----------------------------------------------------------------------===//

class CublasLtMatmulOpLowering : public OpRewritePattern<CublasLtMatmulOp> {
 private:
  static constexpr const char kCustomCallTarget[] = "xla.gpu.cublas.lt.matmul";

 public:
  CublasLtMatmulOpLowering(MLIRContext* ctx, UidGenerator& uid,
                           CustomCallDeclarations& custom_calls)
      : OpRewritePattern<CublasLtMatmulOp>(ctx),
        uid_(uid),
        custom_calls_(custom_calls) {}

  LogicalResult matchAndRewrite(CublasLtMatmulOp op,
                                PatternRewriter& rewriter) const override {
    // Get the custom call target.
    std::string matmul = kCustomCallTarget;

    switch (op.getEpilogue()) {
      case mlir::lmhlo_gpu::CublasLtMatmulEpilogue::Default:
      case mlir::lmhlo_gpu::CublasLtMatmulEpilogue::Relu:
      case mlir::lmhlo_gpu::CublasLtMatmulEpilogue::Gelu:
        if (op.getNumOperands() != 4) {
          return op.emitOpError("unexpected number of operands for matmul");
        }
        break;
      case mlir::lmhlo_gpu::CublasLtMatmulEpilogue::Bias:
      case mlir::lmhlo_gpu::CublasLtMatmulEpilogue::BiasRelu:
      case mlir::lmhlo_gpu::CublasLtMatmulEpilogue::BiasGelu:
        if (op.getNumOperands() != 5) {
          return op.emitOpError("unexpected number of operands for matmul");
        }
        matmul += ".bias";
        break;
      case mlir::lmhlo_gpu::CublasLtMatmulEpilogue::GeluAux:
        if (op.getNumOperands() != 5) {
          return op.emitOpError("unexpected number of operands for matmul");
        }
        matmul += ".aux";
        break;
      case mlir::lmhlo_gpu::CublasLtMatmulEpilogue::BiasGeluAux:
        if (op.getNumOperands() != 6) {
          return op.emitOpError("unexpected number of operands for matmul");
        }
        matmul += ".bias.aux";
        break;
    }

    // Get or create a custom call function declaration.
    ImplicitLocOpBuilder b(op.getLoc(), rewriter);
    func::FuncOp callee = custom_calls_.GetOrCreate(b, matmul, op);

    // Convert matmul to a function call.
    auto call = rewriter.create<func::CallOp>(op.getLoc(), callee.getName(),
                                              TypeRange(), op.getOperands());

    // Assign a unique id to this instance of a matmul operation.
    call->setAttr(b.getStringAttr("uid"), b.getI64IntegerAttr(uid_.uid()));

    // Copy backend specific attributes.
    call->setAttr(b.getStringAttr("algorithm"), op.getAlgorithmAttr());
    call->setAttr(b.getStringAttr("alpha_imag"), op.getAlphaImagAttr());
    call->setAttr(b.getStringAttr("alpha_real"), op.getAlphaRealAttr());
    call->setAttr(b.getStringAttr("beta"), op.getBetaAttr());
    call->setAttr(b.getStringAttr("dot_dims"), op.getDotDimensionNumbers());
    call->setAttr(b.getStringAttr("epilogue"), op.getEpilogueAttr());

    // TODO(ezhulenev): Today we can't pass an array of enum attributes to the
    // custom call. Also we do not have a corresponding precision enum on the
    // SE/XLA side, so we encode it as an i32 array (tensor).
    if (auto precisions = op.getPrecisionConfig()) {
      llvm::SmallVector<int32_t> values;
      for (auto precision : *precisions) {
        auto value = precision.cast<mhlo::PrecisionAttr>().getValue();
        values.push_back(static_cast<int32_t>(value));
      }
      call->setAttr(b.getStringAttr("precision"), b.getI32TensorAttr(values));
    } else {
      call->setAttr(b.getStringAttr("precision"), b.getI32TensorAttr({0, 0}));
    }

    // Erase the original matmul operation.
    rewriter.eraseOp(op);

    return success();
  }

 private:
  UidGenerator& uid_;
  CustomCallDeclarations& custom_calls_;
};

// As above for FP8 Custom Calls.
class CublasLtMatmulF8OpLowering : public OpRewritePattern<CublasLtMatmulF8Op> {
 private:
  static constexpr const char kCustomCallTarget[] =
      "xla.gpu.cublas.lt.matmul.f8";

 public:
  CublasLtMatmulF8OpLowering(MLIRContext* ctx, UidGenerator& uid,
                             CustomCallDeclarations& custom_calls)
      : OpRewritePattern<CublasLtMatmulF8Op>(ctx),
        uid_(uid),
        custom_calls_(custom_calls) {}

  LogicalResult matchAndRewrite(CublasLtMatmulF8Op op,
                                PatternRewriter& rewriter) const override {
    // Get the custom call target.
    std::string matmul = kCustomCallTarget;

    if (op.getNumOperands() == 9) {
      matmul += ".d_amax";
    } else if (op.getNumOperands() != 8) {
      return op.emitOpError("unexpected number of operands for matmul");
    }

    // Get or create a custom call function declaration.
    ImplicitLocOpBuilder b(op.getLoc(), rewriter);
    func::FuncOp callee = custom_calls_.GetOrCreate(b, matmul, op);

    // Convert matmul to a function call.
    auto call = rewriter.create<func::CallOp>(op.getLoc(), callee.getName(),
                                              TypeRange(), op.getOperands());

    // Assign a unique id to this instance of a matmul operation.
    call->setAttr(b.getStringAttr("uid"), b.getI64IntegerAttr(uid_.uid()));

    // Copy backend specific attributes.
    call->setAttr(b.getStringAttr("algorithm"), op.getAlgorithmAttr());
    call->setAttr(b.getStringAttr("alpha_imag"), op.getAlphaImagAttr());
    call->setAttr(b.getStringAttr("alpha_real"), op.getAlphaRealAttr());
    call->setAttr(b.getStringAttr("beta"), op.getBetaAttr());
    call->setAttr(b.getStringAttr("dot_dims"), op.getDotDimensionNumbers());
    call->setAttr(b.getStringAttr("epilogue"), op.getEpilogueAttr());

    // TODO(ezhulenev): Today we can't pass an array of enum attributes to the
    // custom call. Also we do not have a corresponding precision enum on the
    // SE/XLA side, so we encode it as an i32 array (tensor).
    if (auto precisions = op.getPrecisionConfig()) {
      llvm::SmallVector<int32_t> values;
      for (auto precision : *precisions) {
        auto value = precision.cast<mhlo::PrecisionAttr>().getValue();
        values.push_back(static_cast<int32_t>(value));
      }
      call->setAttr(b.getStringAttr("precision"), b.getI32TensorAttr(values));
    } else {
      call->setAttr(b.getStringAttr("precision"), b.getI32TensorAttr({0, 0}));
    }

    // Erase the original matmul operation.
    rewriter.eraseOp(op);

    return success();
  }

 private:
  UidGenerator& uid_;
  CustomCallDeclarations& custom_calls_;
};

//===----------------------------------------------------------------------===//

template <typename Conv>
class ConvOpLowering : public OpRewritePattern<Conv> {
 private:
  static StringRef CustomCallTarget(ConvForwardOp) {
    return "xla.gpu.conv.forward";
  }
  static StringRef CustomCallTarget(ConvForwardFusedOp) {
    return "xla.gpu.conv.forward.fused";
  }
  static StringRef CustomCallTarget(ConvForwardFusedSideInputOp) {
    return "xla.gpu.conv.forward.fused.side_input";
  }
  static StringRef CustomCallTarget(ConvBackwardFilterOp) {
    return "xla.gpu.conv.backward.filter";
  }
  static StringRef CustomCallTarget(ConvBackwardInputOp) {
    return "xla.gpu.conv.backward.input";
  }

 public:
  explicit ConvOpLowering(MLIRContext* ctx, UidGenerator& uid,
                          CustomCallDeclarations& custom_calls)
      : OpRewritePattern<Conv>(ctx), uid_(uid), custom_calls_(custom_calls) {}

  LogicalResult matchAndRewrite(Conv op,
                                PatternRewriter& rewriter) const override {
    // Get or create a custom call function declaration.
    ImplicitLocOpBuilder b(op.getLoc(), rewriter);
    func::FuncOp callee =
        custom_calls_.GetOrCreate(b, CustomCallTarget(op), op);

    // Convert Conv to a function call.
    auto call = rewriter.create<func::CallOp>(op.getLoc(), callee.getName(),
                                              TypeRange(), op.getOperands());

    // Helper functins to copy attributes from the conv op to the custom call.
    auto set_attr = [&](StringRef name, Attribute attr) {
      call->setAttr(b.getStringAttr(name), attr);
    };

    auto set_xi64 = [&](StringRef name,
                        std::optional<DenseIntElementsAttr> attr) {
      SmallVector<int64_t> values;
      if (attr.has_value())
        values = llvm::to_vector(attr->getValues<int64_t>());
      set_attr(name, b.getI64TensorAttr(values));
    };

    // Convert `BoolElementsAttr` to i64 before passing to the runtime.
    // TODO(ezhulenev): Allow passing boolean tensors to the XLA custom calls.
    auto set_xi1 = [&](StringRef name, std::optional<DenseElementsAttr> attr) {
      SmallVector<int64_t> values;
      if (attr.has_value())
        values.assign(attr->getValues<bool>().begin(),
                      attr->getValues<bool>().end());
      set_attr(name, b.getI64TensorAttr(values));
    };

    // Assign a unique id to this instance of a conv operation.
    call->setAttr(b.getStringAttr("uid"), b.getI64IntegerAttr(uid_.uid()));

    // Copy dimension number attributes.
    call->setAttr(b.getStringAttr("conv_dims"), op.getDimensionNumbers());

    // Copy convolution window attributes.
    set_xi1("window_reversal", op.getWindowReversal());
    set_xi64("window_strides", op.getWindowStrides());
    set_xi64("lhs_dilation", op.getLhsDilation());
    set_xi64("rhs_dilation", op.getRhsDilation());
    set_xi64("padding", op.getPadding());

    // Copy backend config.
    call->setAttr(b.getStringAttr("backend_config"), op.getBackendConfig());

    // Copy remaining attributes.
    set_attr("feature_group_count", op.getFeatureGroupCountAttr());
    set_attr("result_scale", op.getResultScaleAttr());

    // Copy attributes specific for fused convolutions.
    if (auto fused = dyn_cast<ConvForwardFusedOp>(op.getOperation())) {
      call->setAttr(b.getStringAttr("activation_mode"),
                    fused.getActivationModeAttr());
    }

    // Copy attributes specific for fused convolutions with side input.
    if (auto fused = dyn_cast<ConvForwardFusedSideInputOp>(op.getOperation())) {
      call->setAttr(b.getStringAttr("activation_mode"),
                    fused.getActivationModeAttr());
      set_attr("side_input_scale", fused.getSideInputScaleAttr());
    }

    // Erase the original conv operation.
    rewriter.eraseOp(op);

    return success();
  }

 private:
  UidGenerator& uid_;
  CustomCallDeclarations& custom_calls_;
};

class ConvForwardOpLowering : public ConvOpLowering<ConvForwardOp> {
 public:
  using ConvOpLowering::ConvOpLowering;
};

class ConvForwardFusedOpLowering : public ConvOpLowering<ConvForwardFusedOp> {
 public:
  using ConvOpLowering::ConvOpLowering;
};

class ConvBackwardFilterOpLowering
    : public ConvOpLowering<ConvBackwardFilterOp> {
 public:
  using ConvOpLowering::ConvOpLowering;
};

class ConvBackwardInputOpLowering : public ConvOpLowering<ConvBackwardInputOp> {
 public:
  using ConvOpLowering::ConvOpLowering;
};

class ConvForwardFusedSideInputOpLowering
    : public ConvOpLowering<ConvForwardFusedSideInputOp> {
 public:
  using ConvOpLowering::ConvOpLowering;
};

//===----------------------------------------------------------------------===//

template <typename ConvReorder>
class CudnnConvReorderOpLowering : public OpRewritePattern<ConvReorder> {
 private:
  static StringRef CustomCallTarget(CudnnConvReorderFilterOp) {
    return "xla.gpu.conv.reorder.filter";
  }
  static StringRef CustomCallTarget(CudnnConvReorderFilterAndBiasOp) {
    return "xla.gpu.conv.reorder.filter_and_bias";
  }

 public:
  explicit CudnnConvReorderOpLowering(MLIRContext* ctx,
                                      CustomCallDeclarations& custom_calls)
      : OpRewritePattern<ConvReorder>(ctx), custom_calls_(custom_calls) {}

  LogicalResult matchAndRewrite(ConvReorder op,
                                PatternRewriter& rewriter) const override {
    // Get or create a custom call function declaration.
    ImplicitLocOpBuilder b(op.getLoc(), rewriter);
    func::FuncOp callee =
        custom_calls_.GetOrCreate(b, CustomCallTarget(op), op);

    auto filterDims = rewriter.getDenseI64ArrayAttr(
        llvm::to_vector(op.getFilterDims().template getValues<int64_t>()));

    // Replace ConvOp with an equivalent custom call.
    auto call = rewriter.replaceOpWithNewOp<func::CallOp>(
        op, callee.getName(), TypeRange(), op.getOperands());
    call->setAttr(b.getStringAttr("filter_dims"), filterDims);

    return success();
  }

 private:
  CustomCallDeclarations& custom_calls_;
};

class CudnnConvReorderFilterOpLowering
    : public CudnnConvReorderOpLowering<CudnnConvReorderFilterOp> {
 public:
  using CudnnConvReorderOpLowering::CudnnConvReorderOpLowering;
};

class CudnnConvReorderFilterAndBiasOpLowering
    : public CudnnConvReorderOpLowering<CudnnConvReorderFilterAndBiasOp> {
 public:
  using CudnnConvReorderOpLowering::CudnnConvReorderOpLowering;
};

//===----------------------------------------------------------------------===//

class CholeskyOpLowering : public OpRewritePattern<CholeskyOp> {
 private:
  static constexpr const char kCustomCallTarget[] = "xla.gpu.cholesky";

 public:
  explicit CholeskyOpLowering(MLIRContext* ctx,
                              CustomCallDeclarations& custom_calls)
      : OpRewritePattern(ctx), custom_calls_(custom_calls) {}

  LogicalResult matchAndRewrite(CholeskyOp op,
                                PatternRewriter& rewriter) const override {
    // Get or create a custom call function declaration.
    ImplicitLocOpBuilder b(op.getLoc(), rewriter);
    func::FuncOp callee = custom_calls_.GetOrCreate(b, kCustomCallTarget, op);

    // Convert Cholesky to a function call.
    auto call = rewriter.create<func::CallOp>(op.getLoc(), callee.getName(),
                                              TypeRange(), op.getOperands());

    const auto& dims =
        op.getInput().getType().cast<mlir::MemRefType>().getShape();
    if (dims.size() < 2)
      return op.emitOpError() << "Input's dimension count (" << dims.size()
                              << ") must be 2 or greater.";
    int64_t n = dims[dims.size() - 1];
    int64_t batch_size =
        std::accumulate(dims.begin(), dims.end() - 2, int64_t{1},
                        [](int64_t a, int64_t b) { return a * b; });

    // Copy backend specific attributes.
    call->setAttr(b.getStringAttr("batch_size"),
                  b.getI64IntegerAttr(batch_size));
    call->setAttr(b.getStringAttr("n"), b.getI64IntegerAttr(n));
    call->setAttr(b.getStringAttr("is_lower"), op.getIsLowerAttr());

    // Erase the original Cholesky operation.
    rewriter.eraseOp(op);

    return success();
  }

 private:
  CustomCallDeclarations& custom_calls_;
};

//===----------------------------------------------------------------------===//

void ConvertLmhloGpuToGpuRuntimePass::runOnOperation() {
  ModuleOp module = getOperation();
  MLIRContext* ctx = module.getContext();

  // Keep track of the custom calls created from the lowered operations.
  SymbolTable sym_table(module);
  CustomCallDeclarations custom_calls(std::move(sym_table));

  // Convert lmhlo_gpu operations to XLA gpu runtime custom calls.
  RewritePatternSet patterns(ctx);

  // Each unique Gemm/Matmul operation in the module will get assigned a uid.
  UidGenerator matmul_uid;
  patterns.insert<GemmOpLowering, CublasLtMatmulOpLowering,
                  CublasLtMatmulF8OpLowering>(ctx, matmul_uid, custom_calls);

  // Each unique Conv operation in the module will get assigned a uid.
  UidGenerator conv_uid;
  patterns.insert<ConvForwardOpLowering, ConvForwardFusedOpLowering,
                  ConvForwardFusedSideInputOpLowering,
                  ConvBackwardFilterOpLowering, ConvBackwardInputOpLowering>(
      ctx, conv_uid, custom_calls);

  // Patterns for every other Gpu operation.
  patterns.insert<CudnnConvReorderFilterOpLowering>(ctx, custom_calls);
  patterns.insert<CudnnConvReorderFilterAndBiasOpLowering>(ctx, custom_calls);
  patterns.insert<CholeskyOpLowering>(ctx, custom_calls);

  if (failed(applyPatternsAndFoldGreedily(module, std::move(patterns))))
    return signalPassFailure();
}

std::unique_ptr<mlir::OperationPass<mlir::ModuleOp>>
createConvertLmhloGpuToGpuRuntimePass() {
  return std::make_unique<ConvertLmhloGpuToGpuRuntimePass>();
}

}  // namespace gpu
}  // namespace xla
