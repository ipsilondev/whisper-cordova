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

#ifndef TENSORFLOW_COMPILER_XLA_PJRT_HOST_CALLBACK_H_
#define TENSORFLOW_COMPILER_XLA_PJRT_HOST_CALLBACK_H_

#include <atomic>
#include <functional>
#include <utility>
#include <vector>

#include "tensorflow/compiler/xla/pjrt/pjrt_client.h"

// The following provides an API for implementing host callbacks on top of
// PjRT's send/recv interface (see xla::SendCallback and xla::RecvCallback).
// While this is not the only way to implement host callbacks using send/recv,
// it is provided as an example implementation that encapsulates common
// mechanisms for host callbacks in a framework-agnostic manner.

namespace xla {

// A thread-safe queue for passing PjRtChunk objects for e.g. from Send ops to
// Recv ops.
class ThreadSafePjRtChunkQueue {
 public:
  // Push a PjRtChunk into the queue.
  void Push(PjRtChunk chunk) {
    absl::MutexLock lock(&mu_);
    queue_.push_back(std::move(chunk));
  }

  // Pop a PjRtChunk from the queue. This method blocks if the queue is empty.
  PjRtChunk Pop() {
    absl::MutexLock lock(&mu_);
    auto cond = [this]() {
      mu_.AssertHeld();
      return !queue_.empty();
    };
    mu_.Await(absl::Condition(&cond));
    auto chunk = std::move(queue_.front());
    queue_.pop_front();
    return chunk;
  }

 private:
  absl::Mutex mu_;
  std::deque<PjRtChunk> queue_ ABSL_GUARDED_BY(mu_);
};

struct HostCallbackArgInfo {
  // The channel_id associated with this value in HLO.
  uint16_t channel_id;
  // The host shape for thie value.
  Shape shape;
};

struct HostCallback {
  // The metadata (e.g. channel_id, shape) for the operands and results.
  std::vector<HostCallbackArgInfo> operands;
  std::vector<HostCallbackArgInfo> results;

  // The host callback function takes two pointer arrays, each element of which
  // points to allocated host buffer according to corresponding operand or
  // result's shape. The first is for the outputs and the second is for the
  // inputs. The buffers are only guaranteed to be alive during the call. The
  // callback can also return error status to indicate the entire execution
  // should fail.
  std::function<Status(void**, void**)> callback;
};

// A helper class that maintains the send/recv states for a host callback.
class HostCallbackContext {
 public:
  HostCallbackContext(HostCallback host_callback, PjRtClient* client)
      : HostCallbackContext(std::move(host_callback),
                            client->GetPjRtHostMemoryForDeviceManager()) {}

  HostCallbackContext(
      HostCallback host_callback,
      PjRtHostMemoryForDeviceManager* host_memory_for_device_manager)
      : host_callback_(std::move(host_callback)),
        host_memory_for_device_manager_(host_memory_for_device_manager),
        args_(host_callback_.operands.size()),
        result_channels_(host_callback_.results.size()),
        ready_count_(args_.size()) {
    CHECK(host_memory_for_device_manager_);

    for (auto& channel : result_channels_) {
      channel = std::make_unique<ThreadSafePjRtChunkQueue>();
    }
  }

  Status OnSend(int arg_num, const PjRtTransferMetadata& metadata,
                PjRtChunk data);

  void Receive(int res_num, const PjRtTransferMetadata& metadata,
               CopyToDeviceStream& stream);

  const HostCallback& host_callback() const { return host_callback_; }

 private:
  HostCallback host_callback_;
  PjRtHostMemoryForDeviceManager* host_memory_for_device_manager_ = nullptr;
  std::vector<PjRtChunk> args_;
  std::vector<std::unique_ptr<ThreadSafePjRtChunkQueue>> result_channels_;
  std::atomic<int> ready_count_;
};

// The execution states for host callbacks for all replicas. The states are kept
// as vectors of vectors. The outer vector corresponds to the execution
// replicas. The inner vector is a list of host callback states for a single
// execution replica.
struct HostCallbackStates {
  std::vector<std::vector<std::unique_ptr<HostCallbackContext>>> contexts;
  std::vector<std::vector<SendCallback>> send_callbacks;
  std::vector<std::vector<RecvCallback>> recv_callbacks;
};

// Creates the execution context for the `host_callback` for one replica.
std::unique_ptr<HostCallbackContext>
CreateHostCallbackStateAndAppendSendRecvCallbacks(
    HostCallback host_callback,
    PjRtHostMemoryForDeviceManager* host_memory_for_device_manager,
    std::vector<SendCallback>& send_callbacks,
    std::vector<RecvCallback>& recv_callbacks);

}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_PJRT_HOST_CALLBACK_H_
