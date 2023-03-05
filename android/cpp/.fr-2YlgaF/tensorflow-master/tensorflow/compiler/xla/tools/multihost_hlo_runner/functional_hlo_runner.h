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

#ifndef TENSORFLOW_COMPILER_XLA_TOOLS_MULTIHOST_HLO_RUNNER_FUNCTIONAL_HLO_RUNNER_H_
#define TENSORFLOW_COMPILER_XLA_TOOLS_MULTIHOST_HLO_RUNNER_FUNCTIONAL_HLO_RUNNER_H_

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "tensorflow/compiler/xla/hlo/ir/hlo_module.h"
#include "tensorflow/compiler/xla/literal.h"
#include "tensorflow/compiler/xla/pjrt/pjrt_client.h"
#include "tensorflow/compiler/xla/pjrt/pjrt_executable.h"
#include "tensorflow/tsl/platform/logging.h"
#include "tensorflow/tsl/platform/statusor.h"

namespace xla {

// Supported input formats for the input HLO module.
enum class InputFormat {
  kText,                 // Text format.
  kProtoText,            // Protobuf text format.
  kProtoBinary,          // Protobuf binary format.
  kSnapshotProtoBinary,  // HloSnapshot protobuf binary format. Can be dumped by
                         // TensorFlow by setting the environment variable
                         // xla_dump_hlo_snapshots.
};

bool AbslParseFlag(absl::string_view text, InputFormat* input_format,
                   std::string* error);
std::string AbslUnparseFlag(InputFormat input_format);

// FunctionalHloRunner takes an HLO module as input and runs the HLO module
// on a single or multiple hosts with various options (e.g. SPMD). The HLO
// module can be pre- or post-optimizations.
class FunctionalHloRunner {
 public:
  // This class has only static methods.
  FunctionalHloRunner() = delete;

  using LiteralVec = std::vector<Literal>;
  using ShapeVec = std::vector<Shape>;
  using PerDeviceLiteralVecType = absl::btree_map<int, LiteralVec>;
  using PerDeviceShapeVecType = absl::btree_map<int, ShapeVec>;
  using PerDeviceIndexVecType = absl::btree_map<int, std::vector<int>>;

  enum class LogOutputMode { kLogOutput, kNotLogOutput };

  enum class HloPassesMode {
    // Only call the XLA compiler's RunBackend to compile the module. This is
    // used to run a post-optimization HLO module (dumped as
    // 'xxx.after_optimizations.hlo.xxx').
    kRunXLABackendOnly,
    // Calls Compile (i.e., both RunHloPasses and RunBackend) to compile the
    // module, but disables all HLO passes.
    kDisableAllHloPasses,
    // Standard XLA compilation by calling Compile (or both RunHloPasses and
    // RunBackend). This is used to run a pre-optimizations module.
    kStandardCompile
  };

  enum class SpmdMode { kUseSpmdPartitioning, kNotUseSpmdPartitioning };

  enum class SpmdPartitionedMode {
    kIsSpmdPartitionedModule,
    kIsNotSpmdPartitionedModule
  };

  enum class XlaTextDumpMode { kDumpAsText, kNotDumpAsText };

  enum class XlaProtoDumpMode { kDumpAsProto, kNotDumpAsProto };

  enum class ModuleArgumentMode {
    // Use device ID (casted to proper type) as arguments.
    kUseDeviceIdAsInput,
    // Use random values as arguments.
    kUseRandomInputs,
    // Use random values as arguments, and different local devices share the
    // same argument values.
    kUseSharedRandomInputs,
    // Use arguments which have all of their bytes set to 0 (not respecting any
    // constraints on the range).
    kUseZerosAsInput,
    // Use uninitialized device buffers as arguments (not respecting any
    // constraints on the range). This drastically reduces
    // the host memory usage and the startup time.
    kUninitialized,
  };

  enum class ModuleOutputMode {
    // Return output from all devices.
    kReturnOutputs,
    // Do not return output from any device.
    kNotReturnOutputs,
    // Return the output only from the logical device 0.
    kReturnDevice0Outputs
  };

