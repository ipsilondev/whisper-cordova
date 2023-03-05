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

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tensorflow/compiler/xla/backends/profiler/cpu/metadata_utils.h"
#include "tensorflow/compiler/xla/service/hlo.pb.h"
#include "tensorflow/compiler/xla/service/xla_debug_info_manager.h"
#include "tensorflow/tsl/platform/macros.h"
#include "tensorflow/tsl/platform/status.h"
#include "tensorflow/tsl/profiler/lib/profiler_factory.h"
#include "tensorflow/tsl/profiler/lib/profiler_interface.h"
#include "tensorflow/tsl/profiler/protobuf/profiler_options.pb.h"
#include "tensorflow/tsl/profiler/protobuf/xplane.pb.h"
#include "tensorflow/tsl/profiler/utils/xplane_schema.h"
#include "tensorflow/tsl/profiler/utils/xplane_utils.h"

namespace xla {
namespace profiler {
namespace {

// MetadataCollector collect miscellaneous metadata for xprof, e.g. HLO protos
// from XLA runtime etc.
//
// Thread-safety: This class is go/thread-compatible.
class MetadataCollector : public tsl::profiler::ProfilerInterface {
 public:
  MetadataCollector() = default;

  Status Start() override {
    if (!trace_active_) {
      xla::XlaDebugInfoManager::Get()->StartTracing();
      trace_active_ = true;
    }
    return OkStatus();
  }

  Status Stop() override {
    if (trace_active_) {
      xla::XlaDebugInfoManager::Get()->StopTracing(&debug_info_);
      trace_active_ = false;
    }
    return OkStatus();
  }

  Status CollectData(tsl::profiler::XSpace* space) override {
    if (!debug_info_.empty()) {
      tsl::profiler::XPlane* plane =
          tsl::profiler::FindOrAddMutablePlaneWithName(
              space, tsl::profiler::kMetadataPlaneName);
      MetadataXPlaneBuilder metadata_plane(plane);
      for (auto& hlo_proto : debug_info_) {
        metadata_plane.AddHloProto(hlo_proto->hlo_module().id(), *hlo_proto);
        hlo_proto.reset();
      }
      debug_info_.clear();
    }
    return OkStatus();
  }

 private:
  std::vector<std::unique_ptr<xla::HloProto>> debug_info_;
  bool trace_active_ = false;

  TF_DISALLOW_COPY_AND_ASSIGN(MetadataCollector);
};

std::unique_ptr<tsl::profiler::ProfilerInterface> CreatMetadataCollector(
    const tensorflow::ProfileOptions& options) {
  return options.enable_hlo_proto() ? std::make_unique<MetadataCollector>()
                                    : nullptr;
}

}  // namespace

auto register_metadata_collector_factory = [] {
  RegisterProfilerFactory(&CreatMetadataCollector);
  return 0;
}();

}  // namespace profiler
}  // namespace xla
