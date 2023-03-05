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

#ifndef TENSORFLOW_CORE_COMMON_RUNTIME_NEXT_PLUGGABLE_DEVICE_C_PLUGIN_COORDINATION_SERVICE_AGENT_H_
#define TENSORFLOW_CORE_COMMON_RUNTIME_NEXT_PLUGGABLE_DEVICE_C_PLUGIN_COORDINATION_SERVICE_AGENT_H_

#include <string>

#include "tensorflow/c/experimental/next_pluggable_device/c_api.h"
#include "tensorflow/c/kernels_experimental.h"
#include "tensorflow/core/common_runtime/next_pluggable_device/plugin_coordination_service_agent.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/status.h"

namespace tensorflow {

class CPluginCoordinationServiceAgent : public PluginCoordinationServiceAgent {
 public:
  explicit CPluginCoordinationServiceAgent(void* agent)
      : agent_(reinterpret_cast<TF_CoordinationServiceAgent*>(agent)) {}

  bool IsInitialized() const override {
    if (agent_ == nullptr) return false;
    return TF_CoordinationServiceIsInitialized(agent_);
  }

  Status InsertKeyValue(const std::string& key,
                        const std::string& value) override;

  StatusOr<std::string> GetKeyValue(const std::string& key) override;

  Status DeleteKeyValue(const std::string& key) override;

 private:
  TF_CoordinationServiceAgent* agent_;
};

}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_COMMON_RUNTIME_NEXT_PLUGGABLE_DEVICE_C_PLUGIN_COORDINATION_SERVICE_AGENT_H_
