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

#include "tensorflow/compiler/xla/service/gpu/cublas_cudnn.h"

#include "absl/strings/string_view.h"

namespace xla {
namespace gpu {

bool IsCublasGemm(const HloInstruction& hlo) {
  return IsLegacyCublasMatmul(hlo) || IsCublasLtMatmul(hlo);
}

bool IsLegacyCublasMatmul(const HloInstruction& hlo) {
  return hlo.opcode() == HloOpcode::kCustomCall &&
         hlo.custom_call_target() == kGemmCallTarget;
}

bool IsCublasLtMatmul(const HloInstruction& hlo) {
  return hlo.opcode() == HloOpcode::kCustomCall &&
         hlo.custom_call_target() == kCublasLtMatmulCallTarget;
}

bool IsCublasLtMatmulF8(const HloInstruction& hlo) {
  return hlo.opcode() == HloOpcode::kCustomCall &&
         hlo.custom_call_target() == kCublasLtMatmulF8CallTarget;
}

const absl::string_view kGemmCallTarget = "__cublas$gemm";
const absl::string_view kCublasLtMatmulCallTarget = "__cublas$lt$matmul";
const absl::string_view kCublasLtMatmulF8CallTarget = "__cublas$lt$matmul$f8";
const absl::string_view kTriangularSolveCallTarget = "__cublas$triangularSolve";

const absl::string_view kCudnnConvBackwardInputCallTarget =
    "__cudnn$convBackwardInput";
const absl::string_view kCudnnConvBackwardFilterCallTarget =
    "__cudnn$convBackwardFilter";
const absl::string_view kCudnnConvBiasActivationForwardCallTarget =
    "__cudnn$convBiasActivationForward";
const absl::string_view kCudnnConvForwardCallTarget = "__cudnn$convForward";
const absl::string_view kCudnnConvReorderFilterCallTarget =
    "__cudnn$convReorderFilter";
const absl::string_view kCudnnConvReorderFilterAndBiasCallTarget =
    "__cudnn$convReorderFilterAndBias";

bool IsCustomCallToDnnConvolution(const HloInstruction& hlo) {
  if (hlo.opcode() != HloOpcode::kCustomCall) {
    return false;
  }
  const auto& target = hlo.custom_call_target();
  return target == kCudnnConvForwardCallTarget ||
         target == kCudnnConvBackwardInputCallTarget ||
         target == kCudnnConvBackwardFilterCallTarget ||
         target == kCudnnConvBiasActivationForwardCallTarget;
}

bool IsCudnnConvolutionReorder(const HloInstruction& hlo) {
  if (hlo.opcode() != HloOpcode::kCustomCall) {
    return false;
  }
  const auto& target = hlo.custom_call_target();
  return target == kCudnnConvReorderFilterCallTarget ||
         target == kCudnnConvReorderFilterAndBiasCallTarget;
}

StatusOr<CudnnConvKind> GetCudnnConvKind(
    const HloCustomCallInstruction* instr) {
  absl::string_view target = instr->custom_call_target();
  if (target == kCudnnConvForwardCallTarget) {
    return CudnnConvKind::kForward;
  }
  if (target == kCudnnConvBackwardInputCallTarget) {
    return CudnnConvKind::kBackwardInput;
  }
  if (target == kCudnnConvBackwardFilterCallTarget) {
    return CudnnConvKind::kBackwardFilter;
  }
  if (target == kCudnnConvBiasActivationForwardCallTarget) {
    return CudnnConvKind::kForwardActivation;
  }
  return InternalError("Unexpected call target: %s", target);
}

std::string CudnnConvKindToString(CudnnConvKind kind) {
  switch (kind) {
    case CudnnConvKind::kForward:
      return "forward";
    case CudnnConvKind::kBackwardFilter:
      return "backward_filter";
    case CudnnConvKind::kBackwardInput:
      return "backward_input";
    case CudnnConvKind::kForwardActivation:
      return "forward with activation";
  }
}

}  // namespace gpu
}  // namespace xla
