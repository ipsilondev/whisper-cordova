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

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/base/dynamic_annotations.h"
#include "tensorflow/compiler/xla/mlir/runtime/transforms/compilation_pipeline_options.h"
#include "tensorflow/compiler/xla/mlir/runtime/transforms/tests/testlib_pipeline.h"
#include "tensorflow/compiler/xla/mlir/runtime/utils/async_runtime_api.h"
#include "tensorflow/compiler/xla/runtime/arguments.h"
#include "tensorflow/compiler/xla/runtime/async_runtime.h"
#include "tensorflow/compiler/xla/runtime/custom_call_registry.h"
#include "tensorflow/compiler/xla/runtime/jit_executable.h"
#include "tensorflow/compiler/xla/runtime/logical_result.h"
#include "tensorflow/compiler/xla/runtime/results.h"
#include "tensorflow/compiler/xla/runtime/types.h"
#include "tensorflow/tsl/platform/test.h"
#include "tensorflow/tsl/platform/test_benchmark.h"

namespace xla {
namespace runtime {

using absl::StatusOr;

//===----------------------------------------------------------------------===//
// A helper function that compiles the given `module` to an XLA runtime
// executable and runs the module's `test` function with the given arguments.
// Results are returned to the caller via the user-provided result converter.
//===----------------------------------------------------------------------===//

static AsyncTaskRunner* NoRunner() {
  return reinterpret_cast<AsyncTaskRunner*>(0XDEADBEEF);
}

// Lazily execute tasks
class LazyAsyncTaskRunner : public AsyncTaskRunner {
 public:
  void Schedule(Task task) final { tasks_.push_back(std::move(task)); }
  void Run() {
    while (!tasks_.empty()) {
      tasks_.back()();
      tasks_.pop_back();
      break;
    }
  }

 private:
  std::vector<Task> tasks_;
};

struct CustomCallRegistry {
  std::function<void(DynamicCustomCallRegistry&)> dynamic_custom_calls;
  std::function<void(DirectCustomCallRegistry&)> direct_custom_calls;
};

static absl::StatusOr<JitExecutable> Compile(
    std::string_view module, absl::Span<const std::string_view> exported,
    const CustomCallRegistry& registry = {}) {
  JitExecutable::Options opts;
  CompilationPipelineOptions copts;
  opts.specialization = JitExecutable::Specialization::kDisabled;
  opts.compiler.symbols_binding = ToSymbolsBinding(
      registry.direct_custom_calls, copts.populate_type_id_names);
  opts.compiler.register_dialects = [&](DialectRegistry& dialects) {
    RegisterXlaRuntimeTestlibDialects(dialects);
  };
  opts.compiler.create_compilation_pipeline = CreateXlaRuntimeTestlibPipeline;

  return JitExecutable::Instantiate(module, opts, exported);
}

static absl::StatusOr<ExecutionReference> Execute(
    JitExecutable& jit_executable, unsigned ordinal, ArgumentsRef args,
    ResultConverter& results, AsyncTaskRunner* async_task_runner = NoRunner(),
    const CustomCallRegistry& registry = {}, bool use_lazy_runner = false) {
  AsyncValuePtr<Executable> executable = jit_executable.DefaultExecutable();
  if (executable.IsError()) return executable.GetError();

  // Register all dynamic custom calls.
  DynamicCustomCallRegistry dynamic_custom_calls;
  if (registry.dynamic_custom_calls)
    registry.dynamic_custom_calls(dynamic_custom_calls);

  CustomCall::UserData user_data;
  // Always add a pointer to `self` to user data.
  user_data.insert(&executable.get());

  Executable::ExecuteOpts execute_opts;
  execute_opts.custom_call_registry = &dynamic_custom_calls;
  execute_opts.custom_call_data = &user_data;
  execute_opts.async_task_runner = async_task_runner;
  if (use_lazy_runner) {
    LazyAsyncTaskRunner runner;
    execute_opts.async_task_runner = &runner;
    FunctionRef function_ref = executable->function_ref(ordinal);
    auto status = function_ref(args, results, execute_opts);
    runner.Run();
    return status;
  }

  FunctionRef function_ref = executable->function_ref(ordinal);
  return function_ref(args, results, execute_opts);
}

static absl::StatusOr<ExecutionReference> CompileAndExecute(
    std::string_view module, ArgumentsRef args, ResultConverter& results,
    AsyncTaskRunner* async_task_runner = NoRunner(),
    const CustomCallRegistry& registry = {}, bool use_lazy_runner = false) {
  StatusOr<JitExecutable> jit_executable = Compile(module, {"test"}, registry);
  if (!jit_executable.ok()) return jit_executable.status();

  return Execute(*jit_executable, 0, args, results, async_task_runner, registry,
                 use_lazy_runner);
}

//===----------------------------------------------------------------------===//

namespace {

// An owning wrapper around Memref desciptor that releases the underlying buffer
// when destructed. Used for testing passing ownerhip of memrefs allocated in
// the compiled executables to the C++ caller.
struct OwnedMemref {
  ~OwnedMemref() {
    if (desc.has_value()) std::free(desc->data());
  }

