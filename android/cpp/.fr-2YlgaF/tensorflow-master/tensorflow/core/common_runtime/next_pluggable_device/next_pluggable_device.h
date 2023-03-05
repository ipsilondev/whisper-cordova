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

#ifndef TENSORFLOW_CORE_COMMON_RUNTIME_NEXT_PLUGGABLE_DEVICE_NEXT_PLUGGABLE_DEVICE_H_
#define TENSORFLOW_CORE_COMMON_RUNTIME_NEXT_PLUGGABLE_DEVICE_NEXT_PLUGGABLE_DEVICE_H_

#include <memory>
#include <string>

#include "tensorflow/core/common_runtime/local_device.h"
#include "tensorflow/core/common_runtime/next_pluggable_device/next_pluggable_device_context.h"
#include "tensorflow/core/platform/refcount.h"

namespace tensorflow {

class NextPluggableDeviceAllocator;

class NextPluggableDevice : public LocalDevice {
 public:
  struct Options {
    // The device name's prefix (e.g., "/task:7")
    string device_name_prefix;

    // The name of the  device (e.g., "GPU")
    string device_name;

    // The name of the compilation device (e.g., "XLA_TPU_JIT");
    string compilation_device_name;

    // The number of the device.
    int device_ordinal = -1;
  };

  NextPluggableDevice(const SessionOptions& session_options,
                      const Options& options);

  ~NextPluggableDevice() override;

  Allocator* GetAllocator(AllocatorAttributes attr) override;

  void Compute(OpKernel* op_kernel, OpKernelContext* context) override;

  void ComputeAsync(AsyncOpKernel* op_kernel, OpKernelContext* context,
                    AsyncOpKernel::DoneCallback done) override;

  Status Sync() override;

  void Sync(const DoneCallback& done) override;

  Status TryGetDeviceContext(DeviceContext** out_context) override;

  Status MakeTensorFromProto(const TensorProto& tensor_proto,
                             const AllocatorAttributes alloc_attrs,
                             Tensor* tensor) override;

  int GetDeviceOrdinal() const { return device_ordinal_; }

  const std::string& GetCompilationDeviceType() const {
    return compilation_device_type_;
  }

 private:
  int device_ordinal_;
  std::string compilation_device_type_;
  // Need to use RefCountPtr since DeviceContext is a ref counted object.
  core::RefCountPtr<DeviceContext> device_context_;
  std::unique_ptr<NextPluggableDeviceAllocator> allocator_;
  std::unique_ptr<DeviceBase::AcceleratorDeviceInfo> accelerator_device_info_;
};

}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_COMMON_RUNTIME_NEXT_PLUGGABLE_DEVICE_NEXT_PLUGGABLE_DEVICE_H_
