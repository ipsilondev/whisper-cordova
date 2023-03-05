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

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_GPU_GPU_HLO_COST_ANALYSIS_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_GPU_GPU_HLO_COST_ANALYSIS_H_

#include <memory>
#include <string>

#include "tensorflow/compiler/xla/service/hlo_cost_analysis.h"

namespace xla {
namespace gpu {

// Cost analysis for GPUs.
class GpuHloCostAnalysis : public HloCostAnalysis {
  // Each instruction creating a new basic block roughly doubles the total
  // number of basic blocks and the IR code size accordingly.
  static constexpr int64_t kMaxBasicBlockSplitsPerFusion = 10;
  static constexpr int64_t kMaxIRSize = 10000;

 public:
  explicit GpuHloCostAnalysis(const Options& options)
      : HloCostAnalysis(options) {}

  Status Preprocess(const HloInstruction* hlo) override;

  Status HandleCustomCall(const HloInstruction* call) override;

  int64_t GetConvolutionFlops(const HloInstruction* convolution) override;

  Status HandleElementwiseOp(const HloInstruction* hlo);
  Status HandleElementwiseUnary(const HloInstruction* hlo) override;
  Status HandleElementwiseBinary(const HloInstruction* hlo) override;

  // Estimate the total size of IR accounting for both duplication
  // of producer code by consumer and the total number of basic blocks.
  // Tell if merged IR size would be too slow to compile.
  bool ProducerConsumerMergedTooLarge(const HloInstruction& producer,
                                      const HloInstruction& consumer);

  // IR size scale of an instruction: 1 for most instructions,
  // but for fusions is the number of instructions emitted including the
  // duplication due to non-element-wise accesses.
  float IrSize(const HloInstruction& hlo) const;

  // Total common elementwise utilization of two instructions within a fusion.
  // If two parameters have several common elementwise use roots returned is
  // the sum of these utilizations. Can also be used to query if a parameter
  // is used elementwise from the fusion's root.
  float CommonElementwiseUtilization(const HloInstruction* a,
                                     const HloInstruction* b) const;

 protected:
  std::unique_ptr<HloCostAnalysis> CreateNestedCostAnalysis() override;
  int64_t FusionParameterReadBytes(const HloInstruction* hlo) const override;
  Status FusionCalculateUtilizations(const HloInstruction* fusion) override;

  size_t immediate_constant_max_elements() const override { return 8; }

  bool KeyToCopyFromSubcomputation(absl::string_view key) const override;

  // Some instructions create new LLVM basic blocks; with our current code
  // generation this means in the worst case doubling the IR size of a fusion
  // containing such an instruction.
  // Count these to avoid unmanageable IR code size.
  float IrBasicBlockSplitCount(const HloInstruction& hlo) const;

  // To estimate where within the computation an instruction output can be
  // reused and where it has to be recomputed again we group accesses to the
  // instruction by their origin from "element-wise use roots". All access
  // paths from such a root to the instruction are element-wise.
  absl::flat_hash_map<const HloInstruction*,
                      absl::flat_hash_set<const HloInstruction*>>
      elementwise_use_roots_;

  // Elementwise utilization of instruction's input subtree if it is a root.
  // This is different from hlo_properties_[instr][kUtilizationKey] which
  // is the utilization of the instruction by other roots.
  absl::flat_hash_map<const HloInstruction*, float> root_utilizations_;
};

}  // namespace gpu
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_GPU_GPU_HLO_COST_ANALYSIS_H_
