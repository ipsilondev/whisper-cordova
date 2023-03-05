// Copyright 2022 The TensorFlow Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tensorflow/compiler/xla/service/cpu/runtime/custom_call.h"

#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "tensorflow/compiler/xla/primitive_util.h"
#include "tensorflow/compiler/xla/runtime/custom_call.h"
#include "tensorflow/compiler/xla/runtime/custom_call_registry.h"
#include "tensorflow/compiler/xla/runtime/executable.h"
#include "tensorflow/compiler/xla/service/custom_call_status_internal.h"
#include "tensorflow/compiler/xla/service/custom_call_target_registry.h"
#include "tensorflow/compiler/xla/service/hlo.pb.h"
#include "tensorflow/compiler/xla/xla.pb.h"

namespace xla {
namespace cpu {

using mlir::StringRef;
using mlir::succeeded;

using ::xla::runtime::CustomCall;
using ::xla::runtime::Executable;

// Disable all CustomCall checks in optimized build.
static constexpr CustomCall::RuntimeChecks RuntimeChecks() {
#if defined(NDEBUG)
  return CustomCall::RuntimeChecks::kNone;
#else
  return CustomCall::RuntimeChecks::kDefault;
#endif
}

// -------------------------------------------------------------------------- //

namespace {
struct XlaCustomCall {
  absl::Status operator()(CustomCall::RemainingArgs args, int32_t num_results,
                          bool output_tuple, StringRef call_target_name,
                          int32_t api_version) const;
  static XlaCustomCall Handler() { return XlaCustomCall(); }
};
}  // namespace

absl::Status XlaCustomCall::operator()(CustomCall::RemainingArgs args,
                                       int32_t num_results, bool output_tuple,
                                       StringRef call_target_name,
                                       int32_t api_version) const {
  // Find the Xla custom call handler.
  void* call_target = CustomCallTargetRegistry::Global()->Lookup(
      call_target_name.str(), "Host");
  if (!call_target) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Cannot find the Xla custom call handler ", call_target_name.str()));
  }

  // Prepare pointers to buffers to pass to the Xla custom call handler.
  llvm::SmallVector<void*> buffers;
  for (unsigned i = 0; i < args.size(); ++i) {
    // We use zero-sized memrefs to represent holes in custom calls with target
    // arguments mapping (see `CustomCallTargetArgMapping`).
    if (auto memref = args.get<runtime::FlatMemrefView>(i); succeeded(memref)) {
      buffers.push_back(memref->size_in_bytes == 0 ? nullptr : memref->data);
      continue;
    }
    if (auto strided = args.get<runtime::StridedMemrefView>(i);
        succeeded(strided)) {
      int64_t size_in_bytes = primitive_util::ByteWidth(strided->dtype);
      for (int64_t size : strided->sizes) size_in_bytes *= size;
      buffers.push_back(size_in_bytes == 0 ? nullptr : strided->data);
      continue;
    }
    return absl::InvalidArgumentError(
        "Failed to get arguments as (strided) memref view");
  }

  // Multiple result buffers are passed as a tuple, which is represented as a
  // buffer of pointers.
  void* result_buffer =
      !output_tuple ? buffers.back() : buffers.end() - num_results;

  // Original custom call API version that doesn't support returning status.
  if (api_version == CustomCallApiVersion::API_VERSION_ORIGINAL) {
    using XlaCustomCallType = void (*)(void* /*result*/, void** /*args*/);
    auto xla_call_target = reinterpret_cast<XlaCustomCallType>(call_target);

    xla_call_target(result_buffer, buffers.data());

    return absl::OkStatus();
  }

  // Xla Custom call API returning status.
  if (api_version == CustomCallApiVersion::API_VERSION_STATUS_RETURNING) {
    using XlaCustomCallType = void (*)(void* /*result*/, void** /*args*/,
                                       XlaCustomCallStatus* /*status*/);
    auto xla_call_target = reinterpret_cast<XlaCustomCallType>(call_target);

    XlaCustomCallStatus custom_call_status;
    xla_call_target(result_buffer, buffers.data(), &custom_call_status);

    if (auto message = CustomCallStatusGetMessage(&custom_call_status)) {
      return absl::InternalError(message.value());
    } else {
      return absl::OkStatus();
    }
  }

  return absl::InvalidArgumentError("Incorrect custom call API version");
}

static bool CustomCall(runtime::ExecutionContext* ctx, void** args,
                       void** attrs, void** rets) {
  static auto* handler = CustomCall::Bind("xla.cpu.custom_call")
                             .Arg<CustomCall::RemainingArgs>()  // args
                             .Attr<int32_t>("num_results")
                             .Attr<bool>("output_tuple")
                             .Attr<std::string_view>("call_target_name")
                             .Attr<int32_t>("api_version")
                             .To<RuntimeChecks()>(XlaCustomCall::Handler())
                             .release();
  return succeeded(Executable::Call(ctx, *handler, args, attrs, rets));
}

void PopulateXlaCpuCustomCall(runtime::DirectCustomCallRegistry& registry) {
  registry.Register("xla.cpu.custom_call", &xla::cpu::CustomCall);
}

}  // namespace cpu
}  // namespace xla