  MemrefDesc* operator->() { return &desc.value(); }

  std::optional<MemrefDesc> desc;
};

}  // namespace

//===----------------------------------------------------------------------===//

static void AssertNoError(const absl::Status& status) {
  assert(false && "Unexpected call to `ReturnError`");
}

static void IgnoreError(const absl::Status& status) {}

void Emplace(void* int_ptr, AsyncValue* dst) {
  auto& v = dst->get<int32_t>();
  v = *reinterpret_cast<int32_t*>(int_ptr);
}

struct ReturnI32 {
  LogicalResult operator()(unsigned result_index, const Type* type,
                           const Type* runtime_type, void* ret) const {
    auto* scalar = llvm::dyn_cast<ScalarType>(type);
    if (scalar && scalar->type() == PrimitiveType::S32) {
      ABSL_ANNOTATE_MEMORY_IS_INITIALIZED(ret, sizeof(int32_t));
      *ptr = *reinterpret_cast<int32_t*>(ret);
      return success();
    }
    return failure();
  }

  int32_t* ptr = nullptr;
};

struct ReturnMemref {
  LogicalResult operator()(unsigned result_index, const Type* type,
                           const Type* runtime_type, void* ret) const {
    auto* memref = llvm::dyn_cast<MemrefType>(runtime_type);
    if (!memref) return failure();

    auto desc = ConvertReturnedMemref<MemrefDesc>(*this, memref, ret);
    if (failed(desc)) return failure();

    ptr->desc = std::move(*desc);
    return success();
  }

  MemrefDesc operator()(PrimitiveType element_type, void* base_ptr,
                        void* data_ptr, int64_t offset,
                        absl::Span<const int64_t> sizes,
                        absl::Span<const int64_t> strides) const {
    return MemrefDesc(element_type, base_ptr, offset, sizes, strides);
  }

  OwnedMemref* ptr = nullptr;
};

struct ReturnAsyncToken {
  LogicalResult operator()(unsigned result_index, const Type* type,
                           const Type* runtime_type, void* result_ptr) const {
    if (!llvm::isa<AsyncTokenType>(type)) return failure();

    // Load the pointer to the async token from a pointer to result storage.
    ABSL_ANNOTATE_MEMORY_IS_INITIALIZED(result_ptr, sizeof(void*));
    void* ret = *reinterpret_cast<void**>(result_ptr);
    auto* token = static_cast<AsyncRuntime::Token*>(ret);
    auto* async_value = AsyncRuntime::GetAsyncValue(token);
    CHECK(async_value->IsAvailable());
    chain.SetStateConcrete();
    AsyncRuntime::DropRef(AsyncRuntime::ToAsyncRuntimeObject(token));
    return success();
  }

