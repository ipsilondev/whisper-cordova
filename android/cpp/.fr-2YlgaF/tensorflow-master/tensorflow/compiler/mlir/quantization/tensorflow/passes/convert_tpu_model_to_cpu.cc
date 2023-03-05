/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

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

// Convert TPU models to CPU for the TF quantizer to play with the input TPU
// graph.

#include <memory>
#include <string>
#include <utility>

#include "llvm/Support/Casting.h"
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Pass/PassManager.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"  // from @llvm-project
#include "mlir/Transforms/Passes.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/lite/quantization/quantization_config.h"
#include "tensorflow/compiler/mlir/lite/quantization/quantization_utils.h"
#include "tensorflow/compiler/mlir/lite/transforms/passes.h"
#include "tensorflow/compiler/mlir/quantization/tensorflow/passes/passes.h"
#include "tensorflow/compiler/mlir/quantization/tensorflow/passes/utils.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"

namespace mlir {
namespace quant {
namespace {

// Convert a TPU model to be compatible on CPU by rewriting/removing TPU ops.
class ConvertTpuModelToCpuPass
    : public PassWrapper<ConvertTpuModelToCpuPass, OperationPass<ModuleOp>> {
 public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ConvertTpuModelToCpuPass)
  explicit ConvertTpuModelToCpuPass() = default;

  StringRef getArgument() const final {
    // This is the argument used to refer to the pass in
    // the textual format (on the commandline for example).
    return "quant-convert-tpu-model-to-cpu";
  }
  StringRef getDescription() const final {
    return "Convert TPU models to CPU by rewriting TPU related operations.";
  }

  void runOnOperation() override;
};

class RemoveTpuOp : public RewritePattern {
 public:
  explicit RemoveTpuOp(MLIRContext* context)
      : RewritePattern(MatchAnyOpTypeTag(), /*benefit=*/1, context) {}

 private:
  LogicalResult matchAndRewrite(Operation* call_op,
                                PatternRewriter& rewriter) const override {
    if (isa<TF::TPUReplicateMetadataOp, TF::TPUCompilationResultOp,
            TF::TPUOrdinalSelectorOp>(call_op)) {
      call_op->erase();
    } else if (auto replicated_input_op =
                   dyn_cast_or_null<TF::TPUReplicatedInputOp>(call_op)) {
      // TODO(b/267700110): Handle multiple input/output cases.
      rewriter.replaceOp(replicated_input_op, replicated_input_op.getInputs());
    } else if (auto replicated_output_op =
                   dyn_cast_or_null<TF::TPUReplicatedOutputOp>(call_op)) {
      // TODO(b/267700110): Handle multiple input/output cases.
      rewriter.replaceOp(replicated_output_op, replicated_output_op.getInput());
    } else {
      return failure();
    }
    return success();
  }
};

class ReplaceTPUPartitionedCallOpWithPartitionedCallOp
    : public OpRewritePattern<TF::TPUPartitionedCallOp> {
 public:
  using OpRewritePattern<TF::TPUPartitionedCallOp>::OpRewritePattern;

 private:
  LogicalResult matchAndRewrite(TF::TPUPartitionedCallOp call_op,
                                PatternRewriter& rewriter) const override {
    auto f_attr = call_op.getFAttr().dyn_cast<FlatSymbolRefAttr>();
    auto module_op = call_op->getParentOfType<ModuleOp>();
    SymbolTable symbol_table(module_op);

    auto f_name = f_attr.getValue();
    func::FuncOp float_func =
        dyn_cast<func::FuncOp>(symbol_table.lookup(f_name));
    if (!float_func) {
      return failure();
    }
    rewriter.setInsertionPointAfter(call_op);

    // The TPUPartitionedCall has a TPUOrdinalSelectorOp for its last argument
    // which should be removed. So the replaced PartitionedCall op should keep
    // its original arguments except for the last element.
    SmallVector<Value> args = call_op.getOperands().drop_back();

    rewriter.replaceOpWithNewOp<TF::PartitionedCallOp>(
        call_op, float_func.getResultTypes(), args, f_attr);
    return success();
  }
};

void ConvertTpuModelToCpuPass::runOnOperation() {
  MLIRContext* ctx = &getContext();
  RewritePatternSet patterns(ctx);
  auto module_op = getOperation();

  patterns.add<ReplaceTPUPartitionedCallOpWithPartitionedCallOp>(ctx);
  patterns.add<RemoveTpuOp>(ctx);

  if (failed(applyPatternsAndFoldGreedily(module_op, std::move(patterns)))) {
    module_op.emitError() << "quant-internal-convert-tpu-model-to-cpu failed.";
    signalPassFailure();
  }

  // Add passes to remove the PartitionedCall op and cast bf16 ops to f32.
  PassManager pm(ctx);
  pm.addPass(createInlinerPass());
  pm.addNestedPass<func::FuncOp>(createCanonicalizerPass());
  pm.addPass(CreateCastBf16OpsToF32Pass());

  if (failed(pm.run(module_op))) {
    module_op.emitError() << "quant-internal-convert-tpu-model-to-cpu failed.";
    signalPassFailure();
  }
}

}  // namespace

// Creates an instance of the Convert TPU model to CPU pass.
std::unique_ptr<OperationPass<ModuleOp>> CreateConvertTpuModelToCpuPass() {
  return std::make_unique<ConvertTpuModelToCpuPass>();
}

static PassRegistration<ConvertTpuModelToCpuPass> pass;

}  // namespace quant
}  // namespace mlir