  // The options controlling the preprocessing of the HLO before it's compiled
  // and executed.
  struct PreprocessingOptions {
    // This indicates whether the module is the partitioned result of SPMD. If
    // yes, we will add (replicated) sharding annotations to the module.
    SpmdPartitionedMode spmd_partitioned_mode =
        SpmdPartitionedMode::kIsNotSpmdPartitionedModule;
    // If set, we will flatten all while loops to the specified number of
    // iterations.
    std::optional<int> while_execution_count = std::nullopt;
    // If set, we will remove all infeed and outfeed operations.
    bool remove_infeed_outfeed = true;

    // Should we flatten all while loops?
    bool flatten_while_loop() const {
      return while_execution_count.has_value();
    }

    // Is the module the partitioned result of SPMD?
    bool is_spmd_partitioned_module() const {
      return spmd_partitioned_mode ==
             SpmdPartitionedMode::kIsSpmdPartitionedModule;
    }
  };

  // The options controlling the compilation of the HLO module.
  //
  // A CompileOptions object can be created from this with CreateCompileOptions.
  struct RawCompileOptions {
    HloPassesMode hlo_passes_mode = HloPassesMode::kStandardCompile;
    SpmdMode spmd_mode = SpmdMode::kNotUseSpmdPartitioning;
    // We can set additional build options by specifying an ExecutionOptions
    // message.
    //
    // It can also specify the number of replicas and partitions - in
    // that case we don't have to set num_replicas and num_partitions.
    std::optional<ExecutionOptions> execution_options = std::nullopt;
    std::optional<int> num_replicas = 1;
    std::optional<int> num_partitions = 1;
    // See the comment on xla::MultiSliceConfig.
    std::optional<int> num_slices = std::nullopt;
    // A directory to dump xla debug data to.
    std::string xla_dump_to = "";
    XlaTextDumpMode xla_text_dump_mode = XlaTextDumpMode::kNotDumpAsText;
    XlaProtoDumpMode xla_proto_dump_mode = XlaProtoDumpMode::kNotDumpAsProto;
  };

  // The options controlling the execution of the HLO module.
  struct RunningOptions {
    // Option controlling the inputs of the HLO.
    ModuleArgumentMode module_argument_mode =
        ModuleArgumentMode::kUseRandomInputs;
    // Option controlling the outputs of the HLO.
    ModuleOutputMode module_output_mode = ModuleOutputMode::kReturnOutputs;
    // Repeatedly execute the HLO for this many times.
    size_t num_repeats = 1;
    // This indicates whether we log the inputs and outputs to stderr.
    LogOutputMode log_input_output_mode = LogOutputMode::kNotLogOutput;
    const MultiSliceConfig* multi_slice_config = nullptr;

    // Should we log the inputs and outputs to stderr?
    bool log_input_output() const {
      return log_input_output_mode == LogOutputMode::kLogOutput;
    }
  };

  struct HloModuleAndArguments {
    std::unique_ptr<HloModule> hlo_module;
    std::vector<Literal> arguments;
  };

  struct ReplicasAndPartitions {
    int replicas = 1;
    int partitions = 1;
  };

  // Create a PjRtClient which can run HLOs on GPU.
  static StatusOr<std::unique_ptr<PjRtClient>> CreateGpuClient();

  // Loads an ExecutionOptions proto (which can be used in RawCompileOptions).
  static StatusOr<ExecutionOptions> LoadExecutionOptions(
      absl::string_view path);

  // Creates the compilation options.
  //
  // If RawCompileOptions::num_slices is set, the
  // CompileOptions::device_assignment has to be set manually.
  static StatusOr<CompileOptions> CreateCompileOptions(
      const PjRtClient& client,
      const FunctionalHloRunner::RawCompileOptions& raw_options,
      int task_id = 0);

  // Runs on HLO module and dumps the output if needed.
  //
  // This is the highest level API in this file.
  static Status LoadAndRunAndDump(
      PjRtClient& client,
      const xla::FunctionalHloRunner::PreprocessingOptions& preproc_options,
      const xla::FunctionalHloRunner::RawCompileOptions& raw_compile_options,
      const xla::FunctionalHloRunner::RunningOptions& running_options,
      absl::Span<const std::string> hlo_files, InputFormat input_format,
      std::string dump_output_to = "", int task_id = 0);