  AsyncValuePtr<Chain> chain;
};

struct ReturnAsyncI32 {
  LogicalResult operator()(unsigned result_index, const Type* type,
                           const Type* runtime_type, void* result_ptr) const {
    auto* value_type = llvm::dyn_cast<AsyncValueType>(type);
    if (!value_type) return failure();

    // Load the pointer to the async value from a pointer to result storage.
    ABSL_ANNOTATE_MEMORY_IS_INITIALIZED(result_ptr, sizeof(void*));
    void* ret = *reinterpret_cast<void**>(result_ptr);
    auto* value = static_cast<AsyncRuntime::Value*>(ret);
    auto* scalar = llvm::dyn_cast<ScalarType>(&value_type->value_type());

    if (scalar && scalar->type() == PrimitiveType::S32) {
      ExtractAsyncValue(value, ptr.value(), Emplace);
      return success();
    }

    return failure();
  }

  AsyncValuePtr<int32_t> ptr;
};

struct ReturnAsyncMemref {
  LogicalResult operator()(unsigned result_index, const Type* type,
                           const Type* runtime_type, void* result_ptr) const {
    auto* value_type = llvm::dyn_cast<AsyncValueType>(type);
    if (!value_type) return failure();

    // Load the pointer to the async memref from a pointer to result storage.
    ABSL_ANNOTATE_MEMORY_IS_INITIALIZED(result_ptr, sizeof(void*));
    void* ret = *reinterpret_cast<void**>(result_ptr);
    auto* value = static_cast<AsyncRuntime::Value*>(ret);
    auto* memref = llvm::dyn_cast<MemrefType>(&value_type->value_type());

    if (memref) {
      // TODO(ezhulenev): Emplace function captures `memref` by reference, and
      // if `value` is not available, then it will lead to asan errors. We need
      // an `ExtractAsyncValue` that can take absl::AnyInvocable callback, that
      // will capture all referenced values. Alternative solution is a large
      // switch statement that will dispatch for different types and ranks.
      ExtractAsyncValue(value, ptr.value(), [&](void* data, AsyncValue* dst) {
        auto desc = ConvertReturnedMemref<MemrefDesc>(*this, memref, data);
        if (succeeded(desc)) dst->get<OwnedMemref>().desc = std::move(*desc);
      });
      return success();
    }

    return failure();
  }

  MemrefDesc operator()(PrimitiveType element_type, void* base_ptr,
                        void* data_ptr, int64_t offset,
                        absl::Span<const int64_t> sizes,
                        absl::Span<const int64_t> strides) const {
    return MemrefDesc(element_type, base_ptr, offset, sizes, strides);
  }

  AsyncValuePtr<OwnedMemref> ptr;
};

// Execute all tasks in the caller thread immediately.
class InlineAsyncTaskRunner : public AsyncTaskRunner {
 public:
  void Schedule(Task task) final { (task(), num_executed_++); }
  size_t num_executed() const { return num_executed_; }

 private:
  size_t num_executed_ = 0;
};

//===----------------------------------------------------------------------===//

TEST(ExecutableTest, ReturnScalar) {
  absl::string_view module = R"(
    func.func @test() -> i32 {
      %0 = arith.constant 42 : i32
      return %0 : i32
    }
  )";

  int32_t result = 0;
  ResultConverterSet converter(AssertNoError, ReturnI32{&result});

  ASSERT_TRUE(CompileAndExecute(module, {}, converter).ok());
  EXPECT_EQ(result, 42);
}

TEST(ExecutableTest, ReturnMemref) {
  absl::string_view module = R"(
    func.func @test() -> memref<?x?xf32> {
      %0 = arith.constant 1 : index
      %1 = arith.constant 2 : index
      %2 = memref.alloc(%0, %1) : memref<?x?xf32>
      return %2 : memref<?x?xf32>
    }
  )";

  OwnedMemref result;
  ResultConverterSet converter(AssertNoError, ReturnMemref{&result});

  ASSERT_TRUE(CompileAndExecute(module, {}, converter).ok());
  ASSERT_TRUE(result.desc.has_value());
  EXPECT_EQ(result->rank(), 2);
  EXPECT_EQ(result->size(0), 1);
  EXPECT_EQ(result->size(1), 2);
}

