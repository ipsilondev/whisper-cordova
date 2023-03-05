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

#include "tensorflow/dtensor/cc/dtensor_device.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/memory/memory.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "llvm/Support/Casting.h"
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/OwningOpRef.h"  // from @llvm-project
#include "tensorflow/c/c_api_experimental.h"
#include "tensorflow/c/eager/c_api.h"
#include "tensorflow/c/eager/parallel_device/parallel_device_lib.h"
#include "tensorflow/c/eager/tfe_context_internal.h"
#include "tensorflow/c/eager/tfe_tensorhandle_internal.h"
#include "tensorflow/c/tf_datatype.h"
#include "tensorflow/c/tf_status.h"
#include "tensorflow/c/tf_status_helper.h"
#include "tensorflow/c/tf_tensor_internal.h"
#include "tensorflow/compiler/mlir/tensorflow/translate/export_graphdef.h"
#include "tensorflow/compiler/mlir/tensorflow/translate/mlir_roundtrip_flags.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/stream_executor/tpu/c_api_decl.h"
#include "tensorflow/compiler/xla/stream_executor/tpu/tpu_platform_interface.h"
#include "tensorflow/compiler/xla/stream_executor/tpu/tpu_topology.h"
#include "tensorflow/core/common_runtime/device_set.h"
#include "tensorflow/core/common_runtime/eager/context.h"
#include "tensorflow/core/common_runtime/eager/tensor_handle.h"
#include "tensorflow/core/common_runtime/graph_constructor.h"
#include "tensorflow/core/common_runtime/shape_refiner.h"
#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/framework/function.pb.h"
#include "tensorflow/core/framework/graph_to_functiondef.h"
#include "tensorflow/core/framework/node_def_builder.h"
#include "tensorflow/core/framework/node_def_util.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/graph/algorithm.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/lib/strings/proto_serialization.h"
#include "tensorflow/core/platform/casts.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/fingerprint.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/profiler/lib/traceme.h"
#include "tensorflow/core/util/dump_graph.h"
#include "tensorflow/dtensor/cc/constants.h"
#include "tensorflow/dtensor/cc/dstatus.h"
#include "tensorflow/dtensor/cc/dtensor_device_util.h"
#include "tensorflow/dtensor/cc/dtensor_graph_to_mlir_pass.h"
#include "tensorflow/dtensor/cc/parallel_executor.h"
#include "tensorflow/dtensor/cc/small_constant_optimization.h"
#include "tensorflow/dtensor/cc/tensor_layout.h"
#include "tensorflow/dtensor/cc/tpu_system_interface.h"
#include "tensorflow/dtensor/proto/layout.pb.h"
#include "tensorflow/tsl/platform/status.h"
#include "tensorflow/tsl/platform/statusor.h"
#include "tensorflow/tsl/util/env_var.h"

namespace tensorflow {
namespace dtensor {

class DTensorDevice {
 public:
  static StatusOr<DTensorDevice*> Create(absl::string_view name) {
    std::string use_parallel_executor;
    TF_RETURN_IF_ERROR(tsl::ReadStringFromEnvVar(
        "DTENSOR_USE_PARALLEL_EXECUTOR", "", &use_parallel_executor));
    if (use_parallel_executor.empty()) {
      return new DTensorDevice(name, nullptr);
    } else {
      TF_ASSIGN_OR_RETURN(auto parallel_executor,
                          CreateDefaultParallelExecutor());
      return new DTensorDevice(name, std::move(parallel_executor));
    }
  }

  void AddMesh(std::unique_ptr<MeshWithParallelDevice> mesh,
               bool is_host_mesh) {
    if (is_host_mesh) {
      std::string& tpu_host_mesh = Mesh::tpu_host_mesh();
      const std::string new_tpu_host_mesh = mesh->mesh_config().ToString();
      if (!tpu_host_mesh.empty()) {
        // TODO(b/180046115): Add per-TPU-mesh host mesh bookkeeping.
        LOG(WARNING)
            << "A new TPU host mesh is overwriting the old TPU host mesh. The "
               "old TPU mesh cannot be used in sea of donuts mode anymore.";
      }
      tpu_host_mesh.assign(new_tpu_host_mesh);
    }
    // For idempotency, don't register the same mesh twice.
    if (!mesh_to_device_map_.insert({mesh->mesh_config(), std::move(mesh)})
             .second)
      return;
    if (!default_mesh_) {
      global_default_mesh_ = mesh_to_device_map_.begin()->second.get();
      default_mesh_ = global_default_mesh_;
    }
  }

  // Runs an operation on the DTensorDevice,
  //
  // Ignoring the placement of the original op (TFE_OpGetDevice(original_op)).
  // This indicates whether the user explicitly placed the op on the DTensor
  // device (vs. having it placed on the DTensor device because an input was
  // placed there), but DTensor is doing type-based dispatch and so handles
  // these cases identically at the moment.
  void Execute(const TFE_Op* original_op, int* num_outputs,
               TFE_TensorHandle** outputs, TF_Status* status);

  void SetDefaultLayout(Layout layout) { default_layout_.emplace(layout); }
  void ClearDefaultLayout() { default_layout_.reset(); }
  void SetDefaultMesh(Mesh mesh) {
    default_mesh_ = mesh_to_device_map_.at(mesh).get();
  }
  void ClearDefaultMesh() { default_mesh_ = global_default_mesh_; }
  void SetSameShapePolicy(bool enabled) {
    same_shape_policy_enabled_ = enabled;
  }

  Status SetTPUCoreIDs(const std::string& mesh_name,
                       const std::vector<int>& tpu_core_ids) {
    if (VLOG_IS_ON(1)) {
      LOG(INFO) << "Setting TPU core IDs for "
                << (mesh_name.empty() ? "default mesh" : mesh_name) << ": ";
      for (auto i : tpu_core_ids) {
        LOG(INFO) << i;
      }
    }
    // Setting the default mesh under an empty name repeatedly is fine, which
    // happens when initialize_tpu_system is called multiple times
    // especially in tests. All the set mappings should be the same anyway.
    if (!mesh_name.empty() && Mesh::tpu_core_ids().count(mesh_name) > 0) {
      return errors::AlreadyExists("Mesh name already in use: ", mesh_name);
    }
    Mesh::tpu_core_ids()[mesh_name].assign(tpu_core_ids.begin(),
                                           tpu_core_ids.end());
    return OkStatus();
  }

  void ClearTPUCoreIDs() { Mesh::tpu_core_ids().clear(); }

  std::vector<std::vector<int>> TPUCoreIDsToLocations(
      TFE_Context* context, const std::vector<int>& tpu_core_ids) {
    TpuSystemInterface* tpu_system = GetPreferredTpuSystem();
    if (tpu_system == nullptr) {
      VLOG(1) << "Calling TPUCoreIDsToLocations on the default TPU system.";
      std::vector<std::vector<int>> tpu_core_locations;
      tpu_core_locations.reserve(tpu_core_ids.size());
      tpu::TpuPlatformInterface* tpu_platform =
          tpu::TpuPlatformInterface::GetRegisteredPlatform();
      if (tpu_platform == nullptr) {
        LOG(WARNING) << "No TPU platform is found.";
        return {{}};
      }
      if (!tpu_platform->Initialized()) {
        LOG(WARNING) << "TPU platform is not initialized.";
        return {{}};
      }
      tpu::TpuTopologyExternal tpu_topology = tpu_platform->topology();

      for (const int& tpu_core_id : tpu_core_ids) {
        tpu::TpuCoreLocationExternal core =
            tpu_topology.CoreForId(TpuCoreTypeEnum::kTensorCore, tpu_core_id);
        tpu::TpuDimensionsExternal tpu_chip_location = core.chip_coordinates();
        tpu_core_locations.push_back({tpu_chip_location.x, tpu_chip_location.y,
                                      tpu_chip_location.z, core.index()});
      }
      return tpu_core_locations;
    } else {
      VLOG(1) << "Calling TPUCoreIDsToLocations on a preferred TPU system.";
      return tpu_system->TPUCoreIDsToLocations(context, tpu_core_ids);
    }
  }

  std::vector<int> TPUCoreLocationsToIDs(
      TFE_Context* context,
      const std::vector<std::vector<int>>& tpu_core_locations) {
    TpuSystemInterface* tpu_system = GetPreferredTpuSystem();
    if (tpu_system == nullptr) {
      VLOG(1) << "Calling TPUCoreLocationsToIDs on the default TPU system.";
      std::vector<int> tpu_core_ids;
      tpu_core_ids.reserve(tpu_core_locations.size());
      tpu::TpuPlatformInterface* tpu_platform =
          tpu::TpuPlatformInterface::GetRegisteredPlatform();
      if (tpu_platform == nullptr) {
        LOG(WARNING) << "No TPU platform is found.";
        return {};
      }
      if (!tpu_platform->Initialized()) {
        LOG(WARNING) << "TPU platform is not initialized.";
        return {};
      }
      tpu::TpuTopologyExternal tpu_topology = tpu_platform->topology();

      for (const std::vector<int>& tpu_core_location : tpu_core_locations) {
        tpu::TpuCoreLocationExternal core = tpu_topology.Core(
            TpuCoreTypeEnum::kTensorCore, tpu_core_location[0],
            tpu_core_location[1], tpu_core_location[2], tpu_core_location[3]);
        tpu_core_ids.push_back(core.Id());
      }
      return tpu_core_ids;
    } else {
      VLOG(1) << "Calling TPUCoreLocationsToIDs on a preferred TPU system.";
      return tpu_system->TPUCoreLocationsToIDs(context, tpu_core_locations);
    }
  }

  // Waits for ops to finish in ALL meshes as we share the cancellation manager.
  void AsyncWait(TFE_Context* context, TF_Status* status) {
    std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> first_bad_status(
        nullptr, TF_DeleteStatus);

    for (const auto& pair : mesh_to_device_map_) {
      std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> async_wait_status(
          TF_NewStatus(), TF_DeleteStatus);

      pair.second->parallel_device().AsyncWait(context,
                                               async_wait_status.get());

      TF_Code error_code = TF_GetCode(async_wait_status.get());
      if (error_code != TF_OK &&
          (first_bad_status == nullptr ||
           TF_GetCode(first_bad_status.get()) == TF_CANCELLED)) {
        first_bad_status.reset(TF_NewStatus());
        TF_SetStatus(first_bad_status.get(), error_code,
                     TF_Message(async_wait_status.get()));
      }
    }

    if (first_bad_status != nullptr) {
      TF_SetStatus(status, TF_GetCode(first_bad_status.get()),
                   TF_Message(first_bad_status.get()));
    }

    // Reset the global function rendezvous, which otherwise stores a failure
    // state.
    tensorflow::unwrap(context)->ResetGlobalRendezvousForFunction();

    // Reset the cancellation manager on (potential) failure so we don't cancel
    // future ops. This is only safe because we have just cleared pending async
    // nodes, which may have had a reference to he cancellation manager.
    cancellation_manager_ = std::make_unique<CancellationManager>();
  }

  TFE_TensorHandle* Pack(TFE_Context* context, int num_inputs,
                         TFE_TensorHandle** inputs,
                         const std::string& string_layout, TF_Status* status);

  std::vector<TFE_TensorHandle*> Unpack(TFE_Context* context,
                                        TFE_TensorHandle* input,
                                        TF_Status* status);

  // Return the layout for the input tensor.
  std::string FetchLayout(TFE_Context* context, TFE_TensorHandle* input,
                          TF_Status* status);

  // Returns whether `input` is a dtensor of this DTensorDevice.
  bool IsDTensor(TFE_Context* context, TFE_TensorHandle* input,
                 TF_Status* status);

  TFE_TensorHandle* SparsePack(TFE_Context* context, int num_inputs,
                               TFE_TensorHandle** indices,
                               TFE_TensorHandle** values,
                               TFE_TensorHandle** shapes,
                               const std::string& string_layout,
                               TF_Status* status);

  bool IsSparseDTensor(TFE_Context* context, TFE_TensorHandle* input,
                       TF_Status* status);

  std::unordered_map<std::string, int> GetFunctionCacheStats(
      TFE_Context* context, TF_Status* status) const;

  void SetIteratorElementLayouts(TFE_Context* context, TFE_TensorHandle* input,
                                 const std::vector<std::string>& string_layouts,
                                 TF_Status* status);

 private:
  DTensorDevice(absl::string_view name,
                std::unique_ptr<ParallelExecutor> parallel_executor)
      : name_(name),
        same_shape_policy_enabled_(false),
        function_manager_(new ExecutableManager<ExecutionFunctions>()),
        module_manager_(
            new ExecutableManager<mlir::OwningOpRef<mlir::ModuleOp>>()),
        cancellation_manager_(std::make_unique<CancellationManager>()),
        parallel_executor_(std::move(parallel_executor)) {}

