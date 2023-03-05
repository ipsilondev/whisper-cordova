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

#include "tensorflow/compiler/xla/pjrt/distributed/service.h"

#include <memory>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "grpcpp/server_builder.h"
#include "tensorflow/compiler/xla/pjrt/distributed/protocol.h"
#include "tensorflow/compiler/xla/pjrt/distributed/util.h"
#include "tensorflow/compiler/xla/status.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/tsl/distributed_runtime/coordination/coordination_service.h"
#include "tensorflow/tsl/distributed_runtime/rpc/async_service_interface.h"
#include "tensorflow/tsl/distributed_runtime/rpc/coordination/grpc_coordination_service_impl.h"
#include "tensorflow/tsl/platform/env.h"
#include "tensorflow/tsl/platform/errors.h"
#include "tensorflow/tsl/platform/random.h"
#include "tensorflow/tsl/platform/threadpool.h"
#include "tensorflow/tsl/protobuf/coordination_config.pb.h"

namespace {
constexpr int kBarrierTimedOut = -1000;

std::unique_ptr<tsl::CoordinationServiceInterface> EnableCoordinationService(
    const xla::DistributedRuntimeServiceImpl::Options& options) {
  const std::string job_name = "jax_worker";
  tensorflow::CoordinationServiceConfig config;
  config.set_service_type("standalone");
  config.set_service_leader(absl::StrCat("/job:", job_name, "/task:0"));
  config.set_cluster_register_timeout_in_ms(
      absl::ToInt64Milliseconds(options.enumerate_devices_timeout));
  config.set_heartbeat_timeout_in_ms(absl::ToInt64Milliseconds(
      options.heartbeat_interval * options.max_missing_heartbeats));
  config.set_shutdown_barrier_timeout_in_ms(
      absl::ToInt64Milliseconds(options.shutdown_timeout));
  tensorflow::CoordinatedJob* job =
      config.mutable_coordinated_job_list()->Add();
  job->set_name(job_name);
  job->set_num_tasks(options.num_nodes);
  auto service = tsl::CoordinationServiceInterface::EnableCoordinationService(
      options.env, config, /*cache=*/nullptr);
  // Convert list of local devices to global device message as EnumerateDevies()
  // response.
  service->SetDeviceAggregationFunction(
      [](const tensorflow::DeviceInfo& raw_global_devices) {
        xla::GlobalTopologyProto global_topology;
        int global_device_id = 0;
        // Assign local devices of the same host to the same slice_index.
        int next_slice_index = 0;
        absl::flat_hash_map<std::string, int> boot_id_to_slice_index;
        // Unwrap result to local device proto.
        for (const auto& device : raw_global_devices.device()) {
          xla::LocalTopologyProto local_topology;
          // Note that tensorflow::DeviceInfo.device is xla.LocalTopologyProto!
          device.UnpackTo(&local_topology);
          // Every new boot_id seen is treated as a new host/slice.
          absl::string_view boot_id = local_topology.boot_id();
          auto [it, inserted] =
              boot_id_to_slice_index.try_emplace(boot_id, next_slice_index);
          if (inserted) {
            ++next_slice_index;
          }
          // Set deterministic global ids.
          for (xla::DeviceProto& device : *local_topology.mutable_devices()) {
            device.set_global_device_id(global_device_id++);
            device.set_slice_index(it->second);
          }
          *global_topology.mutable_nodes()->Add() = local_topology;
        }
        if (VLOG_IS_ON(10)) {
          for (auto it = boot_id_to_slice_index.begin();
               it != boot_id_to_slice_index.end(); ++it) {
            LOG(INFO) << "BuildGlobalTopology boot_id_to_slice_index "
                      << it->first << "->" << it->second;
          }
        }
        // Wrap result back in DeviceInfo proto.
        tensorflow::DeviceInfo global_devices;
        global_devices.mutable_device()->Add()->PackFrom(global_topology);
        return global_devices;
      });
  return service;
}
}  // namespace