TEST(ExecutableTest, ScalarArgs) {
  absl::string_view module = R"(
    func.func @test(%arg0: i32, %arg1: i32) -> i32 {
      %0 = arith.addi %arg0, %arg1 : i32
      return %0 : i32
    }
  )";

  int32_t result = 0;
  ResultConverterSet converter(AssertNoError, ReturnI32{&result});

  ScalarArg arg0(static_cast<int32_t>(20));
  ScalarArg arg1(static_cast<int32_t>(22));

  ASSERT_TRUE(CompileAndExecute(module, {arg0, arg1}, converter).ok());
  EXPECT_EQ(result, 42);
}

TEST(ExecutableTest, MultipleFunctions) {
  absl::string_view module = R"(
    func.func @add(%arg0: i32, %arg1: i32) -> i32 {
      %0 = arith.addi %arg0, %arg1 : i32
      return %0 : i32
    }

    func.func @mul(%arg0: i32, %arg1: i32) -> i32 {
      %0 = arith.muli %arg0, %arg1 : i32
      return %0 : i32
    }
  )";

  absl::StatusOr<JitExecutable> compiled = Compile(module, {"add", "mul"});
  ASSERT_TRUE(compiled.ok());
  EXPECT_EQ(compiled->num_functions(), 2);

  int32_t result = 0;
  ResultConverterSet converter(AssertNoError, ReturnI32{&result});

  ScalarArg arg0(static_cast<int32_t>(20));
  ScalarArg arg1(static_cast<int32_t>(22));

  ASSERT_TRUE(Execute(*compiled, /*ordinal=*/0, {arg0, arg1}, converter).ok());
  EXPECT_EQ(result, 20 + 22);

  ASSERT_TRUE(Execute(*compiled, /*ordinal=*/1, {arg0, arg1}, converter).ok());
  EXPECT_EQ(result, 20 * 22);
}

TEST(ExecutableTest, AssertionFailure) {
  absl::string_view module = R"(
    func.func @test(%arg0: i32) {
      %c42 = arith.constant 42 : i32
      %0 = arith.cmpi ne, %c42, %arg0 : i32
      cf.assert %0, "Oops, argument can't be 42"
      return
    }
  )";

  NoResultConverter converter;

  {
    ScalarArg arg0(int32_t{20});
    EXPECT_TRUE(CompileAndExecute(module, {arg0}, converter).ok());
  }

  {
    ScalarArg arg0(int32_t{42});
    auto executed = CompileAndExecute(module, {arg0}, converter);
    EXPECT_FALSE(executed.ok());
    EXPECT_EQ(executed.status().message(),
              "run time error: Oops, argument can't be 42");
  }
}

TEST(ExecutableTest, AssertionFailureOrResult) {
  absl::string_view module = R"(
    func.func @test(%arg0: i32) -> i32 {
      %c42 = arith.constant 42 : i32
      %0 = arith.cmpi ne, %c42, %arg0 : i32
      cf.assert %0, "Oops, argument can't be 42"
      %1 = arith.addi %arg0, %c42 : i32
      return %1 : i32
    }
  )";

  {
    int32_t result = 0;
    ResultConverterSet converter(AssertNoError, ReturnI32{&result});

    ScalarArg arg0(int32_t{20});
    EXPECT_TRUE(CompileAndExecute(module, {arg0}, converter).ok());
    EXPECT_EQ(result, 62);
  }

  {
    int32_t result = 0;
    ResultConverterSet converter(IgnoreError, ReturnI32{&result});

    ScalarArg arg0(int32_t{42});
    auto executed = CompileAndExecute(module, {arg0}, converter);
    EXPECT_FALSE(executed.ok());
    EXPECT_EQ(executed.status().message(),
              "run time error: Oops, argument can't be 42");
    EXPECT_EQ(result, 0);
  }
}

