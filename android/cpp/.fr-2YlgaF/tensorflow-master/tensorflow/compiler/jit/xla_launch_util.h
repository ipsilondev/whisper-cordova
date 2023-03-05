/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

// Contains utilities for launching compiled XLA kernels for a KernelContext.

#ifndef TENSORFLOW_COMPILER_JIT_XLA_LAUNCH_UTIL_H_
#define TENSORFLOW_COMPILER_JIT_XLA_LAUNCH_UTIL_H_

#include <map>
#include <set>
#include <vector>

#include "tensorflow/compiler/jit/variable_info.h"
#include "tensorflow/compiler/jit/xla_tensor.h"
#include "tensorflow/compiler/tf2xla/xla_compiler.h"
#include "tensorflow/compiler/xla/client/local_client.h"
#include "tensorflow/compiler/xla/service/shaped_buffer.h"
#include "tensorflow/compiler/xla/stream_executor/device_memory_allocator.h"
#include "tensorflow/core/framework/allocation_description.pb.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/thread_annotations.h"

namespace tensorflow {

// Creates a list of updated resource variables.
StatusOr<std::vector<VariableInfo>> GatherVariableInfo(
    OpKernelContext* ctx,
    const XlaCompiler::CompilationResult& compilation_result,
    int missing_ctx_input_prefix);

// Returns pointers to inputs stored in `ctx`.
std::vector<const Tensor*> InputsFromContext(OpKernelContext* ctx);

StatusOr<std::vector<int>> GetConstantInputIndicesFromContext(
    OpKernelContext* ctx);

Status SetOutputForConstant(
    OpKernelContext* ctx, bool requires_copy_to_device,
    const XlaCompiler::CompilationResult* compilation_result, int output_num);

// Helper class to perform the marshalling of TensorFlow inputs and outputs to
// ShapedBuffers suitable for passing to an XLA computation.
class XlaComputationLaunchContext {
 public:
  // Create a new launch context. 'allocate_xla_tensors' is true if allocated
  // output tensors and variables are always XlaTensors. If false they are
  // assumed to be "normal" device pointers.
  // If 'use_multiple_streams' is true, tensors may be defined and used on
  // multiple streams and so se::Events must be defined and waited for. If
  // 'use_multiple_streams' is true, 'allocate_xla_tensors' must also be true
  // because we track inter-stream dependencies through events inside XlaTensor
  // objects.
  XlaComputationLaunchContext(xla::LocalClient* client,
                              se::DeviceMemoryAllocator* xla_allocator,
                              int device_ordinal, bool allocate_xla_tensors,
                              bool use_multiple_streams);

  // Builds a XlaCompiler::Argument vector from the arguments to an XlaLaunch
  // op.
  // Precondition: variables in `variable_args` are locked.
  static StatusOr<std::vector<XlaCompiler::Argument>> BuildXlaCompilerArguments(
      absl::Span<int const> must_be_constant_idxs,
      absl::Span<const Tensor* const> inputs,
      absl::Span<VariableInfo const> variable_args, Device* device);

  // Add all inputs within `ctx` as XLA arguments (returned by arguments()).
  // `variables` is a map from TensorFlow argument number to resource variable.
  //
  // Assumes that the first `missing_ctx_input_prefix` inputs to the kernel are
  // missing and adjusts input indices accordingly.  All elements in kernel's
  // input_mapping must be greater than or equal to `missing_ctx_input_prefix`
  // (in other words, no inputs actually required by the kernel can be missing).
  StatusOr<std::vector<xla::ExecutionInput>> PopulateInputs(
      OpKernelContext* ctx,
      const XlaCompiler::CompilationResult* compilation_result,
      const std::map<int, const Tensor*>& resource_vars,
      int missing_ctx_input_prefix,
      const xla::HloInputOutputAliasConfig& input_output_alias);

  // Given the XLA output in `output`, populate all outputs of `ctx`.  Also
  // writes out the resource variable updates.
  //
  // Updates to all resource variables are written in a single atomic operation.
  // This models *->Write dependencies between resource variable operations.
  // See jit/resource_operation_safety_analysis for details.
  //
  //
  // Assumes that the first `missing_ctx_input_prefix` inputs to the
  // compilation_result are missing and adjusts input indices accordingly.
  Status PopulateOutputs(
      OpKernelContext* ctx,
      const XlaCompiler::CompilationResult* compilation_result,
      xla::ScopedShapedBuffer output, int missing_ctx_input_prefix,
      absl::Span<VariableInfo> variable_infos,
      const xla::HloInputOutputAliasConfig& input_output_alias,
      const std::map<int, const Tensor*>& resource_vars);

 private:
  xla::LocalClient* client_;
  se::DeviceMemoryAllocator* xla_allocator_;
  bool allocate_xla_tensors_;
  bool use_multiple_streams_;
  int device_ordinal_;
};

// A simple TensorBuffer implementation that allows us to create Tensors that
// take ownership of pre-allocated memory.
class XlaTensorBuffer : public TensorBuffer {
 public:
  XlaTensorBuffer(const void* ptr, size_t expected_size, size_t actual_size,
                  Allocator* allocator)
      : TensorBuffer(const_cast<void*>(ptr)),
        expected_size_(expected_size),
        actual_size_(actual_size),
        allocator_(allocator) {}

  ~XlaTensorBuffer() override {
    if (data()) {
      allocator_->DeallocateRaw(data());
    }
  }

  size_t size() const override { return expected_size_; }

  TensorBuffer* root_buffer() override { return this; }

  void FillAllocationDescription(AllocationDescription* proto) const override {
    proto->set_requested_bytes(static_cast<int64_t>(expected_size_));
    proto->set_allocator_name(allocator_->Name());
    proto->set_ptr(reinterpret_cast<uintptr_t>(data()));
    if (allocator_->TracksAllocationSizes()) {
      auto ab = static_cast<int64_t>(allocator_->AllocatedSize(data()));
      proto->set_allocated_bytes(ab);
      int64_t id = allocator_->AllocationId(data());
      if (id > 0) {
        proto->set_allocation_id(id);
      }
      if (RefCountIsOne()) {
        proto->set_has_single_reference(true);
      }
    }
  }

 private:
  size_t expected_size_;
  size_t actual_size_;
  Allocator* allocator_;
};

}  // namespace tensorflow

#endif  // TENSORFLOW_COMPILER_JIT_XLA_LAUNCH_UTIL_H_
