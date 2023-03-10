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

#ifndef TENSORFLOW_COMPILER_JIT_PJRT_DEVICE_CONTEXT_H_
#define TENSORFLOW_COMPILER_JIT_PJRT_DEVICE_CONTEXT_H_

#include <memory>

#include "tensorflow/compiler/xla/pjrt/pjrt_client.h"
#include "tensorflow/core/framework/device_base.h"
#include "tensorflow/core/platform/status.h"

namespace tensorflow {

// Helper class for managing data transfers between host and accelerator
// devices using PjRt.
class PjRtDeviceContext : public DeviceContext {
 public:
  void CopyCPUTensorToDevice(const Tensor* cpu_tensor, Device* device,
                             Tensor* device_tensor, StatusCallback done,
                             bool sync_dst_compute) const override;
  void CopyDeviceTensorToCPU(const Tensor* device_tensor,
                             absl::string_view tensor_name, Device* device,
                             Tensor* cpu_tensor, StatusCallback done) override;
  void CopyTensorInSameDevice(const Tensor* input_tensor, Device* device,
                              Tensor* output_tensor,
                              StatusCallback done) const override;
};

}  // namespace tensorflow

#endif  // TENSORFLOW_COMPILER_JIT_PJRT_DEVICE_CONTEXT_H_