  // Loads an HLO module from hlo_file according to input_format and run it.
  // The HLO module is run with the provided arguments if the arguments map is
  // not empty. Otherwise, use arguments from the HLO file or fake arguments.
  // The hlo file might be a HLO snapshot and thus contain arguments, otherwise
  // it is run with fake arguments.
  static StatusOr<PerDeviceLiteralVecType> LoadAndRun(
      PjRtClient& client, const PreprocessingOptions& preproc_options,
      const CompileOptions& compile_options,
      const RunningOptions& running_options,
      absl::Span<const std::string> hlo_files, InputFormat input_format,
      const PerDeviceLiteralVecType& arguments = {});

  // Loads an HLO module from hlo_file according to input_format and run it.
  // The module arguments are provided by `argument_literals`. The arguments per
  // device is defined by the `per_device_index_vec`, which should contain a
  // vector of indices for each local device. This means different device may
  // use the same argument literals. This is essential to run HLO modules with
  // large arguments (e.g., models with large weights).
  static StatusOr<PerDeviceLiteralVecType> LoadAndRun(
      PjRtClient& client, const PreprocessingOptions& preproc_options,
      const CompileOptions& compile_options,
      const RunningOptions& running_options,
      absl::Span<const std::string> hlo_files, InputFormat input_format,
      const LiteralVec& argument_literals,
      const PerDeviceIndexVecType& per_device_index_vec);

  // Compiles and runs the given HLO module with the given arguments for each
  // device. The given arguments is a map from device ID to a list of arguments.
  // If the arguments map is empty, the HLO module is run with fake arguments.
  static StatusOr<PerDeviceLiteralVecType> CompileAndRun(
      PjRtClient& client, const PreprocessingOptions& preproc_options,
      const CompileOptions& compile_options,
      const RunningOptions& running_options, HloModule* hlo_module,
      const PerDeviceLiteralVecType& arguments = {});

  // Compiles and runs the given HLO module with the given arguments for each
  // device. The module arguments are provided by `argument_literals`. The
  // arguments per device is defined by the `per_device_index_vec`, which should
  // contain a vector of indices for each local device. This means different
  // devices may use the same argument literals. This is essential to run HLO
  // modules with large arguments (e.g., models with large weights).
  static StatusOr<PerDeviceLiteralVecType> CompileAndRun(
      PjRtClient& client, const PreprocessingOptions& preproc_options,
      const CompileOptions& compile_options,
      const RunningOptions& running_options, HloModule* hlo_module,
      const LiteralVec& argument_literals,
      const PerDeviceIndexVecType& argument_indices);

  // Compiles the HLO module.
  static StatusOr<std::unique_ptr<PjRtLoadedExecutable>> Compile(
      PjRtClient& client, HloModule* hlo_module,
      const PreprocessingOptions& preproc_options,
      const CompileOptions& compile_options);

  // Runs the executable.
  static StatusOr<PerDeviceLiteralVecType> Run(
      PjRtClient& client, PjRtLoadedExecutable* executable,
      const PerDeviceLiteralVecType& arguments,
      const RunningOptions& running_options);

  // Runs the executable, where the module arguments are provided through
  // a shared literal vector and per-device indices.
  static StatusOr<PerDeviceLiteralVecType> Run(
      PjRtClient& client, PjRtLoadedExecutable* executable,
      const LiteralVec& argument_literals,
      const PerDeviceIndexVecType& argument_indices,
      const RunningOptions& running_options);

  static StatusOr<std::unique_ptr<HloModule>> ReadModuleFromHloTextFile(
      absl::string_view hlo_file);
  static StatusOr<std::unique_ptr<HloModule>> ReadModuleFromBinaryProtoFile(
      absl::string_view hlo_file);
  static StatusOr<std::unique_ptr<HloModule>> ReadModuleFromTextProtoFile(
      absl::string_view hlo_file);

  static StatusOr<HloModuleAndArguments> ReadModuleFromSnapshotBinaryProtoFile(
      absl::string_view hlo_file);
  static StatusOr<HloModuleAndArguments> LoadHloModuleAndArguments(
      absl::string_view hlo_file, InputFormat input_format);

  static StatusOr<std::unique_ptr<HloModule>> ReadModuleFromString(
      absl::string_view hlo_text);