TEST(ExecutableTest, AsyncExecuteAndAwait) {
  absl::string_view module = R"(
    func.func @test(%arg0: i32, %arg1: i32) -> i32 {
      %token, %result = async.execute -> !async.value<i32> {
        %0 = arith.addi %arg0, %arg1 : i32
        async.yield %0 : i32
      }
      %1 = async.await %result : !async.value<i32>
      return %1 : i32
    }
  )";

  int32_t result = 0;
  ResultConverterSet converter(AssertNoError, ReturnI32{&result});

  ScalarArg arg0(static_cast<int32_t>(20));
  ScalarArg arg1(static_cast<int32_t>(22));

  InlineAsyncTaskRunner runner;

  ASSERT_TRUE(CompileAndExecute(module, {arg0, arg1}, converter, &runner).ok());
  EXPECT_EQ(runner.num_executed(), 1);
  EXPECT_EQ(result, 42);
}

TEST(ExecutableTest, AsyncTokenRet) {
  absl::string_view module = R"(
    async.func @test() -> !async.token {
      return
    }
  )";

  AsyncValueRef<Chain> result = MakeConstructedAsyncValueRef<Chain>();
  ResultConverterSet converter(AssertNoError, ReturnAsyncToken{result.AsPtr()});

  ASSERT_TRUE(CompileAndExecute(module, {}, converter).ok());
  EXPECT_EQ(result.IsAvailable(), true);
}

TEST(ExecutableTest, AsyncScalarRet) {
  absl::string_view module = R"(
    async.func @test(%arg0: i32, %arg1: i32) -> !async.value<i32> {
      %0 = arith.addi %arg0, %arg1 : i32
      return %0 : i32
    }
  )";

  AsyncValueRef<int32_t> result = MakeConstructedAsyncValueRef<int32_t>();
  ResultConverterSet converter(AssertNoError, ReturnAsyncI32{result.AsPtr()});

  ScalarArg arg0(static_cast<int32_t>(20));
  ScalarArg arg1(static_cast<int32_t>(22));

  ASSERT_TRUE(CompileAndExecute(module, {arg0, arg1}, converter).ok());
  EXPECT_EQ(result.get(), 42);
}

TEST(ExecutableTest, AsyncMemrefRet) {
  absl::string_view module = R"(
    async.func @test(%arg0: index) -> !async.value<memref<?xf32>> {
      %c0 = arith.constant 0 : index
      %c1 = arith.constant 1 : index

      %0 = memref.alloc(%arg0) : memref<?xf32>
      scf.for %i = %c0 to %arg0 step %c1 {
        %c42 = arith.constant 42.0 : f32
        memref.store %c42, %0[%i] : memref<?xf32>
      }

      return %0 : memref<?xf32>
    }
  )";

  AsyncValueRef<OwnedMemref> result =
      MakeConstructedAsyncValueRef<OwnedMemref>();
  ResultConverterSet converter(AssertNoError,
                               ReturnAsyncMemref{result.AsPtr()});

  ScalarArg arg0(static_cast<int64_t>(32));

  ASSERT_TRUE(CompileAndExecute(module, {arg0}, converter).ok());
  ASSERT_TRUE(result.get().desc.has_value());
  EXPECT_EQ(result.get()->rank(), 1);
  EXPECT_EQ(result.get()->size(0), 32);

  float* data = reinterpret_cast<float*>(result.get()->data());
  EXPECT_TRUE(std::all_of(data, data + 32, [](float v) { return v == 42.0f; }));
}

TEST(ExecutableTest, AsyncWaiting) {
  absl::string_view module = R"(
    async.func @test2(%arg0: i32, %arg1: i32) -> !async.value<i32> {
      %0 = arith.addi %arg0, %arg1 : i32
      return %0 : i32
    }
    async.func @test(%arg0: i32, %arg1:i32) -> !async.value<i32> {
      %0 = async.call @test2(%arg0, %arg1) : (i32, i32) -> !async.value<i32>
      %1 = async.await %0 : !async.value<i32>
      return %1 : i32
    }
  )";

  AsyncValueRef<int32_t> result = MakeConstructedAsyncValueRef<int32_t>();
  ResultConverterSet converter(AssertNoError, ReturnAsyncI32{result.AsPtr()});

  ScalarArg arg0(static_cast<int32_t>(20));
  ScalarArg arg1(static_cast<int32_t>(22));

  ASSERT_TRUE(CompileAndExecute(module, {arg0, arg1}, converter).ok());
  EXPECT_EQ(result.get(), 42);
}

