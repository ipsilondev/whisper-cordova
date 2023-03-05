/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_MLIR_TFRT_JIT_TF_JITRT_PIPELINE_H_
#define TENSORFLOW_COMPILER_MLIR_TFRT_JIT_TF_JITRT_PIPELINE_H_

#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassOptions.h"
#include "llvm/ADT/Hashing.h"
#include "tensorflow/compiler/xla/runtime/compiler.h"

namespace tensorflow {

struct TfJitRtPipelineOptions
    : public mlir::PassPipelineOptions<TfJitRtPipelineOptions> {
  Option<bool> vectorize{*this, "vectorize",
                         llvm::cl::desc("Enable tiling for vectorization."),
                         llvm::cl::init(false)};

  ListOption<int64_t> matmul_tile_sizes{
      *this, "matmul-tile-sizes",
      llvm::cl::desc("Tile sizes for `linalg.matmul`. Leave empty to determine "
                     "sizes automatically."),
      llvm::cl::list_init<int64_t>({}), llvm::cl::ZeroOrMore};

  Option<bool> lower_to_mmt4d{
      *this, "lower-to-mmt4d",
      llvm::cl::desc("Enable the specific code generation (packing) for matmul "
                     "operations."),
      llvm::cl::init(false)};

  Option<bool> legalize_i1_tensors{
      *this, "legalize-i1-tensors",
      llvm::cl::desc("Convert i1 tensors to i8 tensors."),
      llvm::cl::init(false)};
};

// Make TfJitRtPipelineOptions hashable.
inline ::llvm::hash_code hash_value(const TfJitRtPipelineOptions& opts) {
  return ::llvm::hash_combine(
      opts.vectorize.getValue(),
      static_cast<llvm::ArrayRef<int64_t>>(opts.matmul_tile_sizes),
      opts.lower_to_mmt4d.getValue(), opts.legalize_i1_tensors.getValue());
}

// Creates a pipeline that lowers modules from the Tensorflow dialect to
// the Linalg on buffers. `TfJitRtPipelineOptions` contains flags to
// enable/disable experimental features.
void CreateTfJitRtPipeline(mlir::OpPassManager& pm,
                           const TfJitRtPipelineOptions& options);

// Calls CreateTfJitRtPipeline with the default TfJitRtPipelineOptions.
void CreateDefaultTfJitRtPipeline(mlir::OpPassManager& pm);

// Creates a pipeline that runs on compiled module specialization. It runs the
// Tensorflow shape inference and canonicalization, so that specialized function
// always has ranked inputs and results to infer JitRt ABI requirements.
void CreateJitRtSpecializationPipeline(xla::runtime::PassManager& passes);

}  // namespace tensorflow

#endif  // TENSORFLOW_COMPILER_MLIR_TFRT_JIT_TF_JITRT_PIPELINE_H_
