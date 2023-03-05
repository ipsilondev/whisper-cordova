/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/core/data/service/snapshot/snapshot_manager.h"

#include <memory>
#include <string>

#include "tensorflow/core/data/service/dispatcher.pb.h"
#include "tensorflow/core/data/service/test_util.h"
#include "tensorflow/tsl/lib/core/status_test_util.h"
#include "tensorflow/tsl/platform/env.h"
#include "tensorflow/tsl/platform/statusor.h"
#include "tensorflow/tsl/platform/test.h"

namespace tensorflow {
namespace data {
namespace {

TEST(SnapshotManagerTest, CreateStreamAssignment) {
  std::string snapshot_path = testing::LocalTempFilename();
  SnapshotRequest request;
  *request.mutable_dataset() = testing::RangeDataset(10);
  request.set_path(snapshot_path);
  *request.mutable_metadata() =
      testing::CreateDummyDistributedSnapshotMetadata();

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<SnapshotManager> snapshot_manager,
                          SnapshotManager::Start(request, Env::Default()));
  WorkerHeartbeatRequest heartbeat_request;
  WorkerHeartbeatResponse heartbeat_response;
  heartbeat_request.set_worker_address("localhost");
  TF_ASSERT_OK(
      snapshot_manager->WorkerHeartbeat(heartbeat_request, heartbeat_response));
  ASSERT_EQ(heartbeat_response.snapshot_tasks().size(), 1);
  EXPECT_EQ(heartbeat_response.snapshot_tasks(0).base_path(), snapshot_path);
  EXPECT_EQ(heartbeat_response.snapshot_tasks(0).stream_index(), 0);
  EXPECT_EQ(heartbeat_response.snapshot_tasks(0).num_sources(), 1);
}

}  // namespace
}  // namespace data
}  // namespace tensorflow