TEST(ExecutableTest, AsyncCustomCall) {
  absl::string_view source = R"(
    func.func private @custom_call_return() -> !async.value<i32>
      attributes { rt.dynamic, rt.custom_call = "test.custom_call_return" }

    func.func private @custom_call(%arg32 : i32)
      attributes { rt.dynamic, rt.custom_call = "test.custom_call" }

    async.func @test() -> !async.token {
      %0 = func.call @custom_call_return() : () -> !async.value<i32>
      %1 = async.await %0 : !async.value<i32>
      func.call @custom_call(%1) : (i32) -> ()
      return
    }
  )";

  auto f_result = []() -> absl::StatusOr<AsyncValueRef<int32_t>> {
    return tsl::MakeAvailableAsyncValueRef<int32_t>(42);
  };

  int32_t i32 = 0;
  auto f = [&](int32_t arg) {
    i32 = arg;
    return success();
  };

  CustomCallRegistry registry = {[&](DynamicCustomCallRegistry& registry) {
    registry.Register(CustomCall::Bind("test.custom_call_return")
                          .Ret<AsyncValueRef<int32_t>>()
                          .To(f_result));

    registry.Register(
        CustomCall::Bind("test.custom_call").Arg<int32_t>().To(f));
  }};

  AsyncValueRef<Chain> result = MakeConstructedAsyncValueRef<Chain>();
  ResultConverterSet converter(AssertNoError, ReturnAsyncToken{result.AsPtr()});

  ASSERT_TRUE(
      CompileAndExecute(source, /*args=*/{}, converter, NoRunner(), registry)
          .ok());
  EXPECT_EQ(i32, 42);
}

TEST(ExecutableTest, AsyncExecute) {
  absl::string_view source = R"(
    module {
    func.func private @custom_call_return() -> !async.value<i32>
      attributes { rt.dynamic, rt.custom_call = "test.custom_call_return" }

    async.func @test() -> !async.value<i32> {
      %token, %result = async.execute -> !async.value<i32> {
        %0 = func.call @custom_call_return() : () -> !async.value<i32>
        %1 = async.await %0 : !async.value<i32>
        async.yield %1 : i32
      }
      %1 = async.await %result : !async.value<i32>
      return %1 : i32
    }
    }
  )";

  LazyAsyncTaskRunner runner;

  auto async_result = tsl::MakeAvailableAsyncValueRef<int32_t>(42);
  auto f_result = [&]() -> absl::StatusOr<AsyncValueRef<int32_t>> {
    return async_result;
  };

  CustomCallRegistry registry = {[&](DynamicCustomCallRegistry& registry) {
    registry.Register(CustomCall::Bind("test.custom_call_return")
                          .Ret<AsyncValueRef<int32_t>>()
                          .To(f_result));
  }};
  AsyncValueRef<int32_t> result = MakeConstructedAsyncValueRef<int32_t>();
  ResultConverterSet converter(AssertNoError, ReturnAsyncI32{result.AsPtr()});

  ASSERT_TRUE(CompileAndExecute(source, /*args=*/{}, converter, &runner,
                                registry, /*use_lazy_runner=*/true)
                  .ok());

  EXPECT_EQ(result.get(), 42);
}

//===----------------------------------------------------------------------===//
// Performance benchmarks are below.
//===----------------------------------------------------------------------===//