  static StatusOr<std::unique_ptr<HloModule>> ReadModuleFromProto(
      const HloModuleProto& proto);

  // This would ideally be private, but we need it for the implementation of
  // MultihostHloRunner.
  static Status PrepareHloModuleForCompilation(
      HloModule* hlo_module, const PreprocessingOptions& preproc_options);
  // This would ideally be private, but we need it for the implementation of
  // MultihostHloRunner.
  static CompileOptions CompleteCompileOptions(const HloModule& hlo_module,
                                               CompileOptions compile_options);

  static Status DumpOutput(
      const FunctionalHloRunner::PerDeviceLiteralVecType& output,
      absl::string_view dump_output_to, int task_id);

 private:
  // Calculates the requested number of replicas and partitions.
  //
  // The explicit num_replicas and num_partitions options override
  // execution_options.
  //
  // Regarding the num_slices parameter, see the comment on
  // xla::MultiSliceConfig.
  static ReplicasAndPartitions GetReplicasAndPartitions(
      const std::optional<ExecutionOptions>& execution_options,
      int device_count, const std::optional<int>& num_replicas,
      const std::optional<int>& num_partitions, int num_slices = 1);

  // Creates an ExecutableBuildOptions using the specified ExecutionOptions.
  static ExecutableBuildOptions
  CreateExecutableBuildOptionsFromExecutionOptions(
      const ExecutionOptions& execution_options);

  static absl::Span<PjRtDevice* const> GetLocalDevices(
      const PjRtClient& client);

  // Creates fake arguments to run the given executable.
  static StatusOr<std::vector<std::vector<std::unique_ptr<PjRtBuffer>>>>
  CreateArgumentsOnDevice(PjRtClient& client,
                          const PjRtLoadedExecutable* executable,
                          const RunningOptions& running_options,
                          bool flatten_arguments = false);

  // Creates uninitialized arguments to run the given executable.
  static StatusOr<std::vector<std::vector<std::unique_ptr<PjRtBuffer>>>>
  CreateUninitializedArgumentsOnDevice(PjRtClient& client,
                                       const PjRtLoadedExecutable* executable,
                                       const RunningOptions& running_options,
                                       bool flatten_arguments = false);

  // Creates argument buffers based on the given arguments map. Note that the
  // arguments might be invalid when arguments are destructed.
  static StatusOr<std::vector<std::vector<std::unique_ptr<PjRtBuffer>>>>
  CopyArgumentsToDevice(PjRtClient& client,
                        absl::Span<PjRtDevice* const> addressable_devices,
                        const PerDeviceLiteralVecType& arguments,
                        bool log_input);

  static StatusOr<std::vector<std::vector<std::unique_ptr<PjRtBuffer>>>>
  CopyArgumentsToDevice(PjRtClient& client,
                        absl::Span<PjRtDevice* const> addressable_devices,
                        const LiteralVec& argument_literals,
                        const PerDeviceIndexVecType& argument_indices,
                        bool log_input);

  static StatusOr<PerDeviceLiteralVecType> RunInternal(
      PjRtClient& client, PjRtLoadedExecutable* executable,
      std::function<
          StatusOr<std::vector<std::vector<std::unique_ptr<PjRtBuffer>>>>(bool)>
          create_argument_buffers_on_device,
      const RunningOptions& running_options);

  static StatusOr<PerDeviceLiteralVecType> FetchAndLogOutput(
      PjRtClient& client,
      const std::vector<std::vector<std::unique_ptr<PjRtBuffer>>>&
          output_buffers,
      ModuleOutputMode module_output_mode, bool log_output);

  static ReplicasAndPartitions GetReplicasAndPartitionsInternal(
      const std::optional<ExecutionOptions>& execution_options,
      int device_count, const std::optional<int>& num_replicas,
      const std::optional<int>& num_partitions, int num_slices = 1);
};

bool AbslParseFlag(absl::string_view text,
                   FunctionalHloRunner::ModuleArgumentMode* argument_mode,
                   std::string* error);
std::string AbslUnparseFlag(
    FunctionalHloRunner::ModuleArgumentMode argument_mode);

}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_TOOLS_MULTIHOST_HLO_RUNNER_FUNCTIONAL_HLO_RUNNER_H_
