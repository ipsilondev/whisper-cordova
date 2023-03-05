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

#include "tensorflow/compiler/xla/stream_executor/cuda/cuda_blas_lt.h"

#include <algorithm>
#include <climits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "third_party/gpus/cuda/include/cublasLt.h"
#include "third_party/gpus/cuda/include/cublas_v2.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/stream_executor/blas.h"
#include "tensorflow/compiler/xla/stream_executor/cuda/cuda_blas.h"
#include "tensorflow/compiler/xla/stream_executor/cuda/cuda_blas_utils.h"
#include "tensorflow/compiler/xla/stream_executor/gpu/gpu_activation.h"
#include "tensorflow/compiler/xla/stream_executor/gpu/gpu_helpers.h"
#include "tensorflow/compiler/xla/stream_executor/gpu/gpu_stream.h"
#include "tensorflow/compiler/xla/stream_executor/gpu/gpu_timer.h"
#include "tensorflow/compiler/xla/stream_executor/scratch_allocator.h"
#include "tensorflow/compiler/xla/stream_executor/stream.h"

#define SET_ATTR(setter, handle, attr, value) \
  ToStatus(setter(handle, attr, &value, sizeof(decltype(value))), #setter)

#define GET_ATTR(getter, handle, attr, ValueT)                            \
  [&]() -> tsl::StatusOr<ValueT> {                                        \
    ValueT value;                                                         \
    TF_RETURN_IF_ERROR(ToStatus(                                          \
        getter(handle, attr, &value, sizeof(ValueT), nullptr), #getter)); \
    return std::move(value);                                              \
  }()

namespace stream_executor {
namespace cuda {
namespace {

template <typename T>
tsl::Status SetAttr(cublasLtMatrixLayout_t handle,
                    cublasLtMatrixLayoutAttribute_t attr, T value) {
  return SET_ATTR(cublasLtMatrixLayoutSetAttribute, handle, attr, value);
}

template <typename T>
tsl::StatusOr<T> GetAttr(cublasLtMatrixLayout_t handle,
                         cublasLtMatrixLayoutAttribute_t attr) {
  return GET_ATTR(cublasLtMatrixLayoutGetAttribute, handle, attr, T);
}

template <typename T>
tsl::Status SetAttr(cublasLtMatmulDesc_t handle,
                    cublasLtMatmulDescAttributes_t attr, T value) {
  return SET_ATTR(cublasLtMatmulDescSetAttribute, handle, attr, value);
}

template <typename T>
tsl::StatusOr<T> GetAttr(cublasLtMatmulDesc_t handle,
                         cublasLtMatmulDescAttributes_t attr) {
  return GET_ATTR(cublasLtMatmulDescGetAttribute, handle, attr, T);
}

template <typename T>
tsl::Status SetAttr(cublasLtMatmulPreference_t handle,
                    cublasLtMatmulPreferenceAttributes_t attr, T value) {
  return SET_ATTR(cublasLtMatmulPreferenceSetAttribute, handle, attr, value);
}

cublasLtPointerMode_t AsCublasLtPointerMode(BlasLt::PointerMode pointer_mode) {
  switch (pointer_mode) {
    case BlasLt::PointerMode::kHost:
      return CUBLASLT_POINTER_MODE_HOST;
    case BlasLt::PointerMode::kDevice:
      return CUBLASLT_POINTER_MODE_DEVICE;
  }
}

tsl::StatusOr<cublasLtEpilogue_t> AsCublasLtEpilogue(
    BlasLt::Epilogue epilogue) {
  switch (epilogue) {
    case BlasLt::Epilogue::kDefault:
      return CUBLASLT_EPILOGUE_DEFAULT;
    case BlasLt::Epilogue::kReLU:
      return CUBLASLT_EPILOGUE_RELU;
    case BlasLt::Epilogue::kBias:
      return CUBLASLT_EPILOGUE_BIAS;
    case BlasLt::Epilogue::kBiasThenReLU:
      return CUBLASLT_EPILOGUE_RELU_BIAS;
#if CUDA_VERSION >= 11040
    case BlasLt::Epilogue::kGELU:
      return CUBLASLT_EPILOGUE_GELU;
    case BlasLt::Epilogue::kGELUWithAux:
      return CUBLASLT_EPILOGUE_GELU_AUX;
    case BlasLt::Epilogue::kBiasThenGELU:
      return CUBLASLT_EPILOGUE_GELU_BIAS;
    case BlasLt::Epilogue::kBiasThenGELUWithAux:
      return CUBLASLT_EPILOGUE_GELU_AUX_BIAS;
#else
    case BlasLt::Epilogue::kGELU:
    case BlasLt::Epilogue::kGELUWithAux:
    case BlasLt::Epilogue::kBiasThenGELU:
    case BlasLt::Epilogue::kBiasThenGELUWithAux:
      return tsl::errors::Internal("GELU epilogues require cublasLt >= 11.4");
#endif
  }
}

}  // namespace

tsl::Status BlasLt::Init() {
  cublasLtHandle_t blas_lt;
  SE_CUBLAS_RETURN_IF_ERROR(cublasLtCreate(&blas_lt));
  absl::MutexLock lock(&mu_);
  blas_lt_.reset(blas_lt);
  return tsl::OkStatus();
}

/*static*/ tsl::StatusOr<BlasLt::MatrixLayout> BlasLt::MatrixLayout::Create(
    blas::DataType type, size_t num_rows, size_t num_cols,
    BlasLt::MatrixLayout::Order order, size_t batch_size,
    std::optional<int64_t> leading_dim_stride,
    std::optional<int64_t> batch_stride) {
  if (!leading_dim_stride) {
    leading_dim_stride = (order == Order::kRowMajor) ? num_cols : num_rows;
  }

  cublasLtMatrixLayout_t cu_layout;
  SE_CUBLAS_RETURN_IF_ERROR(
      cublasLtMatrixLayoutCreate(&cu_layout, AsCudaDataType(type), num_rows,
                                 num_cols, *leading_dim_stride));
  // Wrap cublas handle immediately, so it is cleaned up if an error occurs.
  BlasLt::MatrixLayout layout(cu_layout);
  TF_RETURN_IF_ERROR(
      SetAttr(cu_layout, CUBLASLT_MATRIX_LAYOUT_ORDER,
              int32_t{(order == Order::kRowMajor) ? CUBLASLT_ORDER_ROW
                                                  : CUBLASLT_ORDER_COL}));
  TF_RETURN_IF_ERROR(SetAttr(cu_layout, CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT,
                             static_cast<int32_t>(batch_size)));

  if (!batch_stride) {
    batch_stride = (batch_size > 1) ? num_rows * num_cols : 0;
  }

  TF_RETURN_IF_ERROR(SetAttr(
      cu_layout, CUBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET, *batch_stride));
  return std::move(layout);
}

cudaDataType_t BlasLt::MatrixLayout::type() const {
  return static_cast<cudaDataType_t>(
      GetAttr<uint32_t>(handle_.get(), CUBLASLT_MATRIX_LAYOUT_TYPE).value());
}

/*static*/ tsl::StatusOr<BlasLt::MatmulDesc> BlasLt::MatmulDesc::Create(
    blas::ComputationType compute_type, blas::DataType scale_type,
    blas::Transpose trans_a, blas::Transpose trans_b, BlasLt::Epilogue epilogue,
    BlasLt::PointerMode pointer_mode) {
  cublasLtMatmulDesc_t cu_desc;
  SE_CUBLAS_RETURN_IF_ERROR(cublasLtMatmulDescCreate(
      &cu_desc, AsCublasComputeType(compute_type), AsCudaDataType(scale_type)));
  // Wrap cublas handle immediately, so it is cleaned up if an error occurs.
  BlasLt::MatmulDesc desc(cu_desc);
  TF_RETURN_IF_ERROR(SetAttr(cu_desc, CUBLASLT_MATMUL_DESC_POINTER_MODE,
                             AsCublasLtPointerMode(pointer_mode)));
  TF_RETURN_IF_ERROR(SetAttr(cu_desc, CUBLASLT_MATMUL_DESC_TRANSA,
                             AsCublasOperation(trans_a)));
  TF_RETURN_IF_ERROR(SetAttr(cu_desc, CUBLASLT_MATMUL_DESC_TRANSB,
                             AsCublasOperation(trans_b)));
  TF_ASSIGN_OR_RETURN(cublasLtEpilogue_t epi, AsCublasLtEpilogue(epilogue));
  TF_RETURN_IF_ERROR(SetAttr(cu_desc, CUBLASLT_MATMUL_DESC_EPILOGUE, epi));
  return std::move(desc);
}

cublasComputeType_t BlasLt::MatmulDesc::compute_type() const {
  return static_cast<cublasComputeType_t>(
      GetAttr<int32_t>(handle_.get(), CUBLASLT_MATMUL_DESC_COMPUTE_TYPE)
          .value());
}

cudaDataType_t BlasLt::MatmulDesc::scale_type() const {
  return static_cast<cudaDataType_t>(
      GetAttr<int32_t>(handle_.get(), CUBLASLT_MATMUL_DESC_SCALE_TYPE).value());
}

cublasLtPointerMode_t BlasLt::MatmulDesc::pointer_mode() const {
  return static_cast<cublasLtPointerMode_t>(
      GetAttr<int32_t>(handle_.get(), CUBLASLT_MATMUL_DESC_POINTER_MODE)
          .value());
}

/*static*/ tsl::StatusOr<BlasLt::MatmulPreference>
BlasLt::MatmulPreference::Create(size_t max_workspace_size) {
  cublasLtMatmulPreference_t cu_preference;
  SE_CUBLAS_RETURN_IF_ERROR(cublasLtMatmulPreferenceCreate(&cu_preference));
  // Wrap cublas handle immediately, so it is cleaned up if an error occurs.
  BlasLt::MatmulPreference preference(cu_preference);
  TF_RETURN_IF_ERROR(SetAttr<uint64_t>(cu_preference,
                                       CUBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES,
                                       max_workspace_size));
  return std::move(preference);
}

tsl::StatusOr<std::vector<BlasLt::MatmulAlgorithm>> BlasLt::GetMatmulAlgorithms(
    const BlasLt::MatmulPlan& plan, const BlasLt::MatmulPreference& preference,
    size_t max_algorithm_count) {
  max_algorithm_count = std::min(max_algorithm_count, size_t{INT_MAX});
  std::vector<cublasLtMatmulHeuristicResult_t> results(max_algorithm_count);
  {
    absl::MutexLock lock(&mu_);
    TF_RET_CHECK(blas_lt_ != nullptr);

    gpu::ScopedActivateExecutorContext sac{parent_};

    int found_algorithm_count = 0;
    SE_CUBLAS_RETURN_IF_ERROR(cublasLtMatmulAlgoGetHeuristic(
        blas_lt_.get(), plan.op_desc.get(), plan.a_desc.get(),
        plan.b_desc.get(), plan.c_desc.get(), plan.d_desc.get(),
        preference.get(), max_algorithm_count, results.data(),
        &found_algorithm_count));
    results.resize(found_algorithm_count);
  }

  std::vector<BlasLt::MatmulAlgorithm> algorithms;
  algorithms.reserve(results.size());
  for (const cublasLtMatmulHeuristicResult_t& result : results) {
    if (result.state == CUBLAS_STATUS_SUCCESS) {  // Skip failed algos.
      algorithms.push_back({result.algo, result.workspaceSize});
    }
  }
  return std::move(algorithms);
}

tsl::Status BlasLt::DoMatmul(Stream* stream, const BlasLt::MatmulPlan& plan,
                             const void* alpha, DeviceMemoryBase a,
                             DeviceMemoryBase b, const void* beta,
                             DeviceMemoryBase c, DeviceMemoryBase d,
                             const BlasLt::MatmulAlgorithm& algorithm,
                             ScratchAllocator& scratch_allocator,
                             DeviceMemoryBase bias, DeviceMemoryBase aux,
                             DeviceMemoryBase a_scale, DeviceMemoryBase b_scale,
                             DeviceMemoryBase c_scale, DeviceMemoryBase d_scale,
                             DeviceMemoryBase d_amax,
                             blas::ProfileResult* profile_result) {
  std::unique_ptr<gpu::GpuTimer, gpu::GpuTimerDeleter> timer;
  if (profile_result != nullptr) {
    timer.reset(new gpu::GpuTimer(parent_));
    TF_RET_CHECK(timer->Init());
    TF_RET_CHECK(timer->Start(gpu::AsGpuStream(stream)));
  }

  void* workspace = nullptr;
  if (algorithm.workspace_size > 0) {
    TF_ASSIGN_OR_RETURN(
        DeviceMemory<uint8_t> alloc,
        scratch_allocator.AllocateBytes(algorithm.workspace_size));
    workspace = gpu::GpuMemoryMutable(&alloc);
  }

  {
    absl::MutexLock lock(&mu_);
    TF_RET_CHECK(blas_lt_ != nullptr);
    // We must set the bias and aux pointers while holding the mutex, to avoid a
    // potential race condition from multiple threads sharing the same plan.
    if (bias != nullptr) {
      TF_RETURN_IF_ERROR(SetAttr(plan.op_desc.get(),
                                 CUBLASLT_MATMUL_DESC_BIAS_POINTER,
                                 bias.opaque()));
    }
#if CUDA_VERSION >= 11080
    if (a_scale != nullptr) {
      TF_RETURN_IF_ERROR(SetAttr(plan.op_desc.get(),
                                 CUBLASLT_MATMUL_DESC_A_SCALE_POINTER,
                                 a_scale.opaque()));
    }
    if (b_scale != nullptr) {
      TF_RETURN_IF_ERROR(SetAttr(plan.op_desc.get(),
                                 CUBLASLT_MATMUL_DESC_B_SCALE_POINTER,
                                 b_scale.opaque()));
    }
    if (c_scale != nullptr) {
      TF_RETURN_IF_ERROR(SetAttr(plan.op_desc.get(),
                                 CUBLASLT_MATMUL_DESC_C_SCALE_POINTER,
                                 c_scale.opaque()));
    }
    if (d_scale != nullptr) {
      TF_RETURN_IF_ERROR(SetAttr(plan.op_desc.get(),
                                 CUBLASLT_MATMUL_DESC_D_SCALE_POINTER,
                                 d_scale.opaque()));
    }
    if (d_amax != nullptr) {
      TF_RETURN_IF_ERROR(SetAttr(plan.op_desc.get(),
                                 CUBLASLT_MATMUL_DESC_AMAX_D_POINTER,
                                 d_amax.opaque()));
    }
#else
    if (a_scale != nullptr || b_scale != nullptr || c_scale != nullptr ||
        d_scale != nullptr || d_amax != nullptr) {
      return tsl::errors::Internal(
          "A/B/C/D scales and amax require cublasLt >= 11.8");
    }
#endif

    if (aux != nullptr) {
#if CUDA_VERSION >= 11040
      TF_RETURN_IF_ERROR(SetAttr(plan.op_desc.get(),
                                 CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_POINTER,
                                 aux.opaque()));

      // Set leading dim and batch stride of auxiliary output to match output.
      // TODO(cjfj): Set this once at initialization.
      TF_ASSIGN_OR_RETURN(
          int64_t output_leading_dim,
          GetAttr<int64_t>(plan.d_desc.get(), CUBLASLT_MATRIX_LAYOUT_LD));

      TF_RETURN_IF_ERROR(SetAttr(plan.op_desc.get(),
                                 CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_LD,
                                 output_leading_dim));

      TF_ASSIGN_OR_RETURN(
          int64_t output_batch_stride,
          GetAttr<int64_t>(plan.d_desc.get(),
                           CUBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET));

      TF_RETURN_IF_ERROR(SetAttr(plan.op_desc.get(),
                                 CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_BATCH_STRIDE,
                                 output_batch_stride));
#else
      return tsl::errors::Internal(
          "Auxiliary inputs / outputs require cublasLt >= 11.4");
#endif
    }

    gpu::ScopedActivateExecutorContext sac{parent_};

    SE_CUBLAS_RETURN_IF_ERROR(cublasLtMatmul(
        blas_lt_.get(), plan.op_desc.get(), alpha, a.opaque(),
        plan.a_desc.get(), b.opaque(), plan.b_desc.get(), beta, c.opaque(),
        plan.c_desc.get(), d.opaque(), plan.d_desc.get(), &algorithm.algo,
        workspace, algorithm.workspace_size, gpu::AsGpuStreamValue(stream)));
  }

  if (timer) {
    TF_RET_CHECK(timer->Stop(gpu::AsGpuStream(stream)));
    profile_result->set_is_valid(true);
    profile_result->set_elapsed_time_in_ms(timer->GetElapsedMilliseconds());
  }
  return tsl::OkStatus();
}

BlasLt* GetBlasLt(Stream* stream) {
  CUDABlas* blas = dynamic_cast<CUDABlas*>(stream->parent()->AsBlas());
  return (blas != nullptr) ? &blas->blas_lt() : nullptr;
}

}  // namespace cuda
}  // namespace stream_executor
