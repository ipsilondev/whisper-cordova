/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/tf2xla/mlir_bridge_pass.h"

#include <string>

#include "tensorflow/compiler/mlir/tf2xla/mlir_bridge_rollout_policy.h"
#include "absl/base/call_once.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_structs.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/bridge.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/device_util.h"
#include "tensorflow/compiler/tf2xla/tf2xla_defs.h"
#include "tensorflow/core/common_runtime/device_set.h"
#include "tensorflow/core/framework/metrics.h"
#include "tensorflow/core/lib/monitoring/gauge.h"
#include "tensorflow/core/public/session_options.h"
#include "tensorflow/core/util/device_name_utils.h"

namespace tensorflow {

auto* mlir_bridge_gauge_v1 = monitoring::Gauge<bool, 0>::New(
    "/tensorflow/config/experimental/enable_mlir_bridge_gauge_v1",
    "Tracks usage of the MLIR-based TF2XLA bridge among TF1 models");
auto* mlir_bridge_gauge_v2 = monitoring::Gauge<bool, 0>::New(
    "/tensorflow/config/experimental/enable_mlir_bridge_gauge_v2",
    "Tracks usage of the MLIR-based TF2XLA bridge among TF2 models");

namespace {

bool HasTPUDevice(mlir::ModuleOp module) {
  mlir::TF::RuntimeDevices devices;
  if (failed(GetDevicesFromOp(module.getOperation(), &devices))) return false;
  return absl::c_any_of(
      devices.device_names(),
      [](const tensorflow::DeviceNameUtils::ParsedName& device) {
        return device.has_type && device.type == kTpuDevice;
      });
}

bool HasTPUOp(mlir::ModuleOp module) {
  auto walk_result = module.walk([&](mlir::Operation* op) {
    // Check for ops with compile device type "TPU". This allows us to support
    // TPU compilation without replication. Note that currently the compile
    // device type is not set by default before bridge, only if eager context
    // attribute `jit_compile_rewrite` is true.
    // TODO(b/229028654): Remove string conversion once we have C++17.
    const llvm::StringRef compile_device_type_attr_name(
        kCompileDeviceTypeAttr.data(), kCompileDeviceTypeAttr.size());
    auto compilation_attr =
        op->getAttrOfType<mlir::StringAttr>(compile_device_type_attr_name);
    if (compilation_attr && compilation_attr.getValue().str() == kTpuDevice) {
      return mlir::WalkResult::interrupt();
    }
    // TODO(b/223677572): Once the scope for new compilation and replication
    // markers is expanded beyond bridge we can remove this check for
    // `kTPUReplicateAttr`, we will then always have a `kCompileDeviceTypeAttr`
    // in such cases (see above).
    // TODO(b/229028654): Remove string conversion once we have C++17.
    const llvm::StringRef tpu_replicate_attr_name(kTpuReplicateAttr.data(),
                                                  kTpuReplicateAttr.size());
    auto replicate_attr =
        op->getAttrOfType<mlir::StringAttr>(tpu_replicate_attr_name);
    if (replicate_attr) return mlir::WalkResult::interrupt();
    return mlir::WalkResult::advance();
  });
  return walk_result.wasInterrupted();
}

// Checks that the module has both TPU devices in its device list and contains
// TPU ops.
bool HasTPUDevicesAndOps(mlir::ModuleOp module) {
  return HasTPUDevice(module) && HasTPUOp(module);
}

bool HasTPUDevice(const DeviceSet& device_set) {
  for (const Device* device : device_set.devices()) {
    if (!device) continue;
    const DeviceNameUtils::ParsedName& name = device->parsed_name();
    if (name.has_type && name.type == "TPU") return true;
  }
  return false;
}

// Check if the `graph` has parameter serverjobs and resource variable arguments
// that are on parameter servers
bool HasPsWithResourceVariable(const Graph& graph) {
  // Check parameter serverjobs and resource variable arguments that are
  // on parameter servers.
  const std::string jobType = "ps";
  const std::string nodeType = "_Arg";
  const std::string attrKey = "T";
  for (const Node* node : graph.nodes()) {
    if (node->type_string() == nodeType) {
      auto device_name = node->assigned_device_name();
      DeviceNameUtils::ParsedName device;
      if (DeviceNameUtils::ParseFullName(device_name, &device) &&
          device.has_job && device.job == jobType) {
        for (const auto& attr : node->attrs()) {
          auto attr_key = attr.first;
          auto attr_value = attr.second;
          if (attr_key == attrKey &&
              attr_value.value_case() == AttrValue::kType &&
              attr_value.type() == DT_RESOURCE) {
            return true;
            break;
          }
        }
      }
    }
  }
  return false;
}

// Check that graph has tf.StatefulPartitionedCall op with _XlaMustCompile.
bool HasQualifiedNonTPUOp(const Graph& graph) {
  const std::string kStatefulPartitionedCallOp = "StatefulPartitionedCall";
  const std::string kXlaMustCompile = "_XlaMustCompile";
  for (const Node* node : graph.nodes()) {
    auto node_op = node->type_string();
    if (node_op == kStatefulPartitionedCallOp) {
      auto attr = node->attrs().FindByString(kXlaMustCompile);
      if (attr != nullptr && attr->b() == true) {
        return true;
      }
    }
  }
  return false;
}

// Check if non TPU pipeline should be used
bool EnableNonTpuBridge(const Graph& graph) {
  // Remark that this is staging change. It will be expanded later for further
  // check based on the requirement.
  return HasPsWithResourceVariable(graph) && HasQualifiedNonTPUOp(graph);
}

}  // namespace

// Analyzes the user requested policy as well as the contents of the graph and
// function_library_definition to determine whether the MLIR Bridge should be
// run.
//
// If the user explicitly requests the bridge be enabled or disabled, this
// function will respect the request. If the user does not explicitly request
// enabled or disabled, it will decide whether or not to run the bridge.
//
// The config_proto param is a required input for all TF1 graphs but it is
// redundant for TF2 graphs.
MlirOptimizationPassState MlirBridgePass::GetPassState(
    const DeviceSet* device_set, const ConfigProto& config_proto,
    const Graph& graph,
    const FunctionLibraryDefinition& function_library) const {
  // Skip MLIR TF XLA Bridge if no TPU devices found and the non TPU graph is
  // not qualified.
  if (device_set && !HasTPUDevice(*device_set) && !EnableNonTpuBridge(graph)) {
    return MlirOptimizationPassState::Disabled;
  }

  // We set `uses_uninitialized_resource_args` to false here because the first
  // phase of the bridge is not affected by uninitialized resource args.
  MlirBridgeRolloutPolicy policy = GetMlirBridgeRolloutPolicy(
      graph, &function_library, config_proto,
      /*uses_uninitialized_resource_args=*/false,
      /*is_v1_compat=*/false, /*record_stats=*/false);
  switch (policy) {
    case MlirBridgeRolloutPolicy::kEnabledByUser:
      return MlirOptimizationPassState::Enabled;
    case MlirBridgeRolloutPolicy::kEnabledAfterGraphAnalysis:
      return MlirOptimizationPassState::FallbackEnabled;
    case MlirBridgeRolloutPolicy::kDisabledByUser:
      VLOG(1) << "Skipping MLIR TPU Bridge, MLIR TPU bridge disabled by user. "
                 "Old bridge will evaluate.";
      metrics::UpdateTfMlirBridgeFirstPhaseCounter("tpu", "v2", true,
                                                   "disabled_by_user");
      return MlirOptimizationPassState::Disabled;
    case MlirBridgeRolloutPolicy::kDisabledAfterGraphAnalysis:
      VLOG(1) << "Skipping MLIR TPU Bridge, MLIR TPU bridge disabled because "
                 "graph has unsupported features. Old bridge will evaluate.";
      metrics::UpdateTfMlirBridgeFirstPhaseCounter("tpu", "v2", true,
                                                   "invalid_graph");
      // We set `uses_uninitialized_resource_args` to false here because the
      // first phase of the bridge is not affected by uninitialized resource
      // args.
      // For Invalid Graph Analysis we need to log here because Run will not be
      // called.
      LogGraphFeatures(graph, &function_library, config_proto,
                       /*uses_uninitialized_resource_args=*/false,
                       /*is_v1_compat=*/false);
      return MlirOptimizationPassState::Disabled;
  }
}

// This runs the first phase of the "bridge", transforming the graph in a form
// that can be executed with delegation of some computations to an accelerator.
// This builds on the model of XLA where a subset of the graph is encapsulated
// and attached to a "compile" operation, whose result is fed to an "execute"
// operation. The kernel for these operations is responsible to lower the
// encapsulated graph to a particular device.
Status MlirBridgePass::Run(const ConfigProto& config_proto,
                           mlir::ModuleOp module, const Graph& graph,
                           const FunctionLibraryDefinition& function_library) {
  static absl::once_flag flag;
  absl::call_once(flag, UpdateLogVerbosityIfDefined, "TF_DEBUG_LOG_VERBOSITY");

  // Check if there are TPU devices or TPU ops. If not, then check if the
  // non TPU graph is qualified to run TF XLA Bridge.
  // This check needs to precede GetPassState for instrumentation purposes.
  bool is_qualified_for_tpu_bridge = HasTPUDevicesAndOps(module),
       is_qualified_for_non_tpu_bridge = false;
  if (!is_qualified_for_tpu_bridge)
    is_qualified_for_non_tpu_bridge = EnableNonTpuBridge(graph);
  if (!is_qualified_for_tpu_bridge && !is_qualified_for_non_tpu_bridge) {
    VLOG(1)
        << "Skipping MLIR TF XLA Bridge, no qualified devices or ops found.";
    return OkStatus();
  }

  // Set device_set to nullptr here as the device specific checks are performed
  // based on the devices in the module.
  // TODO(b/241853328): Add caching of pass state and call logging/metrics
  // related to graph analysis from here.
  auto pass_state = GetPassState(/*device_set=*/nullptr, config_proto, graph,
                                 function_library);

  if (pass_state == MlirOptimizationPassState::Disabled) {
    // GetPassState is called before run() and run() will only be called if the
    // pass is not disabled. However, the graph may have been updated between
    // when the pass state was originally calculated and now, so this check is
    // required to reflect any possible changes.
    VLOG(1) << "MlirBridgePass is disabled and will not run.";
    return OkStatus();
  }

  if (is_qualified_for_tpu_bridge) {
    bool fallback_enabled = false;
    if (pass_state == MlirOptimizationPassState::FallbackEnabled) {
      // We set `uses_uninitialized_resource_args` to false here because the
      // first phase of the bridge is not affected by uninitialized resource
      // args.
      // TODO (b/241853328) Consider moving logging if caching for graph
      // analysis or GetPassState is added
      LogGraphFeatures(graph, &function_library, config_proto,
                       /*uses_uninitialized_resource_args=*/false,
                       /*is_v1_compat=*/false);
      fallback_enabled = true;
    }
    VLOG(1) << "Running MLIR TPU Bridge";
    mlir_bridge_gauge_v2->GetCell()->Set(true);
    return mlir::TFTPU::TPUBridge(module, /*enable_logging=*/VLOG_IS_ON(1),
                                  fallback_enabled);
  }
  VLOG(1) << "Running MLIR non-TPU Bridge";
  return mlir::TF::RunTFXLABridge(module, VLOG_IS_ON(1));
}

MlirOptimizationPassState MlirBridgeV1CompatPass::GetPassState(
    const DeviceSet* device_set, const ConfigProto& config_proto,
    const Graph& graph,
    const FunctionLibraryDefinition& function_library) const {
  // Skip MLIR TPU Bridge if no TPU devices found.
  if (device_set && !HasTPUDevice(*device_set))
    return MlirOptimizationPassState::Disabled;
  // We set `uses_uninitialized_resource_args` to false here because the first
  // phase of the bridge is not affected by uninitialized resource args.
  MlirBridgeRolloutPolicy policy = GetMlirBridgeRolloutPolicy(
      graph, /*function_library=*/&function_library, config_proto,
      /*uses_uninitialized_resource_args=*/false, /*is_v1_compat=*/true,
      /*record_stats=*/false);
  switch (policy) {
    case MlirBridgeRolloutPolicy::kEnabledByUser:
      return MlirOptimizationPassState::Enabled;
    case MlirBridgeRolloutPolicy::kEnabledAfterGraphAnalysis:
      return MlirOptimizationPassState::FallbackEnabled;
    case MlirBridgeRolloutPolicy::kDisabledByUser:
      VLOG(1) << "Skipping MLIR TPU Bridge V1 Compat, MLIR TPU bridge disabled "
                 "by user. Old bridge will evaluate.";
      metrics::UpdateTfMlirBridgeFirstPhaseCounter("tpu", "v1", true,
                                                   "disabled_by_user");
      return MlirOptimizationPassState::Disabled;
    case MlirBridgeRolloutPolicy::kDisabledAfterGraphAnalysis:
      VLOG(1) << "Skipping MLIR TPU Bridge V1 Compat, MLIR TPU bridge disabled "
                 "because graph has unsupported features. Old bridge will "
                 "evaluate.";
      metrics::UpdateTfMlirBridgeFirstPhaseCounter("tpu", "v1", true,
                                                   "invalid_graph");
      // We set `uses_uninitialized_resource_args` to false here because the
      // first phase of the bridge is not affected by uninitialized resource
      // args.
      // For Invalid Graph Analysis we need to log here because Run will not be
      // called.
      LogGraphFeatures(graph, &function_library, config_proto,
                       /*uses_uninitialized_resource_args=*/false,
                       /*is_v1_compat=*/true);
      return MlirOptimizationPassState::Disabled;
  }
}

Status MlirBridgeV1CompatPass::Run(const GraphOptimizationPassOptions& options,
                                   mlir::ModuleOp module) {
  static absl::once_flag flag;
  absl::call_once(flag, UpdateLogVerbosityIfDefined, "TF_DEBUG_LOG_VERBOSITY");

  // Skip function graphs as MlirBridgePass will be used instead.
  if (options.is_function_graph) return OkStatus();

  // Skip MLIR TPU Bridge if no TPU devices or TPU ops found.
  if (!HasTPUDevicesAndOps(module)) {
    VLOG(1) << "Skipping MLIR TPU Bridge V1 Compat, no TPU devices or TPU ops "
               "found";
    return OkStatus();
  }

  MlirOptimizationPassState pass_state =
      GetPassState(/*device_set=*/nullptr, options.session_options->config,
                   **options.graph, *options.flib_def);

  // Set device_set to nullptr here as the device specific checks are performed
  // based on the devices in the module.
  if (pass_state == MlirOptimizationPassState::Disabled) {
    // GetPassState is called before run() and run() will only be called if the
    // pass is not disabled. However, the graph may have been updated between
    // when the pass state was originally calculated and now, so this check is
    // required to reflect any possible changes.
    VLOG(1) << "Skipping MLIR TPU Bridge V1 Compat, session flag not enabled";
    mlir_bridge_gauge_v1->GetCell()->Set(false);
    return OkStatus();
  }

  VLOG(1) << "Running MLIR TPU Bridge V1 Compat";

  bool fallback_enabled = false;
  if (pass_state == MlirOptimizationPassState::FallbackEnabled) {
    // We set `uses_uninitialized_resource_args` to false here because the first
    // phase of the bridge is not affected by uninitialized resource args.
    // TODO (b/241853328) Consider moving logging if caching for graph analysis
    // or GetPassState is added
    LogGraphFeatures(**options.graph, options.flib_def,
                     options.session_options->config,
                     /*uses_uninitialized_resource_args=*/false,
                     /*is_v1_compat=*/true);
    fallback_enabled = true;
  }

  mlir_bridge_gauge_v1->GetCell()->Set(true);

  return mlir::TFTPU::TPUBridgeV1Compat(
      module, /*enable_logging=*/VLOG_IS_ON(1), fallback_enabled);
}

}  // namespace tensorflow
