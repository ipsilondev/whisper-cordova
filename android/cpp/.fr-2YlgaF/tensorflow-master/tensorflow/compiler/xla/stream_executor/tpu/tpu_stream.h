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

#ifndef TENSORFLOW_COMPILER_XLA_STREAM_EXECUTOR_TPU_TPU_STREAM_H_
#define TENSORFLOW_COMPILER_XLA_STREAM_EXECUTOR_TPU_TPU_STREAM_H_

#include "tensorflow/compiler/xla/stream_executor/stream_executor_internal.h"
#include "tensorflow/compiler/xla/stream_executor/tpu/c_api_conversions.h"
#include "tensorflow/compiler/xla/stream_executor/tpu/status_helper.h"
#include "tensorflow/compiler/xla/stream_executor/tpu/tpu_api.h"
#include "tensorflow/compiler/xla/stream_executor/tpu/tpu_executor_c_api.h"
#include "tensorflow/compiler/xla/stream_executor/tpu/tpu_stream_interface.h"

namespace tensorflow {
namespace tpu {

class TpuStream : public tensorflow::tpu::TpuStreamInterface {
 public:
  explicit TpuStream(SE_Stream* stream) : stream_(stream) {}
  ~TpuStream() override {
    stream_executor::tpu::ExecutorApiFn()->TpuStream_FreeFn(stream_);
  }

  bool IsSameSharedMemoryLocation(
      tensorflow::tpu::TpuStreamInterface* other) override {
    return stream_executor::tpu::ExecutorApiFn()
        ->TpuStream_IsSameSharedMemoryLocationFn(
            stream_, static_cast<TpuStream*>(other)->stream_);
  }

  tsl::Status EnqueueTransferHostToDevice(
      stream_executor::DeviceMemoryBase device_dst, const void* host_src,
      uint64_t size) {
    StatusHelper status;
    stream_executor::tpu::ExecutorApiFn()
        ->TpuStream_EnqueueTransferHostToDeviceFn(
            stream_, ApiConverter::ToC(device_dst), const_cast<void*>(host_src),
            size, status.c_status);
    return status.status();
  }

  tsl::Status EnqueueTransferDeviceToHost(
      stream_executor::DeviceMemoryBase device_src, void* host_dst,
      uint64_t size) {
    StatusHelper status;
    stream_executor::tpu::ExecutorApiFn()
        ->TpuStream_EnqueueTransferDeviceToHostFn(
            stream_, ApiConverter::ToC(device_src), host_dst, size,
            status.c_status);
    return status.status();
  }

  tsl::Status EnqueueOnTpuDeviceSendRecvLocal(
      stream_executor::DeviceMemoryBase send_buffer,
      stream_executor::DeviceMemoryBase recv_buffer) override {
    StatusHelper status;
    stream_executor::tpu::ExecutorApiFn()
        ->TpuStream_TpuEnqueueOnDeviceSendRecvLocalFn(
            stream_, ApiConverter::ToC(send_buffer),
            ApiConverter::ToC(recv_buffer), status.c_status);
    return status.status();
  }

  SE_Stream* se_stream() const { return stream_; }

 private:
  mutable SE_Stream* stream_;
};

}  // namespace tpu
}  // namespace tensorflow

#endif  // TENSORFLOW_COMPILER_XLA_STREAM_EXECUTOR_TPU_TPU_STREAM_H_
