/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/service/gpu/copy_thunk.h"

#include "tensorflow/compiler/xla/stream_executor/stream_executor.h"

namespace xla {
namespace gpu {

DeviceToDeviceCopyThunk::DeviceToDeviceCopyThunk(
    ThunkInfo thunk_info, const BufferAllocation::Slice& source_buffer,
    const BufferAllocation::Slice& destination_buffer, uint64_t mem_size,
    mlir::Value source_value, mlir::Value destination_value)
    : Thunk(Kind::kCopy, thunk_info),
      source_buffer_(source_buffer),
      destination_buffer_(destination_buffer),
      mem_size_(mem_size),
      source_value_(source_value),
      destination_value_(destination_value) {}

Status DeviceToDeviceCopyThunk::ExecuteOnStream(const ExecuteParams& params) {
  se::DeviceMemoryBase destination_data =
      params.buffer_allocations->GetDeviceAddress(destination_buffer_);
  se::DeviceMemoryBase source_data =
      params.buffer_allocations->GetDeviceAddress(source_buffer_);
  params.stream->ThenMemcpy(&destination_data, source_data, mem_size_);
  return OkStatus();
}
}  // namespace gpu
}  // namespace xla