static void CompileAndBenchmark(
    benchmark::State& state, std::string_view module, ArgumentsRef args,
    ResultConverter& results, AsyncTaskRunner* async_task_runner = NoRunner()) {
  JitExecutable::Options opts;
  opts.specialization = JitExecutable::Specialization::kDisabled;
  opts.compiler.register_dialects = RegisterXlaRuntimeTestlibDialects;
  opts.compiler.create_compilation_pipeline = CreateXlaRuntimeTestlibPipeline;

  StatusOr<JitExecutable> jit_executable =
      JitExecutable::Instantiate(module, "test", opts);
  CHECK(jit_executable.ok()) << jit_executable.status().message();

  AsyncValuePtr<Executable> executable = jit_executable->DefaultExecutable();
  CHECK(!executable.IsError()) << executable.GetError().message();

  Executable::CallFrame call_frame;
  auto initialized = executable->InitializeCallFrame(args, &call_frame);
  CHECK(initialized.ok()) << initialized.message();

  Executable::ExecuteOpts execute_opts;
  execute_opts.async_task_runner = async_task_runner;

  for (auto _ : state) {
    call_frame.args[0] = nullptr;  // reset execution context
    executable->Execute(call_frame, execute_opts);
    CHECK(!call_frame.is_error) << call_frame.error;
    absl::Status returned = executable->ReturnResults(results, &call_frame);
    CHECK(returned.ok()) << returned.message();
  }
}

void BM_AsyncExecuteAndAwait(benchmark::State& state) {
  absl::string_view module = R"(
    func.func @test(%arg0: i32, %arg1: i32) -> i32 {
      %token, %result = async.execute -> !async.value<i32> {
        %0 = arith.addi %arg0, %arg1 : i32
        async.yield %0 : i32
      }
      %1 = async.await %result : !async.value<i32>
      return %1 : i32
    }
  )";

  int32_t result = 0;
  ResultConverterSet converter(AssertNoError, ReturnI32{&result});

  ScalarArg arg0(static_cast<int32_t>(20));
  ScalarArg arg1(static_cast<int32_t>(22));

  InlineAsyncTaskRunner runner;
  CompileAndBenchmark(state, module, {arg0, arg1}, converter, &runner);
}

void BM_AsyncFunc(benchmark::State& state) {
  absl::string_view module = R"(
    async.func @test(%arg0: i32, %arg1: i32) -> !async.value<i32> {
      %0 = arith.addi %arg0, %arg1 : i32
      return %0 : i32
    }
  )";

  AsyncValueRef<int32_t> result = MakeConstructedAsyncValueRef<int32_t>();
  ResultConverterSet converter(AssertNoError, ReturnAsyncI32{result.AsPtr()});

  ScalarArg arg0(static_cast<int32_t>(20));
  ScalarArg arg1(static_cast<int32_t>(22));

  InlineAsyncTaskRunner runner;
  CompileAndBenchmark(state, module, {arg0, arg1}, converter, &runner);
}

void BM_AsyncFuncCall(benchmark::State& state) {
  absl::string_view module = R"(
    async.func @test2(%arg0: i32, %arg1: i32) -> !async.value<i32> {
      %0 = arith.addi %arg0, %arg1 : i32
      return %0 : i32
    }
    async.func @test(%arg0: i32, %arg1:i32) -> !async.value<i32> {
      %0 = async.call @test2(%arg0, %arg1) : (i32, i32) -> !async.value<i32>
      %1 = async.await %0 : !async.value<i32>
      return %1 : i32
    }
  )";

  AsyncValueRef<int32_t> result = MakeConstructedAsyncValueRef<int32_t>();
  ResultConverterSet converter(AssertNoError, ReturnAsyncI32{result.AsPtr()});

  ScalarArg arg0(static_cast<int32_t>(20));
  ScalarArg arg1(static_cast<int32_t>(22));

  InlineAsyncTaskRunner runner;
  CompileAndBenchmark(state, module, {arg0, arg1}, converter, &runner);
}

BENCHMARK(BM_AsyncExecuteAndAwait);
BENCHMARK(BM_AsyncFunc);
BENCHMARK(BM_AsyncFuncCall);

}  // namespace runtime
}  // namespace xla
