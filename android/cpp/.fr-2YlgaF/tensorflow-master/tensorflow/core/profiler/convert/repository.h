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

#ifndef TENSORFLOW_CORE_PROFILER_CONVERT_REPOSITORY_H_
#define TENSORFLOW_CORE_PROFILER_CONVERT_REPOSITORY_H_

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "tensorflow/core/platform/path.h"
#include "tensorflow/core/platform/statusor.h"
#include "tensorflow/core/profiler/protobuf/xplane.pb.h"

namespace tensorflow {
namespace profiler {

// File system directory snapshot of a profile session.
class SessionSnapshot {
 public:
  // Performs validation and creates SessionSnapshot.
  // <xspace_paths> are the file paths to XSpace protos.
  // Optionally, <xspaces> can contain the XSpace protos pre-loaded by the
  // profiler plugin.
  static StatusOr<SessionSnapshot> Create(
      std::vector<std::string> xspace_paths,
      std::optional<std::vector<std::unique_ptr<XSpace>>> xspaces);

  // Returns the number of XSpaces in the profile session.
  size_t XSpaceSize() const { return xspace_paths_.size(); }

  // Gets XSpace proto.
  // The caller of this function will take ownership of the XSpace.
  StatusOr<std::unique_ptr<XSpace>> GetXSpace(size_t index) const;

  // Gets host name.
  std::string GetHostname(size_t index) const;

  // Gets the run directory of the profile session.
  absl::string_view GetSessionRunDir() const { return session_run_dir_; }

 private:
  SessionSnapshot(std::vector<std::string> xspace_paths,
                  std::optional<std::vector<std::unique_ptr<XSpace>>> xspaces)
      : xspace_paths_(std::move(xspace_paths)), xspaces_(std::move(xspaces)) {
    session_run_dir_ = tensorflow::io::Dirname(xspace_paths_.at(0));
  }

  // File paths to XSpace protos.
  std::vector<std::string> xspace_paths_;
  // The run directory of the profile session.
  absl::string_view session_run_dir_;

  // XSpace protos pre-loaded by the profiler plugin.
  // TODO(profiler): Use blobstore paths to initialize SessionSnapshot instead
  // of using pre-loaded XSpaces.
  mutable std::optional<std::vector<std::unique_ptr<XSpace>>> xspaces_;
};

}  // namespace profiler
}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_PROFILER_CONVERT_REPOSITORY_H_