  // If the `operation_name` of an op indicates a custom DTensor op then
  // separately handle those custom ops instead of running default DTensor graph
  // compilation.
  void MaybeHandleDTensorCustomOps(
      const char* operation_name, const int num_inputs,
      const TFE_OpAttrs* attributes, TFE_Context* context,
      TFE_TensorHandle** inputs, int* num_outputs, TFE_TensorHandle** outputs,
      bool* is_custom_dtensor_op, TF_Status* status);

  // Update output layouts for eager ops based on same shape policy.
  Status UpdateOutputLayoutsWithSameShapePolicy(
      const std::vector<PartialTensorShape>& global_output_shapes,
      const absl::flat_hash_set<Mesh>& input_meshes, absl::string_view op_name,
      tensorflow::Graph* graph, std::vector<const Layout*>* output_layouts);

  // Stores states of a DTensorOperation that will be used for lowering,
  // including different representations (e.g. MLIR Module) of the
  // DTensorOperation, and other states (e.g. output layouts and shapes).
  struct DTensorOperationLoweringContext {
    // Optional MLIR module representation of the DTensorOperation.
    // If exists, it is associated with DTensorDevice's PassRunner.
    std::optional<mlir::ModuleOp> module;
    // Graph representation of the DTensorOperation.
    std::unique_ptr<tensorflow::Graph> graph;
    // Derived output layout of the DTensorOperation
    std::vector<const Layout*> output_layouts;
    // Derived global output shapes of the DTensorOperation.
    std::vector<PartialTensorShape> global_output_shapes;
    // TF Device list associated with the DTensorOperation.
    std::vector<tensorflow::Device*> tf_devices;
    // Cache key of the operation calculated by
    // ExecutableManager<T>::GetCachedExecutable based on the doperation and its
    // metadata (e.g. inputs).
    tensorflow::Fprint128 doperation_cache_key;
  };

  // Takes the description of a DTensorOperation and makes a ModuleOp out of it.
  // The resulting ModuleOp and other derived states of the DTensorOperation are
  // stored in the DTensorOperationLoweringContext. The Module is not
  // transformed by DTensor passes.
  StatusOr<DTensorOperationLoweringContext> DTensorOperationToModule(
      TFE_Context* context, const std::vector<TensorWithLayout*>& inputs,
      const DTensorOperation& doperation, const NameAttrList& eager_attributes);

  // Lowers the ModuleOp in the input DTensorOperationLoweringContext, and
  // extracts ExecutionFunctions from lowered ModuleOp. Some fields (e.g. graph)
  // of the input DTensorOperationLoweringContext may be updated.
  void ModuleToExecutionFunctions(
      TFE_Context* context, const std::vector<TensorWithLayout*>& inputs,
      const DTensorOperation& doperation, const NameAttrList& eager_attributes,
      int num_outputs, DTensorOperationLoweringContext& lowering_context,
      const ExecutionFunctions** execution_functions, TF_Status* status);

  // Execute a given function.
  void ExecuteFunctionAndWait(
      TFE_Context* context, const TranslatedFunction* function_ptr,
      const MeshWithParallelDevice* parallel_device_mesh,
      const std::vector<parallel_device::ParallelTensor*>& parallel_inputs,
      const int64_t step_id, const TFE_OpAttrs* attributes, TF_Status* status);

  // Execute regular operation with ParallelExecutor
  void ParallelExecuteRegularOperation(
      TFE_Context* context, const std::vector<TensorWithLayout*>& inputs,
      mlir::ModuleOp mlir_module, const DTensorOperation& doperation,
      const TFE_OpAttrs* attributes, int* num_outputs,
      TFE_TensorHandle** outputs, TF_Status* status);

  // Implements `Execute` for operations which aren't special-cased in
  void ExecuteRegularOperation(TFE_Context* context,
                               const std::vector<TensorWithLayout*>& inputs,
                               const DTensorOperation& doperation,
                               const TFE_OpAttrs* attributes, int* num_outputs,
                               TFE_TensorHandle** outputs, TF_Status* status);

  // Wraps a TensorWithLayout into a TFE_TensorHandle.
  TFE_TensorHandle* MakeLayoutTensorHandle(TFE_Context* context,
                                           std::unique_ptr<TensorWithLayout> t,
                                           TF_Status* status);

  void RecordInShapeLayoutCache(const TensorWithLayout& tensor);

  // Choose a mesh to broadcast a non-dtensor to a dtensor based on the
  // operation, other input meshes, default mesh, and dtypes.
  const MeshWithParallelDevice* ChooseBroadcastingMesh(
      const absl::flat_hash_set<Mesh>& input_meshes,
      const std::vector<TF_DataType>& dtypes);

  // Returns whether a given mesh is a remote mesh.
  bool is_remote_mesh(const Mesh& mesh) const;

  // Broadcasts `tensor` to `mesh` using replicated sharding. Returns `nullptr`
  // if it fails.
  // TODO(b/256016071): Unify this and the one in `TensorWithLayoutTf`.
  std::unique_ptr<TensorWithLayout> Broadcast(TFE_Context* context,
                                              TFE_TensorHandle* input,
                                              const Mesh& mesh,
                                              TF_Status* status);

  // The name of the device (the custom device)
  std::string name_;
  // Mesh configs with matching parallel devices.
  //
  // For now we just consider the first entry added to dtensor_device as the
  // default mesh. Before we reach an agreement on this, we'll leave it as is.
  absl::flat_hash_map<Mesh, std::unique_ptr<MeshWithParallelDevice>>
      mesh_to_device_map_;
  // TODO(hthu): Consider whether we want to preserve the default_mesh semantic.
  // Current default mesh consistent to default_layout_. If default_layout_ is
  // not set, it equals to global_default_mesh_.
  const MeshWithParallelDevice* default_mesh_ = nullptr;
  // The default mesh of a DTensorDevice, which cannot be modified once being
  // set.
  const MeshWithParallelDevice* global_default_mesh_ = nullptr;
  // If the user has specified a default output layout.
  std::optional<Layout> default_layout_;

  // Determines whether tensors with a shape previously associated with only one
  // layout use that layout if nothing else can be inferred.
  bool same_shape_policy_enabled_;

  DTensorMlirPassRunner pass_runner_;

  struct CachedLayout {
    // The first layout seen with this shape
    Layout layout;
    // Whether the layout is unique for this shape
    bool is_unique;
  };
  absl::flat_hash_map<int64_t, CachedLayout> shape_layout_cache_;

  core::RefCountPtr<ExecutableManager<ExecutionFunctions>> function_manager_;
  core::RefCountPtr<ExecutableManager<mlir::OwningOpRef<mlir::ModuleOp>>>
      module_manager_;

  // Coordinates cancelling ops across meshes on error. Must outlive any queued
  // async op launches, so we only reset it after seeing a failure status.
  std::unique_ptr<CancellationManager> cancellation_manager_;

  // Map each function_mesh_fingerprint (based on the set of the mesh involved)
  // to the number of times of the function execution. The
  // function_mesh_fingerprint and the counter together are used for generating
  // the step id, which is used for rendezvous creation.
  absl::flat_hash_map<uint64, uint64> func_mesh_fingerprint_to_step_counter_;

