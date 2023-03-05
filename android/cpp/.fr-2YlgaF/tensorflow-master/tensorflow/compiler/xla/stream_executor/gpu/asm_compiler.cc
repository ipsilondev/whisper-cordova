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

#include "tensorflow/compiler/xla/stream_executor/gpu/asm_compiler.h"

#include <array>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/base/const_init.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "tensorflow/compiler/xla/stream_executor/gpu/gpu_driver.h"
#include "tensorflow/tsl/platform/cuda_libdevice_path.h"
#include "tensorflow/tsl/platform/env.h"
#include "tensorflow/tsl/platform/errors.h"
#include "tensorflow/tsl/platform/path.h"
#include "tensorflow/tsl/platform/regexp.h"
#include "tensorflow/tsl/platform/statusor.h"
#include "tensorflow/tsl/platform/subprocess.h"

namespace stream_executor {

static tsl::StatusOr<absl::string_view> GetToolVersionString(
    absl::string_view binary_path) {
  static absl::Mutex mu(absl::kConstInit);
  static auto* seen_binary_paths ABSL_GUARDED_BY(mu) =
      new absl::flat_hash_map<std::string, std::string>();

  absl::MutexLock lock(&mu);
  auto it = seen_binary_paths->find(binary_path);
  if (it != seen_binary_paths->end()) {
    // Already checked this binary, nothing to do.
    return absl::string_view(it->second);
  }

  tsl::SubProcess binary;
  std::string binary_path_str(binary_path);
  binary.SetProgram(binary_path_str, {binary_path_str, "--version"});
  binary.SetChannelAction(tsl::CHAN_STDOUT, tsl::ACTION_PIPE);
  if (!binary.Start()) {
    return tsl::errors::Internal(
        absl::StrFormat("Couldn't invoke %s --version", binary_path));
  }

  std::string out;
  int exit_code = binary.Communicate(/*stdin_input=*/nullptr, &out,
                                     /*stderr_output=*/nullptr);
  if (exit_code != 0) {
    return tsl::errors::Internal(absl::StrFormat(
        "Running %s --version returned %d", binary_path, exit_code));
  }
  auto emplace_it = seen_binary_paths->emplace(binary_path, std::move(out));
  return absl::string_view(emplace_it.first->second);
}

tsl::StatusOr<std::array<int64_t, 3>> GetToolVersion(
    absl::string_view tool_path) {
  tsl::StatusOr<absl::string_view> tool_version =
      GetToolVersionString(tool_path);
  if (!tool_version.ok()) {
    return tsl::errors::FailedPrecondition(
        "Couldn't get ptxas/nvlink version string: ", tool_version.status());
  }
  std::array<int64_t, 3> version;
  std::string vmaj_str, vmin_str, vdot_str;
  if (!RE2::PartialMatch(tool_version.value(), R"(\bV(\d+)\.(\d+)\.(\d+)\b)",
                         &vmaj_str, &vmin_str, &vdot_str) ||
      !absl::SimpleAtoi(vmaj_str, &version[0]) ||
      !absl::SimpleAtoi(vmin_str, &version[1]) ||
      !absl::SimpleAtoi(vdot_str, &version[2])) {
    return tsl::errors::FailedPrecondition(
        "Couldn't parse ptxas/nvlink version in output of ", tool_path,
        " --version:\n", tool_version.value());
  }
  return version;
}

// Prints a warning if the ptxas at ptxas_path has known bugs.
//
// Only prints a warning the first time it's called for a particular value of
// ptxas_path.
//
// Locks on entry.˝
static void WarnIfBadPtxasVersion(absl::string_view ptxas_path) {
  tsl::StatusOr<std::array<int64_t, 3>> version = GetToolVersion(ptxas_path);
  if (!version.ok()) {
    LOG(WARNING) << "Couldn't get ptxas version : " << version.status();
    return;
  }

  if (std::make_tuple((*version)[0], (*version)[1]) < std::make_tuple(11, 1)) {
    LOG(ERROR) << "*** WARNING *** You are using ptxas " << (*version)[0] << "."
               << (*version)[1] << "." << (*version)[2]
               << ", which is older than 11.1. ptxas before 11.1 is known to "
                  "miscompile XLA code, leading to incorrect results or "
                  "invalid-address errors.\n";
  }
}

tsl::StatusOr<absl::Span<const uint8_t>> CompileGpuAsmOrGetCached(
    int device_ordinal, const char* ptx, GpuAsmOpts compilation_options) {
  using PtxCacheKey = std::tuple<int, std::string, GpuAsmOpts::PtxOptionsTuple>;
  using PtxCompilerResult = tsl::StatusOr<std::vector<uint8_t>>;
  static absl::Mutex ptx_cache_mutex(absl::kConstInit);
  static auto& ptx_cache ABSL_GUARDED_BY(ptx_cache_mutex) =
      *new absl::flat_hash_map<PtxCacheKey, PtxCompilerResult>();

  absl::MutexLock lock(&ptx_cache_mutex);
  PtxCacheKey cache_key{device_ordinal, std::string(ptx),
                        compilation_options.ToTuple()};
  auto it = ptx_cache.find(cache_key);
  if (it == ptx_cache.end()) {
    PtxCompilerResult compiled =
        CompileGpuAsm(device_ordinal, ptx, compilation_options);
    it = ptx_cache.emplace(cache_key, std::move(compiled)).first;
  }

  CHECK(it != ptx_cache.end());

  // Failed compilation attempts are cached.
  // Use separate status check and ValueOrDie invocation on ptx_cache
  // entry to avoid value moving introduced by TF_ASSIGN_OR_RETURN.

  if (ABSL_PREDICT_FALSE(!it->second.ok())) {
    return it->second.status();
  }

  const std::vector<uint8_t>& compiled = it->second.value();
  return absl::MakeSpan(compiled);
}

tsl::StatusOr<std::vector<uint8_t>> CompileGpuAsm(int device_ordinal,
                                                  const char* ptx_contents,
                                                  GpuAsmOpts options) {
  gpu::GpuDeviceHandle handle;
  TF_RETURN_IF_ERROR(gpu::GpuDriver::GetDevice(device_ordinal, &handle));
  int cc_major;
  int cc_minor;
  TF_RETURN_IF_ERROR(
      gpu::GpuDriver::GetComputeCapability(&cc_major, &cc_minor, handle));
  return CompileGpuAsm(cc_major, cc_minor, ptx_contents, options);
}

std::string FindCudaExecutable(const std::string& binary_name,
                               const std::string& preferred_cuda_dir) {
  static absl::Mutex mu(absl::kConstInit);
  static auto* seen_binary_paths ABSL_GUARDED_BY(mu) =
      new absl::flat_hash_map<std::pair<std::string, std::string>,
                              std::string>();

#if defined(PLATFORM_WINDOWS)
  const std::string binary_filename = binary_name + ".exe";
#else
  const std::string& binary_filename = binary_name;
#endif

  auto cache_key = std::make_pair(binary_name, preferred_cuda_dir);

  absl::MutexLock lock(&mu);
  auto it = seen_binary_paths->find(cache_key);
  if (it != seen_binary_paths->end()) {
    return it->second;
  }

  // Try searching in the default PATH first if applicable.
  if (tsl::PreferPtxasFromPath() &&
      GetToolVersionString(binary_filename).ok()) {
    VLOG(2) << "Using " << binary_filename;
    seen_binary_paths->emplace(std::move(cache_key), binary_filename);
    return binary_filename;
  }

  // Search in cuda root candidates.
  auto env = tsl::Env::Default();
  std::string binary_path;
  for (const std::string& cuda_root :
       tsl::CandidateCudaRoots(preferred_cuda_dir)) {
    binary_path = tsl::io::JoinPath(cuda_root, "bin", binary_filename);
    VLOG(2) << "Looking for " << binary_filename << " at " << binary_path;
    if (env->FileExists(binary_path).ok() &&
        GetToolVersionString(binary_path).ok()) {
      break;
    }
  }
  if (!env->FileExists(binary_path).ok()) {
    // Give up and just rely on subprocess invocation to find the correct
    // binary. This won't work, in all probability, given we already tried that
    // above, but it's the best we can do.
    VLOG(2) << "Unable to find " << binary_name;
    binary_path = binary_filename;
  }
  VLOG(2) << "Using " << binary_filename << " at " << binary_path;
  seen_binary_paths->emplace(std::move(cache_key), binary_path);
  return binary_path;
}

static void LogPtxasTooOld(const std::string& ptxas_path, int cc_major,
                           int cc_minor) {
  using AlreadyLoggedSetTy =
      absl::flat_hash_set<std::tuple<std::string, int, int>>;

  static absl::Mutex* mutex = new absl::Mutex;
  static AlreadyLoggedSetTy* already_logged = new AlreadyLoggedSetTy;

  absl::MutexLock lock(mutex);

  if (already_logged->insert(std::make_tuple(ptxas_path, cc_major, cc_minor))
          .second) {
    LOG(WARNING) << "Falling back to the CUDA driver for PTX compilation; "
                    "ptxas does not support CC "
                 << cc_major << "." << cc_minor;
    LOG(WARNING) << "Used ptxas at " << ptxas_path;
  }
}

static void AppendArgsFromOptions(GpuAsmOpts options,
                                  std::vector<std::string>& args) {
  if (options.disable_gpuasm_optimizations) {
    args.push_back("-O0");
  }
  args.insert(args.end(), options.extra_flags.begin(),
              options.extra_flags.end());
}

tsl::StatusOr<std::array<int64_t, 3>> GetAsmCompilerVersion(
    const std::string& preferred_cuda_dir) {
  std::string ptxas_path = FindCudaExecutable("ptxas", preferred_cuda_dir);
  return GetToolVersion(ptxas_path);
}

tsl::StatusOr<std::vector<uint8_t>> CompileGpuAsm(int cc_major, int cc_minor,
                                                  const char* ptx_contents,
                                                  GpuAsmOpts options) {
  std::string ptxas_path =
      FindCudaExecutable("ptxas", options.preferred_cuda_dir);

  WarnIfBadPtxasVersion(ptxas_path);

  // Write ptx into a temporary file.
  std::string ptx_path;
  auto env = tsl::Env::Default();
  if (!env->LocalTempFilename(&ptx_path)) {
    return tsl::errors::Internal("couldn't get temp PTX file name");
  }
  TF_RETURN_IF_ERROR(tsl::WriteStringToFile(env, ptx_path, ptx_contents));
  VLOG(2) << "ptx written to: " << ptx_path;

  absl::Cleanup ptx_cleaner = [&ptx_path] {
    TF_CHECK_OK(tsl::Env::Default()->DeleteFile(ptx_path));
  };

  // Invoke ptxas and collect its output.
  std::string cubin_path;
  if (!env->LocalTempFilename(&cubin_path)) {
    return tsl::errors::Internal("couldn't get temp CUBIN file name");
  }
  absl::Cleanup cubin_cleaner = [&cubin_path] {
    // CUBIN file may never be created, so the failure to delete it should not
    // produce TF error.
    tsl::Env::Default()->DeleteFile(cubin_path).IgnoreError();
  };
  tsl::SubProcess ptxas_info_dumper;
  std::vector<std::string> ptxas_args = {
      ptxas_path,
      ptx_path,
      "-o",
      cubin_path,
      absl::StrCat("-arch=sm_", cc_major, cc_minor),
      "--warn-on-spills"};
  if (VLOG_IS_ON(2)) {
    ptxas_args.push_back("-v");
  }
  AppendArgsFromOptions(options, ptxas_args);
  if (VLOG_IS_ON(3)) {
    VLOG(3) << absl::StrJoin(ptxas_args, " ");
  }

  ptxas_info_dumper.SetProgram(ptxas_path, ptxas_args);
  ptxas_info_dumper.SetChannelAction(tsl::CHAN_STDERR, tsl::ACTION_PIPE);
  if (!ptxas_info_dumper.Start()) {
    return tsl::errors::Internal("Failed to launch ptxas");
  }
  std::string stderr_output;
  int exit_status = ptxas_info_dumper.Communicate(
      /*stdin_input=*/nullptr, /*stdout_output=*/nullptr, &stderr_output);
  if (exit_status != 0) {
    //  It happens when the ptxas installed is too old for the current GPU.
    //  Example error message associated with this error code:
    //      ptxas fatal   : Value 'sm_80' is not defined for option 'gpu-name'
    // In that case, fallback to the driver for compilation
    if (absl::StartsWith(stderr_output, "ptxas fatal   : Value '") &&
        absl::StrContains(stderr_output,
                          "is not defined for option 'gpu-name'")) {
      LogPtxasTooOld(ptxas_path, cc_major, cc_minor);
      return tsl::errors::Unimplemented(
          ptxas_path, " ptxas too old. Falling back to the driver to compile.");
    }

    return tsl::errors::Internal(
        absl::StrFormat("ptxas exited with non-zero error code %d, output: %s",
                        exit_status, stderr_output));
  }
  // Print the verbose output of ptxas.
  if (!stderr_output.empty()) {
    if (absl::StrContains(stderr_output, "warning")) {
      LOG(INFO) << stderr_output;
    } else {
      VLOG(2) << stderr_output;
    }
  }

  // Read in the result of compilation and return it as a byte vector.
  std::string cubin;
  TF_RETURN_IF_ERROR(
      tsl::ReadFileToString(tsl::Env::Default(), cubin_path, &cubin));
  std::vector<uint8_t> cubin_vector(cubin.begin(), cubin.end());
  return cubin_vector;
}

tsl::StatusOr<std::vector<uint8_t>> BundleGpuAsm(
    std::vector<CubinOrPTXImage> images, GpuAsmOpts options) {
  std::string fatbinary_path =
      FindCudaExecutable("fatbinary", options.preferred_cuda_dir);

  // Write images to temporary files.
  std::vector<std::string> image_paths;
  auto env = tsl::Env::Default();
  for (const CubinOrPTXImage& img : images) {
    std::string img_path;
    if (!env->LocalTempFilename(&img_path)) {
      return tsl::errors::Internal(
          "Could not get temporary filenames for images.");
    }
    TF_RETURN_IF_ERROR(tsl::WriteStringToFile(
        env, img_path, std::string(img.bytes.begin(), img.bytes.end())));
    VLOG(2) << "image written to " << img_path;
    image_paths.push_back(std::move(img_path));
  }
  absl::Cleanup image_files_cleaner = [&image_paths] {
    for (const auto& path : image_paths) {
      TF_CHECK_OK(tsl::Env::Default()->DeleteFile(path));
    }
  };

  // Prepare temorary result file.
  std::string result_path;
  if (!env->LocalTempFilename(&result_path)) {
    return tsl::errors::Internal(
        "Could not get temporary filename for fatbin result.");
  }
  absl::Cleanup result_file_cleaner = [&result_path] {
    // This file may never be created, so the failure to delete it should not
    // propagate to TF.
    tsl::Env::Default()->DeleteFile(result_path).IgnoreError();
  };

  // Compute the ptxas options that were used to produce the cubins.
  std::vector<std::string> ptxas_options;
  AppendArgsFromOptions(options, ptxas_options);

  // Invoke fatbinary and collect its output.
  tsl::SubProcess fatbinary;
  std::vector<std::string> fatbinary_args = {
      fatbinary_path, "--64", "--link", "--compress-all",
      absl::StrCat("--create=", result_path)};
  if (!ptxas_options.empty()) {
    auto command_line = absl::StrJoin(ptxas_options, " ");
    fatbinary_args.push_back(absl::StrFormat("--cmdline=%s", command_line));
  }
  assert(images.size() == image_paths.size());
  for (int i = 0; i < images.size(); i++) {
    fatbinary_args.push_back(absl::StrFormat(
        "--image=profile=%s,file=%s", images[i].profile, image_paths[i]));
  }
  if (VLOG_IS_ON(3)) {
    VLOG(3) << absl::StrJoin(fatbinary_args, " ");
  }
  fatbinary.SetProgram(fatbinary_path, fatbinary_args);
  fatbinary.SetChannelAction(tsl::CHAN_STDERR, tsl::ACTION_PIPE);
  if (!fatbinary.Start()) {
    return tsl::errors::Internal("Failed to launch fatbinary.");
  }
  std::string stderr_output;
  int exit_status = fatbinary.Communicate(
      /*stdin_input=*/nullptr, /*stdout_output=*/nullptr, &stderr_output);
  if (exit_status != 0) {
    return tsl::errors::Internal(absl::StrFormat(
        "fatbinary exited with non-zero error code %d, output: %s", exit_status,
        stderr_output));
  }
  if (!stderr_output.empty()) {
    VLOG(2) << stderr_output;
  }

  // Read in the result and return it as a byte vector.
  std::string result_blob;
  TF_RETURN_IF_ERROR(
      tsl::ReadFileToString(tsl::Env::Default(), result_path, &result_blob));
  return std::vector<uint8_t>(result_blob.begin(), result_blob.end());
}

static std::string findRocmExecutable(const std::string& binary_relative_path,
                                      const std::string& rocm_root_dir) {
  auto env = tsl::Env::Default();
  std::string binary_path =
      tsl::io::JoinPath(rocm_root_dir, binary_relative_path);
  VLOG(2) << "Looking for " << binary_relative_path << " at " << rocm_root_dir;
  if (!env->FileExists(binary_path).ok()) {
    binary_path = absl::StrCat("<", binary_path, " - NOT FOUND>");
  }
  return binary_path;
}

tsl::StatusOr<std::vector<uint8_t>> BundleGpuAsm(
    std::vector<HsacoImage> images, const std::string rocm_root_dir) {
  std::string clang_offload_bundler_path =
      findRocmExecutable("llvm/bin/clang-offload-bundler", rocm_root_dir);

  // Initialise the "--inputs" / "--targets" arguments for the
  // clang-offload-bundler with a dummy file / host target triple...
  // clang-offload-bundler requires 1 and only 1 host target triple
  std::ostringstream inputs_list;
  std::ostringstream targets_list;

  inputs_list << "/dev/null";
  targets_list << "host-x86_64-unknown-linux";

  // Write images to temporary files.
  std::vector<std::string> image_paths;
  auto env = tsl::Env::Default();
  for (const HsacoImage& img : images) {
    std::string img_path;
    if (!env->LocalTempFilename(&img_path)) {
      return tsl::errors::Internal(
          "Could not get temporary filenames for images.");
    }
    TF_RETURN_IF_ERROR(tsl::WriteStringToFile(
        env, img_path, std::string(img.bytes.begin(), img.bytes.end())));
    VLOG(2) << "image written to " << img_path;
    inputs_list << "," << img_path;
    targets_list << ",hip-amdgcn-amd-amdhsa-" << img.gfx_arch;
    image_paths.push_back(std::move(img_path));
  }
  absl::Cleanup image_files_cleaner = [&image_paths] {
    for (const auto& path : image_paths) {
      TF_CHECK_OK(tsl::Env::Default()->DeleteFile(path));
    }
  };

  // Prepare temorary result file.
  std::string result_path;
  if (!env->LocalTempFilename(&result_path)) {
    return tsl::errors::Internal(
        "Could not get temporary filename for fatbin result.");
  }
  absl::Cleanup result_file_cleaner = [&result_path] {
    // This file may never be created, so the failure to delete it should not
    // propagate to TF.
    tsl::Env::Default()->DeleteFile(result_path).IgnoreError();
  };

  // Invoke clang_offload_bundler and collect its output.
  tsl::SubProcess clang_offload_bundler;
  std::vector<std::string> clang_offload_bundler_args = {
      clang_offload_bundler_path, absl::StrCat("--inputs=", inputs_list.str()),
      absl::StrCat("--targets=", targets_list.str()), "--type=o",
      absl::StrCat("--outputs=", result_path)};
  if (VLOG_IS_ON(3)) {
    VLOG(3) << absl::StrJoin(clang_offload_bundler_args, " ");
  }
  clang_offload_bundler.SetProgram(clang_offload_bundler_path,
                                   clang_offload_bundler_args);
  clang_offload_bundler.SetChannelAction(tsl::CHAN_STDERR, tsl::ACTION_PIPE);
  if (!clang_offload_bundler.Start()) {
    return tsl::errors::Internal("Failed to launch clang_offload_bundler.");
  }
  std::string stderr_output;
  int exit_status = clang_offload_bundler.Communicate(
      /*stdin_input=*/nullptr, /*stdout_output=*/nullptr, &stderr_output);
  if (exit_status != 0) {
    return tsl::errors::Internal(
        absl::StrFormat("clang_offload_bundler exited with non-zero error "
                        "code %d, output: %s",
                        exit_status, stderr_output));
  }
  if (!stderr_output.empty()) {
    VLOG(2) << stderr_output;
  }

  // Read in the result and return it as a byte vector.
  std::string result_blob;
  TF_RETURN_IF_ERROR(
      tsl::ReadFileToString(tsl::Env::Default(), result_path, &result_blob));
  return std::vector<uint8_t>(result_blob.begin(), result_blob.end());
}

}  // namespace stream_executor