namespace xla {

DistributedRuntimeServiceImpl::DistributedRuntimeServiceImpl(
    const Options& options)
    : options_(options), session_id_(tsl::random::New64()) {
  nodes_.resize(options.num_nodes);
  local_topologies_.resize(options.num_nodes);
}

DistributedRuntimeServiceImpl::~DistributedRuntimeServiceImpl() {
  {
    absl::MutexLock lock(&mu_);
    state_ = State::kClosed;
    service_status_ = tsl::errors::FailedPrecondition("Service shutting down.");
    if (!stop_heartbeat_thread_.HasBeenNotified()) {
      stop_heartbeat_thread_.Notify();
    }
  }
}

// Steals the contents of `local_topologies`.
void BuildGlobalTopology(absl::Span<LocalTopologyProto> local_topologies,
                         GlobalTopologyProto* global_topology) {
  int next_global_device_id = 0;
  // Assign local devices of the same host to the same slice_index.
  int next_slice_index = 0;
  absl::flat_hash_map<std::string, int> boot_id_to_slice_index;
  for (LocalTopologyProto& local : local_topologies) {
    // Every new boot_id seen is treated as a new host/slice.
    absl::string_view boot_id = local.boot_id();
    auto [it, inserted] =
        boot_id_to_slice_index.try_emplace(boot_id, next_slice_index);
    if (inserted) {
      ++next_slice_index;
    }
    for (DeviceProto& device : *local.mutable_devices()) {
      device.set_global_device_id(next_global_device_id++);
      device.set_slice_index(it->second);
    }
    global_topology->add_nodes()->Swap(&local);
  }
  if (VLOG_IS_ON(10)) {
    for (auto it = boot_id_to_slice_index.begin();
         it != boot_id_to_slice_index.end(); ++it) {
      LOG(INFO) << "BuildGlobalTopology boot_id_to_slice_index " << it->first
                << "->" << it->second;
    }
  }
}

xla::Status DistributedRuntimeServiceImpl::ValidateNodeId(int node_id) {
  if (node_id < 0) {
    return xla::InvalidArgument("Invalid node ID %d, must be non-negative",
                                node_id);
  }
  if (node_id >= options_.num_nodes) {
    return xla::FailedPrecondition(
        "Invalid node ID %d, must be in the range [0, %d)", node_id,
        options_.num_nodes);
  }
  return xla::OkStatus();
}

xla::Status DistributedRuntimeServiceImpl::ValidateSessionId(
    uint64_t session_id) {
  if (session_id != session_id_) {
    return xla::FailedPrecondition(
        "Session ID of request %llu does not match active session ID %llu",
        session_id, session_id_);
  }
  return xla::OkStatus();
}

::grpc::Status DistributedRuntimeServiceImpl::Connect(
    ::grpc::ServerContext* context, const ConnectRequest* request,
    ConnectResponse* response) {
  VLOG(10) << "Connect " << request->DebugString();
  if (request->protocol_version() != DistributedRuntimeProtocolVersion()) {
    return xla::ToGrpcStatus(xla::InvalidArgument("Invalid protocol version %d",
                                                  request->protocol_version()));
  }
  absl::MutexLock lock(&mu_);
  if (state_ != State::kInitializing) {
    // This most likely indicates that a client task was restarted but the
    // old master is still up. Clients should retry on failure.
    return xla::ToGrpcStatus(tsl::errors::Aborted(
        "Connect() called when system is not initializing."));
  }
  int node_id = request->node_id();
  xla::Status status = ValidateNodeId(node_id);
  if (!status.ok()) {
    return xla::ToGrpcStatus(status);
  }
  if (!nodes_[node_id].present) {
    nodes_[node_id].present = true;
    ++num_nodes_present_;
  }
  nodes_[node_id].client_id = request->client_id();

  auto all_nodes_present_or_duplicate_request = [&]() {
    mu_.AssertHeld();
    return num_nodes_present_ == nodes_.size() ||
           nodes_[node_id].client_id != request->client_id();
  };
  auto connect_timeout = absl::Milliseconds(request->timeout_milliseconds());
  if (!mu_.AwaitWithTimeout(
          absl::Condition(&all_nodes_present_or_duplicate_request),
          connect_timeout)) {
    nodes_[node_id].present = false;
    --num_nodes_present_;
    return xla::ToGrpcStatus(tsl::errors::DeadlineExceeded(
        "Timed out after ", absl::FormatDuration(connect_timeout),
        " waiting for all nodes to call Connect()"));
  }

  if (nodes_[node_id].client_id != request->client_id()) {
    // This might happen either if two nodes are erroneously configured with the
    // same ID number, or it might happen if a task fails and is restarted
    // while we are waiting for nodes to connect. To elaborate on the second
    // scenario, it would look like this:
    // * a task calls Connect() with a particular node_id and client_id.
    // * the task is killed and restarted, or alternatively the client's RPC
    //   times out and it decides to retry.
    // * the task calls Connect() again with the same node_id and a different
    //   client_id.
    // In this scenario we take whichever client showed up most recently and
    // evict the client with an out-of-date client ID.
    return xla::ToGrpcStatus(
        tsl::errors::Aborted("Duplicate node ID ", node_id));
  }

  if (node_id == 0) {
    state_ = State::kRunning;
    heartbeat_thread_.reset(options_.env->StartThread(
        tsl::ThreadOptions(), "pjrt_service_heartbeat",
        [this]() { HeartbeatLoop(); }));
  } else {
    auto running = [&]() {
      mu_.AssertHeld();
      return state_ == State::kRunning;
    };
    mu_.Await(absl::Condition(&running));
  }
  nodes_[node_id].last_heartbeat = absl::Now();
  response->set_session_id(session_id_);
  return ::grpc::Status::OK;
}

::grpc::Status DistributedRuntimeServiceImpl::Shutdown(
    ::grpc::ServerContext* context, const ShutdownRequest* request,
    ShutdownResponse* response) {
  VLOG(10) << "Shutdown " << request->DebugString();
  xla::Status status = ValidateSessionId(request->session_id());
  if (!status.ok()) {
    return xla::ToGrpcStatus(status);
  }
  absl::MutexLock lock(&mu_);
  if (state_ != State::kRunning) {
    if (!service_status_.ok()) {
      return xla::ToGrpcStatus(service_status_);
    }
    return xla::ToGrpcStatus(xla::FailedPrecondition(
        "Shutdown() called when system is not running."));
  }
  int node_id = request->node_id();
  status = ValidateNodeId(node_id);
  if (!status.ok()) {
    return xla::ToGrpcStatus(status);
  }
  ++num_nodes_shutting_down_;

  auto all_nodes_shutting_down = [&]() {
    mu_.AssertHeld();
    return num_nodes_shutting_down_ == nodes_.size() || !service_status_.ok();
  };
  if (!mu_.AwaitWithTimeout(absl::Condition(&all_nodes_shutting_down),
                            options_.shutdown_timeout)) {
    state_ = State::kClosed;
    return xla::ToGrpcStatus(tsl::errors::DeadlineExceeded(
        "Timed out after ", absl::FormatDuration(options_.shutdown_timeout),
        " waiting for all nodes to call Shutdown()"));
  }
  state_ = State::kClosed;
  if (!stop_heartbeat_thread_.HasBeenNotified()) {
    stop_heartbeat_thread_.Notify();
  }
  if (!service_status_.ok()) {
    return xla::ToGrpcStatus(service_status_);
  }
  return ::grpc::Status::OK;
}

::grpc::Status DistributedRuntimeServiceImpl::EnumerateDevices(
    ::grpc::ServerContext* context, const EnumerateDevicesRequest* request,
    EnumerateDevicesResponse* response) {
  VLOG(10) << "EnumerateDevices " << request->DebugString();
  xla::Status status = ValidateSessionId(request->session_id());
  if (!status.ok()) {
    return xla::ToGrpcStatus(status);
  }
  absl::MutexLock lock(&mu_);
  if (state_ != State::kRunning) {
    if (!service_status_.ok()) {
      return xla::ToGrpcStatus(service_status_);
    }
    return xla::ToGrpcStatus(xla::FailedPrecondition(
        "EnumerateDevices() called when system is not running."));
  }
  int node_id = request->local_topology().node_id();
  status = ValidateNodeId(node_id);
  if (!status.ok()) {
    return xla::ToGrpcStatus(status);
  }
  local_topologies_[node_id] = request->local_topology();
  ++num_topologies_present_;

  auto all_topologies_present = [&]() {
    mu_.AssertHeld();
    return num_topologies_present_ == nodes_.size() || !service_status_.ok();
  };
  if (!mu_.AwaitWithTimeout(absl::Condition(&all_topologies_present),
                            options_.enumerate_devices_timeout)) {
    return xla::ToGrpcStatus(tsl::errors::DeadlineExceeded(
        "Timed out after ",
        absl::FormatDuration(options_.enumerate_devices_timeout),
        " waiting for all nodes to call EnumerateDevices()"));
  }
  if (!service_status_.ok()) {
    return xla::ToGrpcStatus(service_status_);
  }

  if (node_id == 0) {
    topology_.emplace();
    BuildGlobalTopology(absl::Span<LocalTopologyProto>(local_topologies_),
                        &*topology_);
    local_topologies_.clear();
  } else {
    auto topology_ready = [&]() -> bool {
      mu_.AssertHeld();
      return topology_.has_value();
    };
    mu_.Await(absl::Condition(&topology_ready));
  }
  *response->mutable_global_topology() = *topology_;
  return ::grpc::Status::OK;
}

::grpc::Status DistributedRuntimeServiceImpl::Heartbeat(
    ::grpc::ServerContext* context, const HeartbeatRequest* request,
    HeartbeatResponse* response) {
  VLOG(10) << "Heartbeat " << request->DebugString();
  xla::Status status = ValidateSessionId(request->session_id());
  if (!status.ok()) {
    return xla::ToGrpcStatus(status);
  }
  absl::MutexLock lock(&mu_);
  if (state_ != State::kRunning) {
    if (!service_status_.ok()) {
      return xla::ToGrpcStatus(service_status_);
    }
    return xla::ToGrpcStatus(xla::FailedPrecondition(
        "Heartbeat() called when system is not running."));
  }
  int node_id = request->node_id();
  status = ValidateNodeId(node_id);
  if (!status.ok()) {
    return xla::ToGrpcStatus(status);
  }
  nodes_[node_id].last_heartbeat = absl::Now();
  return ::grpc::Status::OK;
}

void DistributedRuntimeServiceImpl::HeartbeatLoop() {
  while (true) {
    stop_heartbeat_thread_.WaitForNotificationWithTimeout(
        options_.heartbeat_interval);
    VLOG(10) << "Checking heartbeats";
    if (stop_heartbeat_thread_.HasBeenNotified()) {
      VLOG(10) << "Heartbeat checking stopped.";
      return;
    }
    absl::Time now = absl::Now();
    absl::MutexLock lock(&mu_);
    for (size_t i = 0; i < nodes_.size(); ++i) {
      // If we haven't heard from the node for a number of heartbeat intervals,
      // declare that we are unhealthy.
      VLOG(10) << "Node " << i
               << " last heartbeat: " << nodes_[i].last_heartbeat;
      if (nodes_[i].last_heartbeat +
              options_.max_missing_heartbeats * options_.heartbeat_interval <
          now) {
        LOG(INFO) << "Missed heartbeats from node " << i << ". Shutting down.";
        state_ = State::kClosed;
        service_status_ = tsl::errors::Aborted(
            "Shutting down due to missed heartbeat from task ", i);
        return;
      }
    }
  }
}

::grpc::Status DistributedRuntimeServiceImpl::KeyValueGet(
    ::grpc::ServerContext* context, const KeyValueGetRequest* request,
    KeyValueGetResponse* response) {
  VLOG(10) << "KeyValueGet " << request->DebugString();
  xla::Status status = ValidateSessionId(request->session_id());
  if (!status.ok()) {
    return xla::ToGrpcStatus(status);
  }
  {
    absl::MutexLock lock(&mu_);
    if (state_ != State::kRunning) {
      if (!service_status_.ok()) {
        return xla::ToGrpcStatus(service_status_);
      }
      return xla::ToGrpcStatus(xla::FailedPrecondition(
          "KeyValueGet() called when system is not running."));
    }
  }
  return key_value_store_.Get(
      request->key(), absl::Milliseconds(request->timeout_milliseconds()),
      response->mutable_value());
}

::grpc::Status DistributedRuntimeServiceImpl::KeyValueSet(
    ::grpc::ServerContext* context, const KeyValueSetRequest* request,
    KeyValueSetResponse* response) {
  VLOG(10) << "KeyValueSet " << request->DebugString();
  xla::Status status = ValidateSessionId(request->session_id());
  if (!status.ok()) {
    return xla::ToGrpcStatus(status);
  }
  {
    absl::MutexLock lock(&mu_);
    if (state_ != State::kRunning) {
      if (!service_status_.ok()) {
        return xla::ToGrpcStatus(service_status_);
      }
      return xla::ToGrpcStatus(xla::FailedPrecondition(
          "KeyValueSet() called when system is not running; clients must call "
          "Connect() first"));
    }
  }
  return key_value_store_.Set(request->key(), request->value());
}

::grpc::Status DistributedRuntimeServiceImpl::WaitAtBarrier(
    ::grpc::ServerContext* context, const WaitAtBarrierRequest* request,
    WaitAtBarrierResponse* response) {
  VLOG(10) << "WaitAtBarrier " << request->DebugString();
  xla::Status status = ValidateSessionId(request->session_id());
  if (!status.ok()) {
    return xla::ToGrpcStatus(status);
  }
  absl::MutexLock lock(&mu_);
  if (state_ != State::kRunning) {
    if (!service_status_.ok()) {
      return xla::ToGrpcStatus(service_status_);
    }
    return xla::ToGrpcStatus(xla::FailedPrecondition(
        "WaitAtBarrier() called when system is not running."));
  }
  int node_id = request->node_id();
  status = ValidateNodeId(node_id);
  if (!status.ok()) {
    return xla::ToGrpcStatus(status);
  }

  std::string barrier_id = request->barrier_id();

  if (barrier_id_to_num_nodes_[barrier_id] == nodes_.size()) {
    return xla::ToGrpcStatus(
        xla::FailedPrecondition("Calling WaitAtBarrier with the same id "
                                "across barriers is not allowed. Please use "
                                "unique barrier ids across barriers."));
  }

  if (barrier_id_to_num_nodes_[barrier_id] == kBarrierTimedOut) {
    return xla::ToGrpcStatus(xla::FailedPrecondition(
        "A process timed out waiting at the barrier. Exiting early because the "
        "current process will also timeout."));
  }

  ++barrier_id_to_num_nodes_[barrier_id];

  absl::Duration timeout = absl::Milliseconds(request->timeout_milliseconds());
  auto all_nodes_at_barrier = [&]() {
    mu_.AssertHeld();
    return barrier_id_to_num_nodes_[barrier_id] == nodes_.size() ||
           !service_status_.ok();
  };
  // TODO(yashkatariya,hanyangtay): Do something similar to the coordination
  // service here.
  if (!mu_.AwaitWithTimeout(absl::Condition(&all_nodes_at_barrier), timeout)) {
    barrier_id_to_num_nodes_[barrier_id] = kBarrierTimedOut;
    return xla::ToGrpcStatus(tsl::errors::DeadlineExceeded(
        "Timed out after ", timeout,
        " waiting for all nodes to be at WaitAtBarrier()"));
  }

  if (!service_status_.ok()) {
    return xla::ToGrpcStatus(service_status_);
  }
  return ::grpc::Status::OK;
}

CoordinationServiceImpl::CoordinationServiceImpl(
    const DistributedRuntimeServiceImpl::Options& options,
    ::grpc::ServerBuilder* builder)
    : env_(options.env) {
  coord_service_ = EnableCoordinationService(options);
  coord_compute_pool_ = std::make_unique<tsl::thread::ThreadPool>(
      options.env, "CoordinationServiceRpcHandler",
      /*num_threads=*/4);
  coord_rpc_service_ = std::make_unique<tsl::GrpcCoordinationServiceImpl>(
      coord_compute_pool_.get(), builder);
  auto* grpc_coord_service =
      static_cast<tsl::GrpcCoordinationServiceImpl*>(coord_rpc_service_.get());
  grpc_coord_service->SetCoordinationServiceInstance(coord_service_.get());
  LOG(INFO) << "Experimental coordination service is enabled.";
}

CoordinationServiceImpl::~CoordinationServiceImpl() {
  // Service object must be destroyed to clear all pending RPCs before shutting
  // down the RPC service.
  coord_service_ = nullptr;
  static_cast<tsl::GrpcCoordinationServiceImpl*>(coord_rpc_service_.get())
      ->SetCoordinationServiceInstance(nullptr);
  coord_rpc_service_->Shutdown();
}

void CoordinationServiceImpl::StartRpcThread() {
  coord_rpc_thread_.reset(env_->StartThread(
      tsl::ThreadOptions(), "CoordinationServiceHandleRPCsLoop",
      [service = coord_rpc_service_.get()] { service->HandleRPCsLoop(); }));
}

xla::StatusOr<std::unique_ptr<DistributedRuntimeService>>
DistributedRuntimeService::Get(
    const std::string& address,
    std::shared_ptr<::grpc::ServerCredentials> credentials,
    const DistributedRuntimeServiceImpl::Options& options,
    bool use_coordination_service) {
  ::grpc::ServerBuilder builder;
  builder.AddListeningPort(address, credentials);
  VLOG(1) << "Distributed runtime service address " << address;
  auto service = std::make_unique<DistributedRuntimeService>(
      options, &builder, use_coordination_service);
  if (!service->server_) {
    return xla::Unknown("Failed to start RPC server");
  }
  LOG(INFO) << "Jax service listening on " << address;
  return service;
}

DistributedRuntimeService::DistributedRuntimeService(
    const DistributedRuntimeServiceImpl::Options& options,
    ::grpc::ServerBuilder* builder, bool use_coordination_service) {
  if (use_coordination_service) {
    coord_impl_ = std::make_unique<CoordinationServiceImpl>(options, builder);
    server_ = builder->BuildAndStart();
    coord_impl_->StartRpcThread();
  } else {
    impl_ = std::make_unique<DistributedRuntimeServiceImpl>(options);
    builder->RegisterService(impl_.get());
    server_ = builder->BuildAndStart();
  }
}

DistributedRuntimeService::~DistributedRuntimeService() { Shutdown(); }

void DistributedRuntimeService::Shutdown() {
  if (server_) {
    LOG(INFO) << "Jax service shutting down";
    server_->Shutdown();
    server_->Wait();
  }

  // Explicitly destroy coordination service before the gRPC server. This clears
  // all pending RPCs before the gRPC server is destroyed.
  coord_impl_ = nullptr;
  server_ = nullptr;
}

}  // namespace xla