  // Dispatchs post-SPMD functions.
  std::unique_ptr<ParallelExecutor> parallel_executor_;
};

int64_t FingerprintShape(const absl::Span<const int64_t> shape) {
  int64_t fprint = 0;
  for (int64_t dim : shape) {
    fprint = FingerprintCat64(fprint, dim);
  }
  return fprint;
}

parallel_device::ParallelTensor* MeshWithParallelDevice::DeviceIDs(
    TFE_Context* context, TF_Status* status) const {
  if (device_ids_tensor_ == nullptr) {
    // Global device IDs sequentially increase.
    //
    // This is the assumption in the dtensor software stack. MLIR pass relies on
    // this assumption to generate mesh coordinates for each core efficiently.
    //
    // The rule to set local ids and the mapping from global ids to real
    // physical core index, e.g., TPU, is nontrivial unfortunately. It is
    // possible to set identical mapping but the collective operation
    // performance is terrible for most of cases.
    //
    // - For ICI-connected TPU slice, see go/dtensor-device-assignment-summary
    //   for guide how to create efficient core assignments toward peak
    //   performance.
    //
    //   The global id to core assignment mapping is bridged by
    //   `Mesh::tpu_core_ids()` and consumed by `UpdateTPUCompileMetadata`.
    //
    // - For DCN-connected topology, we need to map different sections of the
    //   global ids to its real physical cores separately according to the
    //   runtime requirements. For example, for a 4x32 mesh, in which the outer
    //   dimension is connected via DCN and inner dimension is connected by ICI,
    //   the device assignments for inner dimension should typically form its
    //   own ring order (not plain physical core index) in each sub-meshes and
    //   the outer dimension should be assigned according to the real physical
    //   ring of DNC hosts.
    //
    // Note: In order to change this assumption, MLIR pass needs adjustment. One
    // possible approach is to take a N-D mapping vector for N-D mesh and lookup
    // the coordinates in MLIR, by consulting tensor layout as well, rather than
    // calculation on-the-fly.

    // LINT.IfChange
    for (int64_t i = 0; i < mesh_config_.global_device_ids().size(); ++i) {
      if (mesh_config_.global_device_ids()[i] - i !=
          mesh_config_.global_device_ids()[0]) {
        TF_SetStatus(
            status, TF_INTERNAL,
            absl::StrCat("Global device IDs should be consecutive: ",
                         absl::StrJoin(mesh_config_.global_device_ids(), ", "))
                .c_str());
        return nullptr;
      }
    }
    // LINT.ThenChange(//tensorflow/dtensor/python/layout.py)

    // Local device IDs are a subset of global device IDs, arranged in device
    // ordinal order.
    std::vector<int32_t> ids;
    for (int64_t id : mesh_config_.local_device_ids()) {
      ids.push_back(id);
    }
    VLOG(1) << "Parallel device IDs: " << absl::StrJoin(ids, ", ");
    device_ids_tensor_ =
        parallel_device_->ScalarsFromSequence<int32_t>(ids, context, status);
    if (TF_GetCode(status) != TF_OK) return nullptr;
  }
  return device_ids_tensor_.get();
}

int TensorWithLayoutNumDims(void* data, TF_Status* status) {
  return reinterpret_cast<TensorWithLayout*>(data)->global_shape().size();
}

int64_t TensorWithLayoutDim(void* data, int dim_index, TF_Status* status) {
  return reinterpret_cast<TensorWithLayout*>(data)->global_shape()[dim_index];
}

void TensorWithLayoutDeallocator(void* data) {
  delete reinterpret_cast<TensorWithLayout*>(data);
}

TF_Buffer* TensorWithLayoutSummarize(void* data, TF_Status* status) {
  std::string summary =
      reinterpret_cast<TensorWithLayout*>(data)->SummarizeValue();
  return TF_NewBufferFromString(summary.data(), summary.size());
}

const MeshWithParallelDevice* DTensorDevice::ChooseBroadcastingMesh(
    const absl::flat_hash_set<Mesh>& input_meshes,
    const std::vector<TF_DataType>& dtypes) {
  bool has_string_dtype = std::find(dtypes.begin(), dtypes.end(),
                                    TF_DataType::TF_STRING) != dtypes.end();
  // String tensors can only be broadcast to a CPU mesh, so broadcast
  // to CPU mesh if there is one we can infer.
  if (has_string_dtype) {
    // Choose the first CPU mesh amongst the input meshes as the CPU broadcast
    // mesh if it exists.
    for (const Mesh& mesh : input_meshes) {
      if (mesh.is_cpu_mesh()) {
        return mesh_to_device_map_[mesh].get();
      }
    }
  }

  // If a unique mesh is identified across all inputs, we use that mesh as the
  // mesh to broadcast to. Otherwise we fallback to default mesh.
  const MeshWithParallelDevice* broadcast_mesh =
      input_meshes.size() == 1
          ? mesh_to_device_map_[*input_meshes.begin()].get()
          : default_mesh_;
  return broadcast_mesh;
}

TFE_TensorHandle* DTensorDevice::MakeLayoutTensorHandle(
    TFE_Context* context, std::unique_ptr<TensorWithLayout> t,
    TF_Status* status) {
  TF_DataType dtype = t->dtype();
  TFE_CustomDeviceTensorHandleMethods handle_methods;
  handle_methods.num_dims = &TensorWithLayoutNumDims;
  handle_methods.dim = &TensorWithLayoutDim;
  handle_methods.deallocator = &TensorWithLayoutDeallocator;
  handle_methods.summarize = &TensorWithLayoutSummarize;
  return TFE_NewCustomDeviceTensorHandle(context, name_.c_str(), dtype,
                                         /*data=*/t.release(), handle_methods,
                                         status);
}

void DTensorDevice::RecordInShapeLayoutCache(const TensorWithLayout& tensor) {
  auto existing = shape_layout_cache_.insert(
      {FingerprintShape(tensor.global_shape()),
       CachedLayout{tensor.layout(), /*is_unique=*/true}});

  if (!existing.second) {
    // There is an entry already; if the layout doesn't match we should record
    // the fact that it's not unique.
    if (tensor.layout() != existing.first->second.layout) {
      existing.first->second.is_unique = false;
    }
  }
}

bool DTensorDevice::is_remote_mesh(const Mesh& mesh) const {
  // An empty mesh might be assigned to VarHandleOp during DTensor MLIR lowering
  // pass. Decide whether the empty mesh is remote based on the current default
  // mesh.
  return mesh.is_remote() ||
         (mesh.IsEmpty() && default_mesh_->mesh_config().is_remote());
}

std::unique_ptr<TensorWithLayout> DTensorDevice::Broadcast(
    TFE_Context* context, TFE_TensorHandle* input, const Mesh& mesh,
    TF_Status* status) {
  const char* input_device = TFE_TensorHandleDeviceName(input, status);
  if (TF_GetCode(status) != TF_OK) {
    TF_SetStatus(status, TF_INVALID_ARGUMENT,
                 "Failed to get a valid input device.");
    return nullptr;
  }
  if (name_ == input_device) {
    TF_SetStatus(status, TF_INVALID_ARGUMENT,
                 "Input to Broadcast must be eager tensor.");
    return nullptr;
  }

  TF_Tensor* tf_tensor = TFE_TensorHandleResolve(input, status);
  if (TF_GetCode(status) != TF_OK) {
    TF_SetStatus(status, TF_INVALID_ARGUMENT,
                 "Failed to resolve the input to tensor.");
    return nullptr;
  }
  std::unique_ptr<TF_Tensor, decltype(&TF_DeleteTensor)> resolved_tensor(
      tf_tensor, TF_DeleteTensor);
  Tensor tensor;
  const auto tf_tensor_to_tensor_status =
      TF_TensorToTensor(resolved_tensor.get(), &tensor);
  if (!tf_tensor_to_tensor_status.ok()) {
    TF_SetStatus(status, TF_INTERNAL,
                 tf_tensor_to_tensor_status.ToString().c_str());
    return nullptr;
  }
  if (!parallel_executor_) {
    TF_SetStatus(status, TF_INTERNAL, "Parallel executor is null.");
    return nullptr;
  }
  const Layout layout = Layout::ReplicatedOnMesh(mesh, tensor.dims());
  std::optional<NodeDef> const_value =
      ExtractSmallTensorValue(context, input, layout, status);
  if (TF_GetCode(status) != TF_OK) {
    return nullptr;
  }
  auto tensor_with_layout_pw =
      parallel_executor_->Broadcast(tensor, mesh, const_value);
  if (!tensor_with_layout_pw.ok()) {
    TF_SetStatus(status, TF_INTERNAL,
                 tensor_with_layout_pw.status().ToString().c_str());
    return nullptr;
  }
  return std::move(*tensor_with_layout_pw);
}

StatusOr<NameAttrList> FetchAttributes(const TFE_OpAttrs* attributes) {
  // TODO(allenl): Should we just give up on the public C API to save on
  // serialization/deserialization? We need all of the attributes and to treat
  // them generically, which isn't going to be pleasant with typed attribute
  // methods.
  std::unique_ptr<TF_Buffer, decltype(&TF_DeleteBuffer)> serialized_attributes(
      TF_NewBuffer(), TF_DeleteBuffer);

  TF_Status* status = TF_NewStatus();
  TFE_OpAttrsSerialize(attributes, serialized_attributes.get(), status);
  if (TF_GetCode(status) == TF_OK) {
    TF_DeleteStatus(status);
  } else {
    Status failure_status = StatusFromTF_Status(status);
    TF_DeleteStatus(status);
    return failure_status;
  }

  NameAttrList name_and_attrs;
  if (!name_and_attrs.ParseFromArray(serialized_attributes->data,
                                     serialized_attributes->length)) {
    return tensorflow::errors::Unknown("Could not parse attributes");
  }
  return name_and_attrs;
}

StatusOr<Layout> FetchLayoutFromAttributes(const TFE_OpAttrs* attributes,
                                           absl::string_view attribute_name) {
  // Get attributes.
  TF_ASSIGN_OR_RETURN(NameAttrList name_and_attrs, FetchAttributes(attributes));

  // Get layout string from attributes.
  absl::string_view layout_str =
      name_and_attrs.attr().find(std::string(attribute_name))->second.s();

  // This would probably be slow at the moment without caching.
  // We should consider making this faster in the future.
  return Layout::FromString(string(layout_str));
}

std::string DTensorDevice::FetchLayout(TFE_Context* context,
                                       TFE_TensorHandle* input,
                                       TF_Status* status) {
  VLOG(1) << "Checking layout...";
  const char* input_device = TFE_TensorHandleDeviceName(input, status);
  if (input_device != name_) {
    TF_SetStatus(status, TF_INVALID_ARGUMENT,
                 "FetchLayout expects a tensor placed on the layout device.");
    return {};
  }
  TensorWithLayout* t = reinterpret_cast<TensorWithLayout*>(
      TFE_TensorHandleDevicePointer(input, status));
  if (TF_GetCode(status) != TF_OK) return {};
  return t->layout().ToString();
}

bool DTensorDevice::IsDTensor(TFE_Context* context, TFE_TensorHandle* input,
                              TF_Status* status) {
  const char* input_device = TFE_TensorHandleDeviceName(input, status);
  return input_device == name_;
}

std::vector<TFE_TensorHandle*> DTensorDevice::Unpack(TFE_Context* context,
                                                     TFE_TensorHandle* input,
                                                     TF_Status* status) {
  std::vector<TFE_TensorHandle*> outputs;

  const char* input_device = TFE_TensorHandleDeviceName(input, status);
  if (TF_GetCode(status) != TF_OK) return outputs;
  if (input_device != name_) {
    TF_SetStatus(
        status, TF_INVALID_ARGUMENT,
        absl::StrCat(
            "DTensorUnpack expects a tensor placed on the DTensor device: ",
            name_, ", but input was placed on device: ", input_device)
            .c_str());
    return outputs;
  }
  TensorWithLayout* t = reinterpret_cast<TensorWithLayout*>(
      TFE_TensorHandleDevicePointer(input, status));
  if (TF_GetCode(status) != TF_OK) return outputs;

  if (is_remote_mesh(t->mesh())) {
    TF_SetStatus(status, TF_UNIMPLEMENTED,
                 "DTensorUnpack is not supported on a remote mesh.");
    return outputs;
  }
  const int output_size = t->num_tensors();
  outputs.resize(output_size);

  for (int output_index = 0; output_index < output_size; ++output_index) {
    outputs[output_index] =
        TFE_TensorHandleCopySharingTensor(t->get_tensor(output_index), status);
    if (TF_GetCode(status) != TF_OK) {
      return outputs;
    }
  }
  return outputs;
}

void DTensorDevice::MaybeHandleDTensorCustomOps(
    const char* operation_name, const int num_inputs,
    const TFE_OpAttrs* attributes, TFE_Context* context,
    TFE_TensorHandle** inputs, int* num_outputs, TFE_TensorHandle** outputs,
    bool* is_custom_dtensor_op, TF_Status* status) {
  *is_custom_dtensor_op = true;
  if (operation_name == std::string("_EagerConst")) {
    // Op-by-op const has no obvious layout. DTensor skips an SPMD expansion and
    // instead relies on copy-on when the value is used later.
    std::unique_ptr<TFE_Op, decltype(&TFE_DeleteOp)> op(
        TFE_NewOp(context, operation_name, status), TFE_DeleteOp);
    if (TF_GetCode(status) != TF_OK) return;
    for (int input_index = 0; input_index < num_inputs; ++input_index) {
      TFE_OpAddInput(op.get(), inputs[input_index], status);
      if (TF_GetCode(status) != TF_OK) return;
    }
    TFE_OpAddAttrs(op.get(), attributes);
    TFE_Execute(op.get(), outputs, num_outputs, status);
    return;
  }

  *is_custom_dtensor_op = false;
}

namespace {

// Verifies that all components have the same dtype and shape.
// The component shape will be set upon success.
void VerifyPackTensorShapeAndDtype(
    std::vector<parallel_device::TensorHandlePtr>& components,
    std::vector<int64_t>* component_shape, TF_Status* status) {
  TF_DataType dtype = TFE_TensorHandleDataType(components[0].get());
  auto size = TFE_TensorHandleNumDims(components[0].get(), status);
  if (TF_GetCode(status) != TF_OK) return;
  component_shape->clear();
  component_shape->reserve(size);
  for (int i = 0; i < size; ++i) {
    component_shape->push_back(
        TFE_TensorHandleDim(components[0].get(), i, status));
    if (TF_GetCode(status) != TF_OK) return;
  }

  // Verify that the TensorHandle's shape and dtype match all of the component
  // shapes and dtypes.
  for (const auto& component : components) {
    for (int i = 0; i < component_shape->size(); ++i) {
      int64_t tensor_dim = TFE_TensorHandleDim(component.get(), i, status);
      if (TF_GetCode(status) != TF_OK) return;
      if (tensor_dim != (*component_shape)[i]) {
        TF_SetStatus(status, TF_UNIMPLEMENTED,
                     "Components of a PackedTensor must currently all have "
                     "the same shape");
        return;
      }
      if (TFE_TensorHandleDataType(component.get()) != dtype) {
        TF_SetStatus(status, TF_INTERNAL,
                     "Components of a PackedTensor must all have "
                     "the same dtype");
        return;
      }
    }
  }
}

// Verifies that all TensorHandles have rank `rank` of dtype `dtype`.
void VerifyTensorRankAndDType(TFE_TensorHandle** tensors, int num_input,
                              int expected_rank, TF_DataType* expected_dtype,
                              TF_Status* status) {
  for (int i = 0; i < num_input; ++i) {
    auto actual_rank = TFE_TensorHandleNumDims(tensors[i], status);
    if (TF_GetCode(status) != TF_OK)
      RETURN_STATUS(status, TF_INTERNAL, "Error getting rank of tensor.");
    if (actual_rank != expected_rank)
      RETURN_STATUS(status, TF_INVALID_ARGUMENT,
                    "Rank of tensor did not match the expected rank.");
    if (expected_dtype != nullptr &&
        TFE_TensorHandleDataType(tensors[i]) != *expected_dtype)
      RETURN_STATUS(status, TF_INVALID_ARGUMENT,
                    "Dtype of tensor did not match the expected dtype.");
  }
}

}  // namespace

TFE_TensorHandle* DTensorDevice::Pack(TFE_Context* context, int num_inputs,
                                      TFE_TensorHandle** inputs,
                                      const std::string& string_layout,
                                      TF_Status* status) {
  if (num_inputs < 1) {
    TF_SetStatus(status, TF_INVALID_ARGUMENT,
                 "DTensorPack requires 1 or more inputs");
    return nullptr;
  }
  StatusOr<Layout> target_layout = Layout::FromString(string_layout);
  if (!target_layout.ok()) {
    TF_SetStatus(status, TF_INVALID_ARGUMENT,
                 "Failed to parse layout from string layout");
    return nullptr;
  }
  const Mesh& target_mesh = target_layout->mesh();
  const MeshWithParallelDevice* target_parallel_device =
      mesh_to_device_map_[target_mesh].get();
  if (target_parallel_device == nullptr) {
    TF_SetStatus(status, TF_INVALID_ARGUMENT,
                 absl::StrCat("Required mesh : ", target_mesh.ToString(),
                              "is not registered with DTensor ")
                     .c_str());
    return nullptr;
  }

  std::unique_ptr<TensorWithLayout> packed_tensor;
  if (is_remote_mesh(target_parallel_device->mesh_config())) {
    // Create a dummy output for DTensorPack if inputs are on a remote mesh.
    TF_DataType dtype = TFE_TensorHandleDataType(inputs[0]);
    auto size = TFE_TensorHandleNumDims(inputs[0], status);
    if (TF_GetCode(status) != TF_OK) return nullptr;
    std::vector<int64_t> component_shape;
    component_shape.reserve(size);
    for (int i = 0; i < size; ++i) {
      component_shape.push_back(TFE_TensorHandleDim(inputs[0], i, status));
      if (TF_GetCode(status) != TF_OK) return nullptr;
    }
    packed_tensor = CreateDummyTensorWithLayout(
        component_shape, dtype, target_parallel_device->mesh_config(),
        *target_layout);

  } else {
    auto local_devices = target_parallel_device->mesh_config().local_devices();

    if (num_inputs !=
        target_parallel_device->parallel_device().num_underlying_devices()) {
      TF_SetStatus(status, TF_INVALID_ARGUMENT,
                   absl::StrCat("The dtensor device ", name_, " expected ",
                                local_devices.size(),
                                " inputs to DTensorPack, but got ", num_inputs)
                       .c_str());
      return nullptr;
    }

    std::vector<parallel_device::TensorHandlePtr> components;
    components.reserve(num_inputs);
    for (int i = 0; i < num_inputs; ++i) {
      TFE_TensorHandle* input = inputs[i];
      const char* input_device = TFE_TensorHandleDeviceName(input, status);
      if (TF_GetCode(status) != TF_OK) return nullptr;
      if (name_ == input_device) {
        TF_SetStatus(status, TF_INVALID_ARGUMENT,
                     "Does not support packing a Tensor that is already on "
                     "dtensor device");
        return nullptr;
      }
      // If `input` is on the target device, this creates a new handle sharing
      // the underlying data; otherwise, async copies are invoked.
      components.emplace_back(TFE_TensorHandleCopyToDevice(
          input, context, local_devices[i].c_str(), status));
      if (TF_GetCode(status) != TF_OK) return nullptr;
    }

    std::vector<int64_t> component_shape;
    VerifyPackTensorShapeAndDtype(components, &component_shape, status);
    if (TF_GetCode(status) != TF_OK) return nullptr;

    std::unique_ptr<parallel_device::ParallelTensor> parallel_tensor =
        parallel_device::ParallelTensor::FromTensorHandles(
            target_parallel_device->parallel_device(), std::move(components),
            status);
    if (TF_GetCode(status) != TF_OK) return nullptr;

    if (target_layout->rank() != component_shape.size()) {
      TF_SetStatus(
          status, TF_INVALID_ARGUMENT,
          absl::StrCat(
              "Packed layout should have the same rank as the rank for each "
              "component. The rank of each component is: ",
              component_shape.size(),
              ", while layout has rank: ", target_layout->rank(),
              "\nLayout: ", target_layout->ToString(), "\n")
              .c_str());
      return nullptr;
    }

    packed_tensor = CreateTensorWithLayout(
                        std::move(parallel_tensor),
                        target_parallel_device->mesh_config(), *target_layout)
                        .value();
  }

  RecordInShapeLayoutCache(*packed_tensor);
  TFE_TensorHandle* output =
      MakeLayoutTensorHandle(context, std::move(packed_tensor), status);
  if (TF_GetCode(status) != TF_OK) return nullptr;
  return output;
}

TFE_TensorHandle* DTensorDevice::SparsePack(
    TFE_Context* context, int num_inputs, TFE_TensorHandle** indices,
    TFE_TensorHandle** values, TFE_TensorHandle** shapes,
    const std::string& string_layout, TF_Status* status) {
  StatusOr<Layout> target_layout = Layout::FromString(string_layout);
  if (!target_layout.ok()) {
    TF_SetStatus(status, TF_INVALID_ARGUMENT,
                 "Failed to parse layout from string layout");
    return nullptr;
  }
  const Mesh& target_mesh = target_layout->mesh();
  const MeshWithParallelDevice* target_parallel_device =
      mesh_to_device_map_[target_mesh].get();
  if (target_parallel_device == nullptr) {
    TF_SetStatus(status, TF_INVALID_ARGUMENT,
                 absl::StrCat("Required mesh : ", target_mesh.ToString(),
                              "is not registered with DTensor ")
                     .c_str());
    return nullptr;
  }

  TF_DataType tf_int64 = TF_INT64;
  // Verify rank and dtype of shapes.
  VerifyTensorRankAndDType(shapes, num_inputs, /*expected_rank=*/1,
                           /*expected_dtype=*/&tf_int64, status);
  if (TF_GetCode(status) != TF_OK) return nullptr;

  // Verify rank and dtype of indices.
  VerifyTensorRankAndDType(indices, num_inputs, /*expected_rank=*/2,
                           /*expected_dtype=*/&tf_int64, status);
  if (TF_GetCode(status) != TF_OK) return nullptr;

  // Verify rank of values.
  VerifyTensorRankAndDType(values, num_inputs, /*expected_rank=*/1,
                           /*expected_dtype=*/nullptr, status);
  if (TF_GetCode(status) != TF_OK) return nullptr;

  // Compute the local shape from a shape tensor.
  std::unique_ptr<TF_Tensor, decltype(&TF_DeleteTensor)> shape_tensor(
      TFE_TensorHandleResolve(shapes[0], status), TF_DeleteTensor);
  if (TF_GetCode(status) != TF_OK) {
    TF_SetStatus(
        status, TF_GetCode(status),
        absl::StrCat("Error resolving the tensor handle of shape tensor"
                     ". Original message: ",
                     TF_Message(status))
            .c_str());
    return nullptr;
  }
  int shape_tensor_size = TFE_TensorHandleDim(shapes[0], 0, status);
  if (TF_GetCode(status) != TF_OK || shape_tensor_size <= 0) {
    TF_SetStatus(status, TF_GetCode(status),
                 absl::StrCat("Error computing the num dims of shape tensor",
                              TF_Message(status))
                     .c_str());
    return nullptr;
  }

  const int64_t* data =
      static_cast<int64_t*>(TF_TensorData(shape_tensor.get()));
  std::vector<int64_t> local_shape(data, data + shape_tensor_size);
  if (local_shape.size() != target_layout->rank()) {
    TF_SetStatus(
        status, TF_INVALID_ARGUMENT,
        absl::StrCat(
            "Packed layout should have the same rank as the rank for each "
            "component. The rank of each component is: ",
            local_shape.size(),
            ", while layout has rank: ", target_layout->rank(),
            "\nLayout: ", target_layout->ToString(), "\n")
            .c_str());
    return nullptr;
  }

  // Create the SparseTensorWithLayout.
  std::unique_ptr<TensorWithLayout> packed_tensor;
  if (is_remote_mesh(target_parallel_device->mesh_config())) {
    // Create a dummy SparseTensorWithLayout.
    packed_tensor = SparseTensorWithLayout::Dummy(
        local_shape, target_parallel_device->mesh_config(),
        target_layout.value());
  } else {
    // Parse the indices, values, and dense_shape tensors and put them into
    // parallel tensors, and then pack it into a single SparseTensorWithLayout.
    auto local_devices = target_parallel_device->mesh_config().local_devices();

    std::vector<parallel_device::TensorHandlePtr> indices_components;
    std::vector<parallel_device::TensorHandlePtr> values_components;
    std::vector<parallel_device::TensorHandlePtr> dense_shapes_components;

    // Just a nice trick to make code cleaner to pack each of indices, values,
    // shapes.
    std::vector<std::vector<parallel_device::TensorHandlePtr>*> components{
        &indices_components, &values_components, &dense_shapes_components};
    std::vector<TFE_TensorHandle**> input_vectors{indices, values, shapes};
    for (int component_index = 0; component_index < 3; ++component_index) {
      components[component_index]->reserve(num_inputs);
      TFE_TensorHandle** inputs = input_vectors[component_index];
      for (int i = 0; i < num_inputs; ++i) {
        const char* input_device =
            TFE_TensorHandleDeviceName(inputs[i], status);
        if (TF_GetCode(status) != TF_OK) return nullptr;
        if (name_ == input_device) {
          TF_SetStatus(status, TF_INVALID_ARGUMENT,
                       "Does not support packing a Tensor that is already on "
                       "dtensor device.");
          return nullptr;
        }

        components[component_index]->emplace_back(TFE_TensorHandleCopyToDevice(
            inputs[i], context, local_devices[i].c_str(), status));
        if (TF_GetCode(status) != TF_OK) return nullptr;
      }
    }
    std::unique_ptr<parallel_device::ParallelTensor> parallel_indices_tensor =
        parallel_device::ParallelTensor::FromTensorHandles(
            target_parallel_device->parallel_device(),
            std::move(indices_components), status);

    std::unique_ptr<parallel_device::ParallelTensor> parallel_values_tensor =
        parallel_device::ParallelTensor::FromTensorHandles(
            target_parallel_device->parallel_device(),
            std::move(values_components), status);

    std::unique_ptr<parallel_device::ParallelTensor>
        parallel_dense_shapes_tensor =
            parallel_device::ParallelTensor::FromTensorHandles(
                target_parallel_device->parallel_device(),
                std::move(dense_shapes_components), status);

    if (TF_GetCode(status) != TF_OK) return nullptr;
    packed_tensor =
        SparseTensorWithLayout::Wrap(std::move(parallel_indices_tensor),
                                     std::move(parallel_values_tensor),
                                     std::move(parallel_dense_shapes_tensor),
                                     target_parallel_device->mesh_config(),
                                     target_layout.value(), local_shape)
            .value();
  }

  RecordInShapeLayoutCache(*packed_tensor);
  TFE_TensorHandle* output =
      MakeLayoutTensorHandle(context, std::move(packed_tensor), status);
  if (TF_GetCode(status) != TF_OK) return nullptr;
  return output;
}

bool DTensorDevice::IsSparseDTensor(TFE_Context* context,
                                    TFE_TensorHandle* input,
                                    TF_Status* status) {
  const char* input_device = TFE_TensorHandleDeviceName(input, status);
  if (input_device != name_) {
    TF_SetStatus(
        status, TF_INVALID_ARGUMENT,
        "DTensorSparseUnpack expects a tensor placed on the DTensor device.");
    return false;
  }
  TensorWithLayout* t = reinterpret_cast<TensorWithLayout*>(
      TFE_TensorHandleDevicePointer(input, status));
  if (TF_GetCode(status) != TF_OK) return false;
  return t->tensor_type() == TensorType::kSparse;
}

Status DTensorDevice::UpdateOutputLayoutsWithSameShapePolicy(
    const std::vector<PartialTensorShape>& global_output_shapes,
    const absl::flat_hash_set<Mesh>& input_meshes, absl::string_view op_name,
    tensorflow::Graph* graph, std::vector<const Layout*>* output_layouts) {
  if (!same_shape_policy_enabled_) return OkStatus();
  // Simply do not hint if inputs span across multiple meshes.
  if (input_meshes.size() > 1) return OkStatus();

  for (Node* node : graph->op_nodes()) {
    if (!node->IsRetval()) {
      continue;
    }
    int output_index;
    TF_RETURN_IF_ERROR(GetNodeAttr(node->attrs(), "index", &output_index));
    if (output_layouts->at(output_index)) {
      continue;
    }

    const auto& global_output_shape = global_output_shapes.at(output_index);
    const Layout* layout = nullptr;
    // TODO(b/180022708): This is useful information, we should be
    // able to hint to layout propagation without making it a hard
    // requirement
    //
    // Special cases at the moment:
    // - Relayout needs an exemption.
    // - VarHandleOp does not need hint. VarHandleOp has scalar shape so layout
    //   is trivial. On the other hande, downstream system "thinks' Variable has
    //   shape same as the pointing value. So, providing a layout based on
    //   VarHandleOp (scalar) might confuse the downstream system.
    // - CopyToMesh has a user-supplied layout that is propagated downstream.
    if (op_name != std::string("Relayout") &&
        op_name != std::string("VarHandleOp") &&
        op_name != std::string("CopyToMesh")) {
      // TODO(b/162009702): Support matching between partially-known shapes.
      if (global_output_shape.IsFullyDefined()) {
        gtl::InlinedVector<int64, 4> shape_vector(
            global_output_shape.dim_sizes());
        auto layout_iterator =
            shape_layout_cache_.find(FingerprintShape(shape_vector));
        if (layout_iterator != shape_layout_cache_.end() &&
            layout_iterator->second.is_unique) {
          // We have a cached layout for this shape. Send it to MLIR.
          layout = &layout_iterator->second.layout;
          VLOG(3) << op_name << ": found a cached layout for shape "
                  << global_output_shape.DebugString() << ": \""
                  << layout->ToString() << "\"";
          if (input_meshes.empty() &&
              layout->mesh() != default_mesh_->mesh_config()) {
            VLOG(3) << "But we can't infer a input mesh and cached layout: "
                    << "mesh \"" << (layout->mesh().ToString()) << " "
                    << "is different than the default mesh : \""
                    << default_mesh_->mesh_config().ToString() << "\"\n"
                    << "Not applying the cached layout.";
          } else if (!input_meshes.empty() &&
                     layout->mesh() != *input_meshes.begin()) {
            VLOG(3)
                << "But the layout mesh is different than the executing mesh: "
                << "\"" << (*input_meshes.begin()).ToString() << "\"\n"
                << "Not applying the cached layout.";
          } else {
            (*output_layouts)[output_index] = layout;
            node->AddAttr(kDefaultLayoutAttr, layout->ToString());
          }
        } else if (layout_iterator == shape_layout_cache_.end()) {
          VLOG(3) << op_name << ": no cached layout found for shape "
                  << global_output_shape.DebugString();
        } else {
          VLOG(3) << op_name << ": found multiple layouts for shape "
                  << global_output_shape.DebugString();
        }
      } else {
        VLOG(3) << op_name
                << ": not applying same-shape-same-layout due to "
                   "not-fully-known shape "
                << global_output_shape.DebugString();
      }
    }
  }
  return OkStatus();
}

std::unordered_map<std::string, int> DTensorDevice::GetFunctionCacheStats(
    TFE_Context* context, TF_Status* status) const {
  const auto stats = function_manager_->GetStats();
  return {{"hit", stats.hits}, {"miss", stats.misses}, {"size", stats.size}};
}

void DTensorDevice::SetIteratorElementLayouts(
    TFE_Context* context, TFE_TensorHandle* input,
    const std::vector<std::string>& string_layouts, TF_Status* status) {
  const char* input_device = TFE_TensorHandleDeviceName(input, status);
  if (input_device != name_) {
    RETURN_STATUS(
        status, TF_INVALID_ARGUMENT,
        absl::StrCat(
            "SetIteratorElementLayouts expects an iterator resource placed on ",
            "the DTensor device: ", name_,
            ", but it was placed on device: ", input_device)
            .c_str());
  }
  ResourceHandleWithLayout* t = reinterpret_cast<ResourceHandleWithLayout*>(
      TFE_TensorHandleDevicePointer(input, status));
  if (TF_GetCode(status) != TF_OK) return;

  std::vector<Layout> layouts;
  std::transform(string_layouts.cbegin(), string_layouts.cend(),
                 std::back_inserter(layouts),
                 [](const std::string& layout_str) {
                   return Layout::FromString(layout_str).value();
                 });
  RETURN_C_STATUS_IF_NOT_OK(t->UpdateElementLayouts(layouts), status);
}

// From `graph` containing computation for all meshes, extract/select
// computation for mesh specified in `function`. Returned graph is a cloned
// graph with ops only for single mesh execution.
StatusOr<std::unique_ptr<Graph>> SelectGraphToExecute(
    const TranslatedFunction& function, const Graph& graph,
    std::string* stateful_partitioned_call_name) {
  auto new_graph = std::make_unique<Graph>(graph.flib_def());
  CopyGraph(graph, new_graph.get());
  std::vector<Node*> arg_nodes;
  std::vector<Node*> retval_nodes;
  for (Node* node : new_graph->nodes()) {
    if (node->IsArg()) arg_nodes.emplace_back(node);
    if (node->IsRetval()) retval_nodes.emplace_back(node);
  }

  // Remove irrelevant function calls.
  for (Node* node : new_graph->nodes()) {
    if (node->op_def().name() != "StatefulPartitionedCall") continue;

    if (node->name() != function.node_to_execute->name()) {
      // Remove function call that does not match mesh specification and all
      // output retval nodes connected to the function call node.
      std::queue<Node*> nodes_to_remove;
      nodes_to_remove.push(node);
      while (!nodes_to_remove.empty()) {
        Node* n = nodes_to_remove.front();
        for (const Edge* out_edge : n->out_edges()) {
          if (out_edge->IsControlEdge()) continue;
          Node* out_node = out_edge->dst();
          if (!out_node->IsSink()) nodes_to_remove.push(out_node);
        }
        if (n->IsRetval()) {
          auto pos = std::find(retval_nodes.begin(), retval_nodes.end(), n);
          TF_RET_CHECK(pos != retval_nodes.end());
          retval_nodes.erase(pos);
        }
        nodes_to_remove.pop();
        new_graph->RemoveNode(n);
      }
    }
  }

  *stateful_partitioned_call_name = function.node_to_execute->name();
  VLOG(1) << "Selected call " << *stateful_partitioned_call_name;

  // Remove unused arg nodes in graph.
  for (auto it = arg_nodes.begin(); it != arg_nodes.end(); it++) {
    Node* arg_node = *it;
    bool arg_unused = true;
    for (const Edge* e : arg_node->out_edges()) {
      if (e->dst()->IsOp()) {
        arg_unused = false;
      }
    }
    if (!arg_unused) continue;

    new_graph->RemoveNode(arg_node);
    arg_nodes.erase(it--);
  }

  // Reset index attributes for arg and retval nodes.
  for (Node* n : new_graph->nodes()) {
    // Reset arg node index attributes to its position within all the arg
    // nodes. This should just be increasing from 0 to n where n
    // is the total number of arguments. Note that this definition to
    // the `index` attribute is different from the definition we set in
    // PrepareGraphForMLIR.
    // This attribute is needed for each arg node when converting a Graph to
    // a FunctionDef.
    if (n->IsArg()) {
      auto pos = std::find(arg_nodes.begin(), arg_nodes.end(), n);
      TF_RET_CHECK(pos != arg_nodes.end());
      const int new_index = std::distance(arg_nodes.begin(), pos);
      n->AddAttr("index", new_index);
    }

    // Reset retval nodes index attributes.
    if (n->IsRetval()) {
      auto retval_pos = std::find(retval_nodes.begin(), retval_nodes.end(), n);
      TF_RET_CHECK(retval_pos != retval_nodes.end());
      const int new_index = std::distance(retval_nodes.begin(), retval_pos);
      n->AddAttr("index", new_index);
    }
  }

  VLOG(4) << tensorflow::DumpGraphToFile("selected_graph_to_execute_",
                                         *new_graph);

  return new_graph;
}

// Adds processed graph to run for each mesh computation in
// `execution_functions` to function definition library.
Status AddExecutionFunctionDefsToFunctionDefLibrary(
    const absl::flat_hash_set<Node*>& control_ret_nodes, TFE_Context* context,
    const Graph& graph, ExecutionFunctions* execution_functions) {
  // Note: We use node name instead of node pointer for comparison because
  // node address in the new graph is different with the original graph.
  absl::flat_hash_set<std::string> control_ret_names;
  for (auto* n : control_ret_nodes) {
    control_ret_names.emplace(n->name());
  }
  for (TranslatedFunction& function : execution_functions->function_list) {
    std::string selected_call_node_name;
    // TODO(bfontain): We should just try to call the functions directly rather
    // than wrap
    // Construct graph that executes only computation for `function`.
    TF_ASSIGN_OR_RETURN(
        std::unique_ptr<Graph> new_graph,
        SelectGraphToExecute(function, graph, &selected_call_node_name));
    VLOG(4) << tensorflow::DumpGraphToFile("selected_graph_", *new_graph);

    // Add unique identifier based on the function we are executing to the
    // function/graph and convert graph to functiondef.
    NameAttrList func;
    TF_RETURN_IF_ERROR(
        GetNodeAttr(function.node_to_execute->attrs(), "f", &func));

    static std::atomic<int64_t> unique_function_number(0);
    function.translated_function_name =
        absl::StrCat(func.name(), "_", unique_function_number.fetch_add(1));
    auto control_ret_node_names =
        [&control_ret_names, &selected_call_node_name](
            const Node* node) -> std::optional<std::string> {
      // Add the stateful partitioned call node as a control return as we need
      // to process any control deps inside the inner function.
      if (control_ret_names.contains(node->name()) ||
          node->name() == selected_call_node_name) {
        return node->name();
      }
      return std::nullopt;
    };

    tensorflow::FunctionDef to_run;
    TF_RETURN_IF_ERROR(tensorflow::GraphToFunctionDef(
        *new_graph, function.translated_function_name, control_ret_node_names,
        &to_run));

    for (const auto& out : to_run.signature().output_arg()) {
      function.output_dtypes.emplace_back(static_cast<TF_DataType>(out.type()));
    }

    AddDTensorFunctionAttr(to_run);
    TF_RETURN_IF_ERROR(tensorflow::unwrap(context)->AddFunctionDef(to_run));
  }

  return OkStatus();
}

StatusOr<DTensorDevice::DTensorOperationLoweringContext>
DTensorDevice::DTensorOperationToModule(
    TFE_Context* context, const std::vector<TensorWithLayout*>& inputs,
    const DTensorOperation& doperation, const NameAttrList& eager_attributes) {
  profiler::TraceMe activity(
      [&] { return "DTensorDevice::DTensorOperationToModule"; },
      profiler::TraceMeLevel::kInfo);
  FunctionLibraryDefinition* flib_def =
      tensorflow::unwrap(context)->FuncLibDef();
  DTensorOperationLoweringContext result;
  result.graph = std::make_unique<tensorflow::Graph>(flib_def);

  const FunctionDef* function_def = doperation.function_def;
  if (!function_def) {
    // Output layouts of an eager op (e.g. fill) must be inferred before cache
    // key computation, since they might depend on the current DTensorDevice
    // state.
    TF_RETURN_IF_ERROR(PrepareGraphForMlir(
        *function_manager_, inputs, doperation, *flib_def, eager_attributes,
        default_layout_, result.graph.get(), &result.global_output_shapes,
        &result.output_layouts));

    // Finds all meshes the inputs are lied on.
    absl::flat_hash_set<Mesh> input_meshes;
    for (const TensorWithLayout* tensor : inputs) {
      if (!tensor->layout().mesh().IsEmpty()) {
        input_meshes.insert(tensor->layout().mesh());
      }
    }
    // Currently we only provide layout hints for op-by-op, since
    // they interact badly with layout propagation.
    TF_RETURN_IF_ERROR(UpdateOutputLayoutsWithSameShapePolicy(
        result.global_output_shapes, input_meshes, doperation.name,
        result.graph.get(), &result.output_layouts));
  }

  auto [cache_key, cached_mlir_module] = module_manager_->GetCachedExecutable(
      doperation, eager_attributes, inputs, result.output_layouts);
  result.doperation_cache_key = cache_key;

  if (cached_mlir_module != nullptr) {
    result.module = **cached_mlir_module;
    return result;
  } else if (function_def) {
    LOG(INFO) << "DTensor cache key lookup missed for " << doperation.name
              << ". DTensor is (re-)computing its SPMD transformation.";
  }

  // It includes remote devices when the coordination service is enabled.
  result.tf_devices = tensorflow::unwrap(context)->ListAllTfDevices();
  DeviceSet device_set;
  for (const auto device : result.tf_devices) device_set.AddDevice(device);

  if (function_def) {
    // Output layouts of a function are inferred by MLIR lowering. They are
    // not necessary for cache key computation, so run PrepareGraphForMlir after
    // cache key computation to reduce the overheads of running the same
    // function multiple times.
    TF_RETURN_IF_ERROR(PrepareGraphForMlir(
        *function_manager_, inputs, doperation, *flib_def, eager_attributes,
        default_layout_, result.graph.get(), &result.global_output_shapes,
        &result.output_layouts));
  }

  VLOG(4) << tensorflow::DumpGraphToFile("after_prepare_for_mlir",
                                         *result.graph, flib_def);

  // Converts Graph to MLIR Module.
  TF_ASSIGN_OR_RETURN(
      mlir::OwningOpRef<mlir::ModuleOp> mlir_module_ref,
      pass_runner_.ImportGraphToMlir(
          device_set, doperation.is_func(), doperation.default_mesh, *flib_def,
          *result.graph, result.doperation_cache_key));

  cached_mlir_module = module_manager_->AddCachedExecutable(
      cache_key, mlir_module_ref.release());
  result.module = **cached_mlir_module;
  return result;
}

void DTensorDevice::ModuleToExecutionFunctions(
    TFE_Context* context, const std::vector<TensorWithLayout*>& inputs,
    const DTensorOperation& doperation, const NameAttrList& eager_attributes,
    int num_outputs, DTensorOperationLoweringContext& lowering_context,
    const ExecutionFunctions** execution_functions, TF_Status* status) {
  profiler::TraceMe activity(
      [&] { return "DTensorDevice::ModuleToExecutionFunctions"; },
      profiler::TraceMeLevel::kInfo);
  FunctionLibraryDefinition* flib_def =
      tensorflow::unwrap(context)->FuncLibDef();
  const FunctionDef* function_def = doperation.function_def;

  const ExecutionFunctions* cached_function;

  cached_function = function_manager_->GetCachedExecutableSimple(
      lowering_context.doperation_cache_key);
  if (cached_function != nullptr) {
    *execution_functions = cached_function;
    return;
  } else {
    if (doperation.is_func()) {
      LOG(INFO) << "DTensor cache key lookup missed for " << doperation.name
                << ". DTensor is (re-)computing its ExecutionFunctions.";
    }
  }

  // Transforms ModuleOp and extracts ExecutionFunctions from lowered ModuleOp.
  if (!lowering_context.module.has_value()) {
    RETURN_STATUS(status, TF_INVALID_ARGUMENT,
                  "ModuleOp for ExecutionFunctions extraction is missing.");
  }
  {
    profiler::TraceMe activity([&] { return "DTensorDevice::RunMLIRPasses"; },
                               profiler::TraceMeLevel::kInfo);
    RETURN_C_STATUS_IF_NOT_OK(pass_runner_.Run(*lowering_context.module),
                              status);
  }
  // Converts MLIR to GraphDef and merges to the global Graph.
  absl::flat_hash_set<Node*> control_ret_nodes;
  GraphExportConfig export_config;
  RETURN_C_STATUS_IF_NOT_OK(
      ConvertMlirToGraph(*lowering_context.module, export_config,
                         &(lowering_context.graph), flib_def,
                         &control_ret_nodes),
      status);
  Graph* graph = lowering_context.graph.get();
  VLOG(4) << DumpGraphToFile("after_dtensor_mlir_pass", *graph, flib_def);

  if (flib_def->Contains(kLoadEmbeddingFn)) {
    Status s = InsertFunctionForTPUEmbeddingCheckpoint(status, graph, inputs,
                                                       kLoadEmbeddingFn);
    RETURN_C_STATUS_IF_NOT_OK(s, status);
  }

  // After MLIR transformations, exactly one StatefulPartitionedCall op is
  // returned for mesh cluster in computation. Identity all functions to execute
  // for each mesh and relevant input and output information.
  ASSIGN_OR_RETURN_C_STATUS(
      ExecutionFunctions functions,
      IdentifyAllFunctionsToExecute(*lowering_context.graph,
                                    lowering_context.global_output_shapes),
      status);

  // In order to ensure that all resource assign operations as well as side
  // effecting ops are executed, we add identity ops before function outputs
  // with control rets.
  RETURN_C_STATUS_IF_NOT_OK(MaybeInsertIdentityNodes(function_def, graph),
                            status);

  VLOG(4) << tensorflow::DumpGraphToFile("after_post_processing_graph",
                                         *lowering_context.graph, flib_def);

  RETURN_C_STATUS_IF_NOT_OK(
      AddExecutionFunctionDefsToFunctionDefLibrary(
          control_ret_nodes, context, *lowering_context.graph, &functions),
      status);
  functions.num_device_ids = 1;
  if (function_def) {
    for (TranslatedFunction& function : functions.function_list) {
      functions.function_mesh_fingerprint =
          FingerprintCat64(functions.function_mesh_fingerprint,
                           function.function_mesh.GlobalFingerprint());
    }
  }

  *execution_functions = function_manager_->AddCachedExecutable(
      lowering_context.doperation_cache_key, std::move(functions));
}

void DTensorDevice::ExecuteFunctionAndWait(
    TFE_Context* context, const TranslatedFunction* function_ptr,
    const MeshWithParallelDevice* parallel_device_mesh,
    const std::vector<parallel_device::ParallelTensor*>& parallel_inputs,
    const int64_t step_id, const TFE_OpAttrs* attributes, TF_Status* status) {
  const std::string mesh_str = function_ptr->function_mesh.ToString();
  VLOG(4) << "Launching computation for mesh : " << mesh_str;
  parallel_device_mesh->parallel_device().StartExecute(
      context,
      /*inputs=*/parallel_inputs,
      /*operation_name=*/function_ptr->translated_function_name.c_str(),
      /*attributes=*/attributes,
      /*expected_max_outputs=*/function_ptr->local_output_shapes.size(),
      /*cancellation_manager=*/*cancellation_manager_,
      /*step_id=*/step_id);

  VLOG(4) << "Joining computation result from mesh : " << mesh_str;
  parallel_device_mesh->parallel_device().Join(
      function_ptr->local_output_shapes, status);
  VLOG(4) << "Joining status: " << TF_Message(status);
  if (TF_GetCode(status) != TF_OK && TF_GetCode(status) != TF_CANCELLED) {
    LOG(ERROR) << "Encountered error while executing function: "
               << function_ptr->translated_function_name
               << " for mesh : " << mesh_str
               << " / error : " << TF_Message(status);
  }

  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> async_wait_status(
      TF_NewStatus(), TF_DeleteStatus);
  AsyncWait(context, async_wait_status.get());
  TF_Code error_code = TF_GetCode(async_wait_status.get());
  if (error_code != TF_OK && error_code != TF_CANCELLED) {
    LOG(ERROR) << "Async status: " << TF_Message(async_wait_status.get());
  }
}

void DTensorDevice::ParallelExecuteRegularOperation(
    TFE_Context* context, const std::vector<TensorWithLayout*>& inputs,
    mlir::ModuleOp mlir_module, const DTensorOperation& doperation,
    const TFE_OpAttrs* attributes, int* num_outputs, TFE_TensorHandle** outputs,
    TF_Status* status) {
  auto future_result =
      parallel_executor_->Execute(context, inputs, mlir_module,
                                  /*entry_function_name=*/"main", attributes);
  auto result_with_status = future_result.Await();

  std::vector<TensorWithLayout*> typed_outputs;
  ASSIGN_OR_RETURN_C_STATUS(typed_outputs, result_with_status, status);
  // assign outputs and take outputs' ownership
  *num_outputs = typed_outputs.size();
  for (int i = 0; i < *num_outputs; ++i) {
    outputs[i] = MakeLayoutTensorHandle(
        context, absl::WrapUnique(typed_outputs[i]), status);
  }
}

void DTensorDevice::ExecuteRegularOperation(
    TFE_Context* context, const std::vector<TensorWithLayout*>& inputs,
    const DTensorOperation& doperation, const TFE_OpAttrs* attributes,
    int* num_outputs, TFE_TensorHandle** outputs, TF_Status* status) {
  ASSIGN_OR_RETURN_C_STATUS(auto eager_attributes, FetchAttributes(attributes),
                            status);

  ASSIGN_OR_RETURN_C_STATUS(
      auto lowering_context,
      DTensorOperationToModule(context, inputs, doperation, eager_attributes),
      status);

  if (parallel_executor_) {
    if (!lowering_context.module.has_value()) {
      RETURN_STATUS(status, TF_INTERNAL,
                    "ParallelExecutor is enabled but ModuleOp is missing.");
    }
    ParallelExecuteRegularOperation(context, inputs, *lowering_context.module,
                                    doperation, attributes, num_outputs,
                                    outputs, status);
    return;
  }

  std::vector<TensorWithLayoutTf*> inputs_tf;
  inputs_tf.reserve(inputs.size());
  for (const auto& input : inputs) {
    inputs_tf.push_back(llvm::cast<TensorWithLayoutTf>(input));
  }

  const ExecutionFunctions* execution_functions = nullptr;
  ModuleToExecutionFunctions(context, inputs, doperation, eager_attributes,
                             *num_outputs, lowering_context,
                             &execution_functions, status);

  if (TF_GetCode(status) != TF_OK) return;

  // Update input layouts for resource arguments.
  for (const TranslatedFunction& function :
       execution_functions->function_list) {
    for (const auto& entry : function.resource_input_layouts) {
      // TODO(hthu): Add a TensorWithLayout in the inputs vector at location 0
      // for DeviceId. This is done as the first arg is always DeviceId, and it
      // isn't mapped to input Tensors.
      const int resource_index_to_update = entry.first - 1;
      const Status s =
          llvm::cast<ResourceHandleWithLayout>(inputs[resource_index_to_update])
              ->UpdateLayout(entry.second);
      if (!s.ok()) {
        RETURN_STATUS(status, static_cast<TF_Code>(s.code()),
                      absl::StrCat("Attempt to update layout input arg: ",
                                   resource_index_to_update,
                                   ". Original message: ", s.ToString())
                          .c_str());
      }
    }
  }

  int num_global_outputs = 0;

  std::map<std::string, const MeshWithParallelDevice*>
      function_name_and_mesh_mapping;
  absl::flat_hash_set<std::string> excluded_fn_names;
  std::unique_ptr<const TranslatedFunction> epu_fn_ptr, load_embedding_ptr;
  for (const TranslatedFunction& function :
       execution_functions->function_list) {
    StatusOr<Mesh> maybe_converted_mesh = function.function_mesh;
    if (function.function_mesh.is_epu_mesh()) {
      maybe_converted_mesh = function.function_mesh.ToDeviceType("CPU");
    }

    if (!maybe_converted_mesh.ok()) {
      RETURN_STATUS(status, TF_INVALID_ARGUMENT,
                    absl::StrCat("Failed to convert mesh, get error: ",
                                 maybe_converted_mesh.status().error_message())
                        .c_str());
    }
    const Mesh& mesh = *maybe_converted_mesh;
    const MeshWithParallelDevice* parallel_device_mesh =
        mesh_to_device_map_.contains(mesh) ? mesh_to_device_map_[mesh].get()
                                           : default_mesh_;
    if (parallel_device_mesh == nullptr) {
      RETURN_STATUS(status, TF_INTERNAL,
                    "required mesh is not registered with DTensor device");
    }
    function_name_and_mesh_mapping[function.translated_function_name] =
        parallel_device_mesh;

    if (function.function_mesh.is_epu_mesh()) {
      if (epu_fn_ptr != nullptr) {
        RETURN_STATUS(status, TF_INTERNAL,
                      "There are more than one function defined on EPU mesh.");
      }
      epu_fn_ptr = std::make_unique<const TranslatedFunction>(function);
      excluded_fn_names.insert(function.translated_function_name);
    }
    if (absl::StartsWith(function.translated_function_name, kLoadEmbeddingFn)) {
      if (load_embedding_ptr != nullptr) {
        RETURN_STATUS(status, TF_INTERNAL,
                      "There are more than one function defined on EPU mesh.");
      }
      load_embedding_ptr = std::make_unique<const TranslatedFunction>(function);
      excluded_fn_names.insert(function.translated_function_name);
    }
  }

  // Compute the step_id based on the function_mesh_fingerprint and the
  // corresponding function execution counter.
  uint64 function_mesh_fingerprint =
      execution_functions->function_mesh_fingerprint;
  if (func_mesh_fingerprint_to_step_counter_.contains(
          function_mesh_fingerprint)) {
    func_mesh_fingerprint_to_step_counter_.at(function_mesh_fingerprint)++;
  } else {
    func_mesh_fingerprint_to_step_counter_.insert(
        {function_mesh_fingerprint, 0});
  }
  const uint64 step_id = FingerprintCat64(
      function_mesh_fingerprint,
      func_mesh_fingerprint_to_step_counter_.at(function_mesh_fingerprint));

  // Execute excluded functions in sequence.
  if (epu_fn_ptr != nullptr) {
    ExecuteFunctionAndWait(
        context,
        /*function_ptr=*/epu_fn_ptr.get(),
        /*parallel_device_mesh=*/
        function_name_and_mesh_mapping[epu_fn_ptr->translated_function_name],
        /*parallel_inputs=*/{}, /*step_id=*/step_id, /*attributes=*/attributes,
        /*status=*/status);
  }

  if (load_embedding_ptr != nullptr) {
    StatusOr<std::vector<parallel_device::ParallelTensor*>> parallel_inputs =
        PrepareEmbeddingInputs(inputs_tf);
    if (!parallel_inputs.ok()) {
      RETURN_STATUS(status, TF_INTERNAL,
                    parallel_inputs.status().error_message().c_str());
    }
    ExecuteFunctionAndWait(
        context,
        /*function_ptr=*/load_embedding_ptr.get(),
        /*parallel_device_mesh=*/
        function_name_and_mesh_mapping[load_embedding_ptr
                                           ->translated_function_name],
        /*parallel_inputs=*/*parallel_inputs, /*step_id=*/step_id,
        /*attributes=*/attributes, /*status=*/status);
  }

  // Extract the global parallel inputs and flatten SparseTensors
  // into the three component tensors.
  std::vector<parallel_device::ParallelTensor*> global_parallel_inputs;
  std::vector<parallel_device::ParallelTensor*> global_parallel_sparse_inputs;
  absl::flat_hash_set<int> global_sparse_input_indices;
  for (auto input : inputs_tf) {
    if (auto* sparse_input = llvm::dyn_cast<SparseTensorWithLayout>(input);
        sparse_input) {
      global_parallel_sparse_inputs.push_back(sparse_input->indices());
      global_parallel_sparse_inputs.push_back(sparse_input->dense_shapes());
      global_parallel_sparse_inputs.push_back(sparse_input->values());
    } else {
      global_parallel_inputs.push_back(input->tensor());
    }
  }
  // Insert SparseTensor components to the end, this is because
  // in the MLIR handling of SparseTensors, we place SparseTensor components
  // to the end of the main func arguments for a fixed ordering.
  global_parallel_inputs.insert(global_parallel_inputs.end(),
                                global_parallel_sparse_inputs.begin(),
                                global_parallel_sparse_inputs.end());

  // Execute all functions in parallel.
  for (const TranslatedFunction& function :
       execution_functions->function_list) {
    const Mesh& mesh = function.function_mesh;
    const std::string& translated_function_name =
        function.translated_function_name;

    num_global_outputs += function.local_output_shapes.size();

    if (is_remote_mesh(mesh) ||
        (excluded_fn_names.find(translated_function_name) !=
         excluded_fn_names.end())) {
      // Skip execution for a translated function has remote mesh or when it is
      // excluded.
      continue;
    }

    const MeshWithParallelDevice* parallel_device_mesh =
        function_name_and_mesh_mapping[translated_function_name];

    // Gather the local inputs for this function.
    std::vector<parallel_device::ParallelTensor*> parallel_inputs;
    parallel_inputs.reserve(inputs.size() + 1);
    auto input_mapping = function.input_index_map;

    // We sort here because by this time, the function graph we are executing
    // is a reduced version of the main function, that includes the
    // StatefulPartitionedCall that we are executing for this mesh.
    // Thus, the ordering is the same as the main function ordering, which
    // is sorted increasingly.
    std::sort(input_mapping.begin(), input_mapping.end());

    for (const int global_index : input_mapping) {
      auto input_index = global_index - execution_functions->num_device_ids;

      if (global_index < execution_functions->num_device_ids) {
        parallel_inputs.push_back(
            parallel_device_mesh->DeviceIDs(context, status));
        if (TF_GetCode(status) != TF_OK) return;
      } else {
        parallel_inputs.push_back(global_parallel_inputs[input_index]);
      }
    }

    VLOG(4) << "Launching computation for mesh : " << mesh.ToString();
    parallel_device_mesh->parallel_device().StartExecute(
        context, parallel_inputs, translated_function_name.c_str(), attributes,
        /*expected_max_outputs=*/function.local_output_shapes.size(),
        *cancellation_manager_, /*step_id=*/step_id);
  }

  *num_outputs = num_global_outputs;
  std::vector<std::unique_ptr<TensorWithLayout>> typed_outputs;
  typed_outputs.resize(num_global_outputs);

  // Join all mesh computation together.
  // TODO(b/177932563): Expose cancel logic to handle failures.
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> join_status(
      TF_NewStatus(), TF_DeleteStatus);
  for (const TranslatedFunction& function :
       execution_functions->function_list) {
    // Skip execution for a function when it's excluded.
    if (excluded_fn_names.contains(function.translated_function_name)) {
      continue;
    }
    const Mesh& mesh = function.function_mesh;
    const MeshWithParallelDevice* parallel_device_mesh =
        function_name_and_mesh_mapping[function.translated_function_name];

    std::vector<std::unique_ptr<TensorWithLayout>> output_with_layout;
    output_with_layout.reserve(function.output_index_map.size());
    if (is_remote_mesh(mesh)) {
      // Create dummy outputs on a remote mesh.
      for (int i = 0; i < function.output_index_map.size(); ++i) {
        const auto dim_sizes = function.local_output_shapes.at(i).dim_sizes();
        std::vector<int64_t> local_shape =
            std::vector<int64_t>(dim_sizes.begin(), dim_sizes.end());
        TF_DataType dtype =
            static_cast<TF_DataType>(function.output_dtypes.at(i));
        auto remote_output = CreateDummyTensorWithLayout(
            local_shape, dtype, parallel_device_mesh->mesh_config(),
            function.output_layouts[i]);
        output_with_layout.push_back(std::move(remote_output));
      }
    } else {
      VLOG(4) << "Joining computation result from mesh : " << mesh.ToString();
      auto result = parallel_device_mesh->parallel_device().Join(
          function.local_output_shapes, status);
      if (TF_GetCode(join_status.get()) != TF_OK &&
          // Preserve the first failure we see, but only if it is a real failure
          // and not a cancellation (which was probably triggered by the error
          // we want to propagate).
          (TF_GetCode(status) == TF_OK ||
           TF_GetCode(join_status.get()) != TF_CANCELLED)) {
        continue;
      }
      if (TF_GetCode(status) != TF_OK) {
        if (TF_GetCode(status) != TF_CANCELLED) {
          LOG(ERROR) << "Encountered error while executing function: "
                     << function.translated_function_name
                     << " for mesh : " << mesh.ToString()
                     << " / error : " << TF_Message(status);
        }
        TF_SetStatus(join_status.get(), TF_GetCode(status), TF_Message(status));
        continue;
      }

      for (int i = 0; i < result->size(); ++i) {
        ASSIGN_OR_RETURN_C_STATUS(
            auto local_output,
            CreateTensorWithLayout(std::move((*result)[i]),
                                   parallel_device_mesh->mesh_config(),
                                   function.output_layouts[i]),
            status);
        output_with_layout.push_back(std::move(local_output));
      }
    }

    for (int i = 0; i < function.output_index_map.size(); ++i) {
      // TODO(b/162744844): Generalize this pattern so that the extraction is
      // not special cased.
      if (function.shape_output_metadata.find(i) !=
              function.shape_output_metadata.end() &&
          output_with_layout[i]->const_value_node() != nullptr) {
        output_with_layout[i]
            ->const_value_node()
            ->set_input_layout_for_shape_op_result(
                function.shape_output_metadata.at(i));
      }

      RecordInShapeLayoutCache(*output_with_layout[i]);
      typed_outputs[function.output_index_map[i]] =
          std::move(output_with_layout[i]);
    }
  }
  if (TF_GetCode(join_status.get()) != TF_OK) {
    std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> async_wait_status(
        TF_NewStatus(), TF_DeleteStatus);
    AsyncWait(context, async_wait_status.get());
    TF_Code error_code = TF_GetCode(async_wait_status.get());
    if (error_code != TF_OK && error_code != TF_CANCELLED) {
      // Ignore the AsyncWait() status return since we already have a bad status
      // to propagate. We've just canceled a bunch of operations, so we expect
      // cancellation status returns. We'll log anything else just to be safe.
      LOG(ERROR) << "Error executing " << doperation.name << " "
                 << TF_Message(async_wait_status.get());
    }

    TF_SetStatus(status, TF_GetCode(join_status.get()),
                 TF_Message(join_status.get()));
    return;
  }
  if (VLOG_IS_ON(2)) {
    LOG(INFO) << "Executed " << doperation.name << ", got "
              << typed_outputs.size() << " outputs:";
    for (const std::unique_ptr<TensorWithLayout>& output : typed_outputs) {
      LOG(INFO) << "  " << output->DebugString();
    }
  }
  if (doperation.name == std::string("VarHandleOp")) {
    // For new variables, set the dereferenced shape/dtype so we can pass it in
    // as _handle_dtype and _handle_shape in the future.
    //
    // Note that VarHandleOps generated by `tf.Variable` objects are always run
    // eagerly, which is almost all of the op's usage in TF2. Theoretically a
    // user could run it in a tf.function via tf.raw_ops.VarHandleOp, return it
    // from that function, and add it as an input to another, and it would
    // currently be missing handle information.
    if (typed_outputs.size() != 1) {
      RETURN_STATUS(status, TF_INTERNAL,
                    "Expected one output from VarHandleOp");
    }
    NameAttrList name_and_attrs;
    ASSIGN_OR_RETURN_C_STATUS(name_and_attrs, FetchAttributes(attributes),
                              status);

    RETURN_C_STATUS_IF_NOT_OK(
        llvm::cast<ResourceHandleWithLayout>(typed_outputs[0].get())
            ->UpdateShapeAndDType(name_and_attrs.attr().at("shape").shape(),
                                  name_and_attrs.attr().at("dtype").type()),
        status);
  }

  for (int i = 0; i < *num_outputs; ++i) {
    outputs[i] =
        MakeLayoutTensorHandle(context, std::move(typed_outputs[i]), status);
    if (TF_GetCode(status) != TF_OK) return;
  }
}

void DTensorDevice::Execute(const TFE_Op* original_op, int* num_outputs,
                            TFE_TensorHandle** outputs, TF_Status* status) {
  TFE_Context* context = TFE_OpGetContext(original_op, status);
  if (TF_GetCode(status) != TF_OK) return;
  const char* operation_name = TFE_OpGetName(original_op, status);
  if (TF_GetCode(status) != TF_OK) return;
  const TFE_OpAttrs* attributes = TFE_OpGetAttrs(original_op);
  int num_inputs = TFE_OpGetFlatInputCount(original_op, status);
  if (TF_GetCode(status) != TF_OK) return;
  std::vector<TFE_TensorHandle*> inputs_vector;
  inputs_vector.reserve(num_inputs);

  std::vector<TF_DataType> dtypes;
  dtypes.reserve(num_inputs);

  for (int input_index = 0; input_index < num_inputs; ++input_index) {
    TFE_TensorHandle* input =
        TFE_OpGetFlatInput(original_op, input_index, status);
    if (TF_GetCode(status) != TF_OK) return;
    inputs_vector.push_back(input);
    dtypes.push_back(TFE_TensorHandleDataType(input));
  }
  TFE_TensorHandle** inputs = inputs_vector.data();
  if (!default_mesh_) {
    RETURN_STATUS(
        status, TF_INVALID_ARGUMENT,
        "No default mesh has been registered to DTensor. Use dtensor.run_on to "
        "explicit specify a mesh.");
  }
  DTensorOperation dtensor_operation{
      /*name*/ operation_name,
      /*function_def*/
      tensorflow::unwrap(context)->FindFunctionDef(operation_name),
      /*default_mesh*/ default_mesh_->mesh_config(),
  };

  // First handle DTensor-specific virtual operations.
  bool is_op_handled = false;
  MaybeHandleDTensorCustomOps(operation_name, num_inputs, attributes, context,
                              inputs, num_outputs, outputs, &is_op_handled,
                              status);
  if (is_op_handled) return;

  // This isn't a special op, so we'll defer to TFE_Execute to actually execute
  // it, but we'll also run DTensor MLIR passes and propagate the layout.
  std::vector<TensorWithLayout*> typed_inputs;
  std::vector<std::unique_ptr<TensorWithLayout>> inputs_with_no_layout;

  // Record a unique mesh identified through all inputs that's already on
  // DTensor device. If we can identify a single mesh, the same mesh is used as
  // the mesh to broadcast non-dtensor inputs.
  absl::flat_hash_set<Mesh> input_meshes;
  std::vector<int> not_on_device_input_indices;

  typed_inputs.resize(num_inputs);
  for (int j = 0; j < num_inputs; ++j) {
    TFE_TensorHandle* input = inputs[j];
    const char* input_device = TFE_TensorHandleDeviceName(input, status);
    if (TF_GetCode(status) != TF_OK) return;
    if (name_ != input_device) {
      not_on_device_input_indices.push_back(j);
      continue;
    }
    // Handle input which is on DTensor device already.
    TensorWithLayout* t = reinterpret_cast<TensorWithLayout*>(
        TFE_TensorHandleDevicePointer(input, status));
    if (TF_GetCode(status) != TF_OK) return;

    // VarHandleOp runs on empty mesh, and that isn't registered with device.
    if (!t->layout().mesh().IsEmpty()) {
      input_meshes.insert(t->layout().mesh());
    }
    // Remote mesh inputs are not able to be read and evaluated.
    if (!is_remote_mesh(t->layout().mesh()) &&
        t->const_value_node() != nullptr &&
        !t->const_value_node()->const_value().has_value()) {
      std::optional<NodeDef> const_value =
          ExtractSmallTensorValue(context, input, t->layout(), status);
      if (TF_GetCode(status) != TF_OK) return;
      if (const_value.has_value()) {
        t->const_value_node()->set_const_value(const_value.value());
      }
    }
    typed_inputs[j] = t;
  }

  const MeshWithParallelDevice* broadcast_mesh =
      ChooseBroadcastingMesh(input_meshes, dtypes);

  if (!broadcast_mesh) {
    RETURN_STATUS(status, TF_INVALID_ARGUMENT,
                  "No mesh has been registered to DTensor. Use copy_to_mesh to "
                  "explicit specify a mesh instead.");
  }
  const Mesh& mesh = broadcast_mesh->mesh_config();
  for (int not_on_device_input_index : not_on_device_input_indices) {
    TFE_TensorHandle* input = inputs[not_on_device_input_index];
    // DTensor creation should be explicit, with some exceptions for usability
    // (scalars/shapes/slice specs/etc.) Here we do some trivial validation to
    // enforce this rule.
    int num_dims = TFE_TensorHandleNumDims(input, status);
    if (TF_GetCode(status) != TF_OK) return;
    int64_t num_elements = TFE_TensorHandleNumElements(input, status);
    if (TF_GetCode(status) != TF_OK) return;
    TF_DataType dtype = TFE_TensorHandleDataType(input);
    const bool small_int_tensor = num_elements < kSmallTensorThreshold &&
                                  (dtype == TF_INT32 || dtype == TF_INT64);
    // Only allow large constant autobroadcast for CopyToMesh and Relayout ops.
    if ((operation_name != std::string("CopyToMesh") &&
         operation_name != std::string("Relayout")) &&
        !(num_dims == 0 || dtype == TF_STRING || small_int_tensor)) {
      std::vector<int64_t> tensor_shape(TensorShapeAsVector(input, status));
      if (TF_GetCode(status) != TF_OK) return;
      RETURN_STATUS(
          status, TF_UNIMPLEMENTED,
          absl::StrCat(
              "The op/function ", operation_name,
              " got a regular tensor for input ", not_on_device_input_index,
              " (shape ", ShapeToDebugString(tensor_shape),
              ") but was expecting a DTensor. Currently only scalars and "
              "small integer/string tensors are auto-broadcast to "
              "DTensors. For other tensors, please use copy_to_mesh to "
              "make a DTensor explicitly; note that this may be slow if it "
              "happens frequently.")
              .c_str());
    }
    // Construct temporary TensorWithLayout objects for inputs that didn't
    // have any to start. These are owned by the `inputs_with_no_layout`
    // vector, whereas the input `TFE_TensorHandle`s maintain ownership for
    // inputs that already had layouts (and therefor had TensorWithLayout
    // objects).
    std::unique_ptr<TensorWithLayout> wrapper;
    if (parallel_executor_) {
      std::unique_ptr<TensorWithLayout> tensor_with_layout =
          Broadcast(context, input, mesh, status);
      if (TF_GetCode(status) != TF_OK) {
        return;
      }
      wrapper = std::move(tensor_with_layout);
    } else {
      wrapper = TensorWithLayoutTf::Broadcast(context, input, *broadcast_mesh,
                                              name_, status);
    }
    if (TF_GetCode(status) != TF_OK) return;
    if (!ShouldFoldInputArgument(dtensor_operation.name,
                                 /*input_index=*/not_on_device_input_index) &&
        wrapper->const_value_node() != nullptr) {
      wrapper->const_value_node()->reset_const_value();
    }
    typed_inputs[not_on_device_input_index] = wrapper.get();
    inputs_with_no_layout.emplace_back(wrapper.release());
  }

  ExecuteRegularOperation(context, typed_inputs, dtensor_operation, attributes,
                          num_outputs, outputs, status);
}

void ExecuteOnDTensorDevice(const TFE_Op* original_op, int* num_outputs,
                            TFE_TensorHandle** outputs, TF_Status* status,
                            void* device_info) {
  DTensorDevice* dev = reinterpret_cast<DTensorDevice*>(device_info);
  dev->Execute(original_op, num_outputs, outputs, status);
}

void DeleteDTensorDevice(void* device_info) {
  delete static_cast<DTensorDevice*>(device_info);
}

TFE_TensorHandle* CopyToDTensorDevice(TFE_Context* context,
                                      TFE_TensorHandle* tensor,
                                      TF_Status* status, void* device_info) {
  TF_SetStatus(status, TF_UNIMPLEMENTED,
               "Trying to copy a tensor on to a DTensor mesh without a layout "
               "(use the CopyToMesh op for now).");
  return nullptr;
}

TFE_TensorHandle* CopyFromDTensorDevice(TFE_Context* context,
                                        TFE_TensorHandle* tensor,
                                        const char* target_device_name,
                                        TF_Status* status, void* device_info) {
  if (!absl::StrContains(std::string(target_device_name), "CPU")) {
    TF_SetStatus(
        status, TF_UNIMPLEMENTED,
        "Trying to copy a tensor to a non-CPU device is not supported.");
    return nullptr;
  }
  TensorWithLayout* typed_input = reinterpret_cast<TensorWithLayout*>(
      TFE_TensorHandleDevicePointer(tensor, status));
  if (!tensorflow::dtensor::Layout(typed_input->layout()).IsFullyReplicated()) {
    TF_SetStatus(status, TF_UNIMPLEMENTED,
                 absl::StrCat("Trying to copy a non-replicated DTensor is not "
                              "supported. Input tensor is: ",
                              typed_input->DebugString())
                     .c_str());

    return nullptr;
  }
  if (typed_input->dtype() == TF_RESOURCE) {
    TF_SetStatus(status, TF_UNIMPLEMENTED,
                 "Trying to copy a DTensor resource handle is not supported.");
    return nullptr;
  }
  DTensorDevice* dev = reinterpret_cast<DTensorDevice*>(device_info);
  // Since operations are executed asynchronously, the operation which should
  // produce the tensor we're trying to copy off the DTensor device may be
  // canceled due to a failure on another device. If so, we want to report the
  // failure that caused the cancellation, not the cancellation itself. This
  // requires blocking waiting for other devices to flush their execution
  // queues.
  // Note that we also only need to sync the threads on the parallel_device()
  // directly, or a context level sync might cause unintentional deadlocks when
  // grabbing locks on other threads.
  dev->AsyncWait(context, status);
  if (TF_GetCode(status) != TF_OK) return nullptr;
  return TFE_TensorHandleCopySharingTensor(typed_input->get_tensor(0), status);
}

bool PinToDTensorDevice(const TFE_Op* op, TF_Status* s) {
  // Always pin to the dtensor device if any of its input is a dtensor.
  // Note that if this function is called, the caller guarantees
  // that all inputs that are on a custom device is a single dtensor device.

  // Exception 1:
  // If there is a non-dtensor resource tensor and other dtensor inputs
  // are not on a CPU mesh, then pin to the physical device.
  //
  // This is because our resource upcast to a dtensor only supports broadcasting
  // to a CPU mesh. If any other dtensor inputs are on a TPU mesh,
  // then the mesh that is broadcasted will be the TPU mesh.
  int num_inputs = TFE_OpGetFlatInputCount(op, s);
  std::vector<TFE_TensorHandle*> inputs_vector;
  inputs_vector.reserve(num_inputs);

  absl::flat_hash_set<Mesh> input_meshes;

  bool has_non_dtensor_resource = false;

  for (int input_index = 0; input_index < num_inputs; ++input_index) {
    TFE_TensorHandle* input = TFE_OpGetFlatInput(op, input_index, s);

    std::string input_device_name =
        std::string(TFE_TensorHandleDeviceName(input, s));
    if (!absl::StrContains(absl::AsciiStrToLower(input_device_name),
                           "custom")) {
      TF_DataType dtype = TFE_TensorHandleDataType(input);
      if (dtype == TF_RESOURCE) {
        has_non_dtensor_resource = true;
      }
      continue;
    }

    // Handle input which is on DTensor device already.
    TensorWithLayout* t = reinterpret_cast<TensorWithLayout*>(
        TFE_TensorHandleDevicePointer(input, s));

    if (!t->layout().mesh().IsEmpty()) {
      input_meshes.insert(t->layout().mesh());
    }
  }

  const Mesh* broadcast_mesh =
      input_meshes.size() == 1 ? &(*input_meshes.begin()) : nullptr;

  // Place on physical device as dtensor does not support upcasting resource
  // tensor to a non-cpu mesh.
  if (has_non_dtensor_resource && broadcast_mesh &&
      !broadcast_mesh->is_cpu_mesh()) {
    LOG(WARNING)
        << "DTensor Function has been pinned back to a physical device because"
        << "a regular TF Variable is an input along with dtensor inputs and "
        << "was unable to be upcasted to a DVariable. This "
        << "may be unintended and signify an error in the way the user is "
        << "mixing dtensors with regular variables as inputs to a function.";
    return false;
  }

  return true;
}

void AllocateDTensorDevice(absl::string_view device_name,
                           TFE_CustomDevice* device, void** device_info,
                           TF_Status* status) {
  DTensorDevice* dtensor_device = nullptr;
  if (status) {
    ASSIGN_OR_RETURN_C_STATUS(dtensor_device,
                              DTensorDevice::Create(device_name), status);
  } else {
    // TODO(b/268241383): Remove this branch.
    auto device_status = DTensorDevice::Create(device_name);
    TF_CHECK_OK(device_status.status());
    dtensor_device = device_status.value();
  }

  device->copy_tensor_to_device = &CopyToDTensorDevice;
  device->copy_tensor_from_device = &CopyFromDTensorDevice;
  device->delete_device = &DeleteDTensorDevice;
  device->execute = &ExecuteOnDTensorDevice;
  device->shall_pin_to_this_device = &PinToDTensorDevice;
  *device_info = dtensor_device;
}

void AddMesh(const std::string& serialized_mesh, void* device_info,
             bool is_async, bool is_host_mesh, int in_flight_nodes_limit,
             TF_Status* status) {
  auto mesh_config_or_status = Mesh::FromString(serialized_mesh);
  if (!mesh_config_or_status.ok()) {
    TF_SetStatus(status, TF_INTERNAL,
                 absl::StrCat("Failed to parse mesh config. ",
                              mesh_config_or_status.status().error_message())
                     .c_str());
    return;
  }
  auto mesh_config = mesh_config_or_status.value();
  std::vector<std::string> underlying_devices;
  underlying_devices.insert(underlying_devices.end(),
                            mesh_config.local_devices().begin(),
                            mesh_config.local_devices().end());
  // DTensor uses multi-client setup which doesn't use remote eager, so we can
  // enable eager async execution in ParallelDevice.
  std::unique_ptr<tensorflow::parallel_device::ParallelDevice> parallel(
      new tensorflow::parallel_device::ParallelDevice(
          underlying_devices, is_async, in_flight_nodes_limit));

  auto mesh = std::make_unique<MeshWithParallelDevice>(std::move(mesh_config),
                                                       std::move(parallel));
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  device->AddMesh(std::move(mesh), is_host_mesh);
}

void ExperimentalSetDefaultLayout(const std::string& serialized_layout,
                                  void* device_info, TF_Status* status) {
  StatusOr<Layout> layout = Layout::FromString(serialized_layout);
  if (!layout.ok()) {
    RETURN_STATUS(status, TF_INTERNAL, layout.status().error_message().c_str());
  }
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  device->SetDefaultLayout(layout.value());
}

void ExperimentalClearDefaultLayout(void* device_info, TF_Status* status) {
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  device->ClearDefaultLayout();
}

void ExperimentalSetDefaultMesh(const std::string& serialized_mesh,
                                void* device_info, TF_Status* status) {
  StatusOr<Mesh> mesh = Mesh::FromString(serialized_mesh);
  if (!mesh.ok()) {
    RETURN_STATUS(status, TF_INTERNAL, mesh.status().error_message().c_str());
  }
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  device->SetDefaultMesh(mesh.value());
}

void ExperimentalClearDefaultMesh(void* device_info, TF_Status* status) {
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  device->ClearDefaultMesh();
}

void SetSameShapePolicy(void* device_info, bool enabled) {
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  device->SetSameShapePolicy(enabled);
}

void SetTPUCoreIDs(const std::string& mesh_name,
                   const std::vector<int>& tpu_core_ids, void* device_info,
                   TF_Status* status) {
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  RETURN_C_STATUS_IF_NOT_OK(device->SetTPUCoreIDs(mesh_name, tpu_core_ids),
                            status);
}

void ClearTPUCoreIDs(void* device_info) {
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  device->ClearTPUCoreIDs();
}

std::vector<std::vector<int>> TPUCoreIDsToLocations(
    TFE_Context* context, const std::vector<int>& tpu_core_ids,
    void* device_info) {
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  return device->TPUCoreIDsToLocations(context, tpu_core_ids);
}

std::vector<int> TPUCoreLocationsToIDs(
    TFE_Context* context,
    const std::vector<std::vector<int>>& tpu_core_locations,
    void* device_info) {
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  return device->TPUCoreLocationsToIDs(context, tpu_core_locations);
}

TFE_TensorHandle* Pack(TFE_Context* context, int num_inputs,
                       TFE_TensorHandle** inputs,
                       const std::string& string_layout, void* device_info,
                       TF_Status* status) {
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  return device->Pack(context, num_inputs, inputs, string_layout, status);
}

std::vector<TFE_TensorHandle*> Unpack(TFE_Context* context,
                                      TFE_TensorHandle* input,
                                      void* device_info, TF_Status* status) {
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  return device->Unpack(context, input, status);
}

std::string FetchLayout(TFE_Context* context, TFE_TensorHandle* input,
                        void* device_info, TF_Status* status) {
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  return device->FetchLayout(context, input, status);
}

bool IsDTensor(TFE_Context* context, TFE_TensorHandle* input, void* device_info,
               TF_Status* status) {
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  return device->IsDTensor(context, input, status);
}

TFE_TensorHandle* SparsePack(TFE_Context* context, int num_inputs,
                             TFE_TensorHandle** indices,
                             TFE_TensorHandle** values,
                             TFE_TensorHandle** shapes,
                             const std::string& string_layout,
                             void* device_info, TF_Status* status) {
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  return device->SparsePack(context, num_inputs, indices, values, shapes,
                            string_layout, status);
}

bool IsSparseDTensor(TFE_Context* context, TFE_TensorHandle* input,
                     void* device_info, TF_Status* status) {
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  return device->IsSparseDTensor(context, input, status);
}

std::unordered_map<std::string, int> GetFunctionCacheStats(TFE_Context* context,
                                                           void* device_info,
                                                           TF_Status* status) {
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  return device->GetFunctionCacheStats(context, status);
}

void SetIteratorElementLayouts(TFE_Context* context, TFE_TensorHandle* input,
                               const std::vector<std::string>& string_layouts,
                               void* device_info, TF_Status* status) {
  DTensorDevice* device = reinterpret_cast<DTensorDevice*>(device_info);
  device->SetIteratorElementLayouts(context, input, string_layouts, status);
}

}  // namespace dtensor
}  // namespace tensorflow
