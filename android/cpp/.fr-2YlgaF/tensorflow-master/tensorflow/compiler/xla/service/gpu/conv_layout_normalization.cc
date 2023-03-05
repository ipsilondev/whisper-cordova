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

#include "tensorflow/compiler/xla/service/gpu/conv_layout_normalization.h"

#include <optional>
#include <tuple>
#include <vector>

#include "tensorflow/compiler/xla/hlo/ir/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/hlo/ir/hlo_instruction.h"
#include "tensorflow/compiler/xla/hlo/ir/hlo_instructions.h"
#include "tensorflow/compiler/xla/hlo/ir/hlo_module.h"
#include "tensorflow/compiler/xla/layout_util.h"
#include "tensorflow/compiler/xla/service/gpu/cublas_cudnn.h"
#include "tensorflow/compiler/xla/service/hlo_creation_utils.h"
#include "tensorflow/compiler/xla/shape_util.h"

namespace xla {
namespace gpu {
namespace {

StatusOr<HloInstruction*> UpdateLayoutForCudnnConvolution(
    HloCustomCallInstruction* hlo) {
  HloInstruction* lhs = hlo->mutable_operand(0);
  HloInstruction* rhs = hlo->mutable_operand(1);
  const ConvolutionDimensionNumbers& dim_numbers =
      hlo->convolution_dimension_numbers();

  auto transpose_dim = [&](int64_t dim, const Shape& unnormalized_shape) {
    return unnormalized_shape.rank() -
           FindIndex(unnormalized_shape.layout().minor_to_major(), dim) - 1;
  };

  auto transpose_dims = [&](tsl::protobuf::RepeatedField<int64_t>& dims,
                            const Shape& unnormalized_shape) {
    for (auto& dim : dims) {
      dim = transpose_dim(dim, unnormalized_shape);
    }
  };

  const Shape& conv_output_shape =
      hlo->shape().IsTuple() ? hlo->shape().tuple_shapes(0) : hlo->shape();

  Shape input_shape, filter_shape, output_shape;
  TF_ASSIGN_OR_RETURN(
      gpu::CudnnConvKind conv_kind,
      gpu::GetCudnnConvKind(Cast<HloCustomCallInstruction>(hlo)));
  switch (conv_kind) {
    case gpu::CudnnConvKind::kForward:
    case gpu::CudnnConvKind::kForwardActivation: {
      input_shape = lhs->shape();
      filter_shape = rhs->shape();
      output_shape = conv_output_shape;
      break;
    }
    case gpu::CudnnConvKind::kBackwardInput: {
      filter_shape = rhs->shape();
      output_shape = lhs->shape();
      input_shape = conv_output_shape;
      break;
    }
    case gpu::CudnnConvKind::kBackwardFilter: {
      input_shape = lhs->shape();
      output_shape = rhs->shape();
      filter_shape = conv_output_shape;
      break;
    }
  }

  ConvolutionDimensionNumbers new_dim_numbers = dim_numbers;
  new_dim_numbers.set_input_batch_dimension(
      transpose_dim(dim_numbers.input_batch_dimension(), input_shape));
  new_dim_numbers.set_input_feature_dimension(
      transpose_dim(dim_numbers.input_feature_dimension(), input_shape));
  transpose_dims(*new_dim_numbers.mutable_input_spatial_dimensions(),
                 input_shape);

  new_dim_numbers.set_kernel_input_feature_dimension(transpose_dim(
      dim_numbers.kernel_input_feature_dimension(), filter_shape));
  new_dim_numbers.set_kernel_output_feature_dimension(transpose_dim(
      dim_numbers.kernel_output_feature_dimension(), filter_shape));
  transpose_dims(*new_dim_numbers.mutable_kernel_spatial_dimensions(),
                 filter_shape);

  new_dim_numbers.set_output_batch_dimension(
      transpose_dim(dim_numbers.output_batch_dimension(), output_shape));
  new_dim_numbers.set_output_feature_dimension(
      transpose_dim(dim_numbers.output_feature_dimension(), output_shape));
  transpose_dims(*new_dim_numbers.mutable_output_spatial_dimensions(),
                 output_shape);

  Shape normalized_shape;
  if (hlo->shape().IsTuple()) {
    TF_RET_CHECK(hlo->shape().tuple_shapes_size() == 2);
    TF_RET_CHECK(hlo->shape().tuple_shapes(1).rank() == 1)
        << "Second element in a convolution tuple is expected to be an "
           "allocator of rank one";
    normalized_shape = ShapeUtil::MakeTupleShape(
        {ShapeUtil::MakeShapeWithDescendingLayoutAndSamePhysicalLayout(
             hlo->shape().tuple_shapes(0)),
         hlo->shape().tuple_shapes(1)});
  } else {
    normalized_shape =
        ShapeUtil::MakeShapeWithDescendingLayoutAndSamePhysicalLayout(
            hlo->shape());
  }

  // We need to restore degenerate dimensions, since those might be used in
  // either batch dimension, or contracting dimensions.
  std::vector<HloInstruction*> normalized_operands;
  for (int idx = 0; idx < hlo->operand_count(); idx++) {
    HloInstruction* op = hlo->mutable_operand(idx);
    const Shape& s = op->shape();
    Shape s_reordered =
        ShapeUtil::MakeShapeWithDescendingLayoutAndSamePhysicalLayout(s);
    HloInstruction* normalized_op = op->mutable_operand(0);
    HloInstruction* new_op;
    if (normalized_op->shape() == s_reordered) {
      new_op = normalized_op;
    } else {
      new_op = MakeBitcastHlo(op, s_reordered);
    }
    normalized_operands.push_back(new_op);
  }

  HloInstruction* normalized_conv = hlo->parent()->AddInstruction(
      HloInstruction::CreateCustomCall(normalized_shape, normalized_operands,
                                       hlo->custom_call_target()),
      &hlo->metadata());

  normalized_conv->set_window(hlo->window());
  normalized_conv->set_convolution_dimension_numbers(new_dim_numbers);
  normalized_conv->set_feature_group_count(hlo->feature_group_count());
  normalized_conv->set_raw_backend_config_string(
      hlo->raw_backend_config_string());
  normalized_conv->parent()->parent()->SetAndUniquifyInstrName(normalized_conv,
                                                               hlo->name());

  // We are hoping that AlgebraicSimplifier will simplify the extraneous
  // tuples built this way.
  HloInstruction* bc_to_orig;
  if (normalized_conv->shape().IsTuple()) {
    TF_ASSIGN_OR_RETURN(HloInstruction * normalized_out,
                        MakeGetTupleElementHlo(normalized_conv, 0));
    TF_ASSIGN_OR_RETURN(HloInstruction * allocator,
                        MakeGetTupleElementHlo(normalized_conv, 1));
    HloInstruction* orig_shape_out =
        MakeBitcastHlo(normalized_out, hlo->shape().tuple_shapes(0));
    bc_to_orig = MaybeMakeTuple({orig_shape_out, allocator});
  } else {
    bc_to_orig = MakeBitcastHlo(normalized_conv, hlo->shape());
  }
  return bc_to_orig;
}

// Create an instruction sequence (reshape-transpose-reshape) that effectively
// does the same thing as cudnnReorderFilterAndBias, but could also be constant
// folded or fused.
HloInstruction* CreateTransposeForCudnnFilterReordering(HloInstruction* hlo,
                                                        const Shape& shape) {
  // Filter shape is [O, I / 32, H, W, 32]
  CHECK_EQ(shape.rank(), 5);
  CHECK_EQ(shape.dimensions(0) % 32, 0);
  CHECK_EQ(shape.dimensions(4), 32);

  auto [O, I, H, W] = std::tuple(shape.dimensions(0), shape.dimensions(1),
                                 shape.dimensions(2), shape.dimensions(3));
  Shape shape_bitcast =
      ShapeUtil::MakeShape(shape.element_type(), {O / 8, 4, 2, I, H, W, 8, 4});
  Shape shape_transpose = ShapeUtil::MakeShape(
      shape.element_type(), {I, H, W, O / 8, 2, 8, /*output*/ 4, /*input*/ 4});

  // The permutation is reverse engineered from the cudnn v8.3 implementation
  // (see go/xla-int8x32-cudnn-frontend)
  HloInstruction* bitcast_1 =
      hlo->AddInstruction(HloInstruction::CreateBitcast(shape_bitcast, hlo));
  HloInstruction* transpose =
      hlo->AddInstruction(HloInstruction::CreateTranspose(
          shape_transpose, bitcast_1, {3, 4, 5, 0, 2, 6, 1, 7}));
  HloInstruction* bitcast_2 =
      hlo->AddInstruction(HloInstruction::CreateBitcast(shape, transpose));
  return bitcast_2;
}

// Implement bias reordering, similar to the filter reordering.
HloInstruction* CreateTransposeForCudnnBiasReordering(HloInstruction* hlo,
                                                      const Shape& shape) {
  CHECK_EQ(shape.rank(), 1);
  CHECK_EQ(shape.dimensions(0), 32);

  auto N = shape.dimensions(0);
  Shape shape_bitcast =
      ShapeUtil::MakeShape(shape.element_type(), {N / 32, 4, 2, 4});
  Shape shape_transpose =
      ShapeUtil::MakeShape(shape.element_type(), {N / 32, 2, 4, 4});

  HloInstruction* bitcast_1 =
      hlo->AddInstruction(HloInstruction::CreateBitcast(shape_bitcast, hlo));
  HloInstruction* transpose =
      hlo->AddInstruction(HloInstruction::CreateTranspose(
          shape_transpose, bitcast_1, {0, 2, 1, 3}));
  HloInstruction* bitcast_2 =
      hlo->AddInstruction(HloInstruction::CreateBitcast(shape, transpose));
  return bitcast_2;
}

// Normalize the layout of cuDNN int8x32 filter reordering custom call
// (implemented by calling `cudnnReorderFilterAndBias`), which should be
// followed by a convolution.
// Both the input and the output shape for the filter operand must have the
// NCHW_VECT_C layout.
HloInstruction* UpdateLayoutForCudnnConvolutionReordering(
    HloCustomCallInstruction* hlo) {
  // The custom call may have either one (filter) or two (filter and bias)
  // operands. The number of outputs matches the number of inputs.
  Shape const* filter_shape;
  Shape const* bias_shape;
  std::tie(filter_shape, bias_shape) =
      hlo->shape().IsTuple() ? std::make_tuple(&hlo->shape().tuple_shapes(0),
                                               &hlo->shape().tuple_shapes(1))
                             : std::make_tuple(&hlo->shape(), nullptr);

  // Transpose the filter to match the expected layout (NCHW_VECT_C).
  // This bias is 1D, so the shape doesn't need to be updated.
  auto new_filter_shape =
      ShapeUtil::MakeShapeWithDescendingLayoutAndSamePhysicalLayout(
          *filter_shape);
  auto dimensions = LayoutUtil::MakeLayoutFromMajorToMinor(
      filter_shape->layout().minor_to_major());
  HloInstruction* transpose = hlo->AddInstruction(
      HloInstruction::CreateTranspose(new_filter_shape, hlo->mutable_operand(0),
                                      dimensions.minor_to_major()));

  // Create a replacement custom-call with layout-normalized inputs.
  HloInstruction* result;
  if (bias_shape != nullptr) {
    result = MaybeMakeTuple(
        {CreateTransposeForCudnnFilterReordering(transpose, new_filter_shape),
         CreateTransposeForCudnnBiasReordering(hlo->mutable_operand(1),
                                               *bias_shape)});
  } else {
    result =
        CreateTransposeForCudnnFilterReordering(transpose, new_filter_shape);
  }
  return MakeBitcastHlo(result, hlo->shape());
}

}  // namespace

StatusOr<std::optional<HloInstruction*>> NormalizeLayoutForGpuCustomCalls(
    HloCustomCallInstruction* hlo) {
  if (IsCustomCallToDnnConvolution(*hlo)) {
    TF_ASSIGN_OR_RETURN(HloInstruction * bc_to_orig,
                        UpdateLayoutForCudnnConvolution(hlo));
    return std::make_optional(bc_to_orig);
  }
  if (IsCudnnConvolutionReorder(*hlo)) {
    return std::make_optional(UpdateLayoutForCudnnConvolutionReordering(hlo));
  }
  return {std::nullopt};
}

}  // end namespace gpu
}  // end namespace xla
