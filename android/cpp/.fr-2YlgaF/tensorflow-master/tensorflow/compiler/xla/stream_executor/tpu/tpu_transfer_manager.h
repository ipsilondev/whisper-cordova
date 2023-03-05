/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_XLA_STREAM_EXECUTOR_TPU_TPU_TRANSFER_MANAGER_H_
#define TENSORFLOW_COMPILER_XLA_STREAM_EXECUTOR_TPU_TPU_TRANSFER_MANAGER_H_

#include <deque>
#include <functional>

#include "tensorflow/compiler/xla/literal.h"
#include "tensorflow/compiler/xla/service/shaped_buffer.h"
#include "tensorflow/compiler/xla/service/transfer_manager.h"
#include "tensorflow/compiler/xla/shape.h"
#include "tensorflow/compiler/xla/stream_executor/stream_executor.h"
#include "tensorflow/compiler/xla/stream_executor/tpu/tpu_executor_c_api.h"
#include "tensorflow/compiler/xla/stream_executor/tpu/tpu_transfer_manager_interface.h"

namespace tensorflow {
namespace tpu {

class TpuTransferManager : public xla::TpuTransferManagerInterface {
 public:
  TpuTransferManager();
  ~TpuTransferManager() override;

  template <typename T>
  using StatusOr = tsl::StatusOr<T>;

  stream_executor::Platform::Id PlatformId() const override;

  xla::Shape HostShapeToDeviceShape(
      const xla::Shape& host_shape) const override;

  tsl::Status TransferLiteralToDeviceAsync(
      stream_executor::Stream* stream, const xla::LiteralSlice& literal,
      const xla::ShapedBuffer& device_buffer,
      const TransferMetadata* transfer_metadata) override;

  void TransferLiteralFromDevice(
      stream_executor::Stream* stream, const xla::ShapedBuffer& device_buffer,
      xla::MutableBorrowingLiteral literal,
      std::function<void(tsl::Status)> done,
      const TransferMetadata* transfer_metadata) override;

  tsl::Status TransferLiteralToInfeed(
      stream_executor::StreamExecutor* executor,
      const xla::LiteralSlice& literal) override;

  tsl::Status TransferLiteralFromOutfeed(
      stream_executor::StreamExecutor* executor,
      xla::MutableBorrowingLiteral literal) override;

  tsl::Status TransferBuffersToInfeed(
      se::StreamExecutor* executor,
      const std::deque<tensorflow::tpu::NoncopyableBuffer>& buffers) override;

  tsl::Status ResetDevices(
      absl::Span<stream_executor::StreamExecutor* const> executor) override;

  int64_t GetByteSizeRequirement(const xla::Shape& shape) const override;

  StatusOr<xla::Shape> ChooseCompactLayoutForShape(
      const xla::Shape& host_shape) const override;

  bool CanShapedBufferBeAccessedNow(
      stream_executor::StreamExecutor* executor,
      const xla::ShapedBuffer& device_buffer) const override;

  bool CanBufferBeAccessedNow(
      se::StreamExecutor* executor,
      const se::DeviceMemoryBase& device_buffer) const override;

  tsl::Status WriteSingleTupleIndexTable(
      stream_executor::Stream* stream,
      absl::Span<const stream_executor::DeviceMemoryBase> elements,
      const xla::Shape& shape,
      stream_executor::DeviceMemoryBase* region) override;

  tsl::Status LinearizeToBuffers(
      const xla::LiteralSlice& literal,
      std::deque<tensorflow::tpu::NoncopyableBuffer>* buffers) override;

  tsl::Status ReadDynamicShapes(se::Stream* stream,
                                const xla::ShapedBuffer* device_buffer,
                                xla::Shape* device_shape) override;

 private:
  XLA_TransferManager* manager_;
};

}  // namespace tpu
}  // namespace tensorflow

#endif  // TENSORFLOW_COMPILER_XLA_STREAM_EXECUTOR_TPU_TPU_TRANSFER_MANAGER_H_
