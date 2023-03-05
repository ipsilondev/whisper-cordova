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

#ifndef TENSORFLOW_C_EXPERIMENTAL_NEXT_PLUGGABLE_DEVICE_C_API_H_
#define TENSORFLOW_C_EXPERIMENTAL_NEXT_PLUGGABLE_DEVICE_C_API_H_

#include "tensorflow/c/c_api.h"
#include "tensorflow/c/kernels.h"
#include "tensorflow/c/kernels_experimental.h"
#include "tensorflow/c/tf_buffer.h"
#include "tensorflow/compiler/xla/pjrt/c/pjrt_c_api.h"

// --------------------------------------------------------------------------
// C API for device. The API is under active development and eventually
// should allow registering a plugin device with TensorFlow.

// Macro to control visibility of exported symbols in the shared library (.so,
// .dylib, .dll).
// This duplicates the TF_EXPORT macro definition in
// tensorflow/core/platform/macros.h in order to keep this .h file independent
// of any other includes.
#ifdef SWIG
#define TF_CAPI_EXPORT
#else
#if defined(_WIN32)
#ifdef TF_COMPILE_LIBRARY
#define TF_CAPI_EXPORT __declspec(dllexport)
#else
#define TF_CAPI_EXPORT __declspec(dllimport)
#endif  // TF_COMPILE_LIBRARY
#else
#define TF_CAPI_EXPORT __attribute__((visibility("default")))
#endif  // _WIN32
#endif  // SWIG

#ifdef __cplusplus
extern "C" {
#endif

// TF_Device is a C wrapper to the C++ TF Device class. This is to be passed
// through TF_OpKernelContext, and is opaque to plugin.
typedef struct TF_Device TF_Device;

typedef struct TF_VariableInfo TF_VariableInfo;

// Returns a `TF_Device` pointer, which actually points to a C++ `Device`.
// Currently we only allow `NextPluggableDevice` to be casted as `TF_Device`,
// but in theory every this is a C API for every kind of device.
TF_CAPI_EXPORT extern TF_Device* TF_GetDevice(TF_OpKernelContext* ctx);

// --------------------------  Resource  ---------------------------------------
// Create a `tensorflow::PluginResource` to the ResourceMgr provided by the
// `ctx`. The `tensorflow::PluginResource` wraps a resource by plugin (as a
// opaque pointer, since TensorFlow cannot parse it). `delete_func` is needed
// for ResourceMgr to clean up the resource. `status` will be set.
TF_CAPI_EXPORT extern void TF_CreatePluginResource(
    TF_OpKernelContext* ctx, const char* container_name,
    const char* plugin_resource_name, void* plugin_resource,
    void (*delete_func)(void*), TF_Status* status);

// If the ResourceMgr provided by the `ctx` has a resource
// `plugin_resource_name`, returns it in `*result_plugin_resource`. Otherwise,
// invokes create_func to create the resource. `delete_func` is needed for
// ResourceMgr to clean up the resource. `status` will be set. If `status` is
// not OK, `*result_plugin_resource` will be set as nullptr.
//
// Caller does not take ownership of the `plugin_resource`.
TF_CAPI_EXPORT extern void TF_LookupOrCreatePluginResource(
    TF_OpKernelContext* ctx, const char* container_name,
    const char* plugin_resource_name, void** result_plugin_resource,
    void* (*create_func)(void*), void* create_func_args,
    void (*delete_func)(void*), TF_Status* status);

// -------------------------  VariableInfo  ------------------------------------
TF_CAPI_EXPORT extern TF_VariableInfo* TF_CreateVariableInfoFromContext(
    TF_OpKernelContext* ctx, int index, TF_Status* status);

TF_CAPI_EXPORT extern void TF_LockVariableInfos(TF_VariableInfo** vars,
                                                int num_vars,
                                                TF_Status* status);

TF_CAPI_EXPORT extern void TF_AllocateTempForVariableInfo(
    TF_OpKernelContext* ctx, TF_VariableInfo* var_info, TF_Status* status);

TF_CAPI_EXPORT extern TF_Tensor* TF_GetTensorFromVariableInfo(
    TF_VariableInfo* var_info, TF_Status* status);

TF_CAPI_EXPORT extern void TF_DeleteVariableInfo(TF_VariableInfo* var_info);

// ---------------------  Coordination service  --------------------------------
// Returns a not owning pointer to the coordination service agent, which is
// opaque to plugin. Plugin OpKernels need to use the accompanying C APIs to
// access coordination service functionalities.
TF_CAPI_EXPORT extern TF_CoordinationServiceAgent*
TF_GetCoordinationServiceAgent(TF_OpKernelContext* ctx);

// Returns true if the coordination service agent has been initialized.
TF_CAPI_EXPORT extern bool TF_CoordinationServiceIsInitialized(
    TF_CoordinationServiceAgent* agent);

TF_CAPI_EXPORT extern void TF_CoordinationServiceInsertKeyValue(
    const char* key, const char* value, TF_CoordinationServiceAgent* agent,
    TF_Status* status);

// Obtains key-value from coorination service agent. The returned `TF_Buffer`
// is a newly allocated buffer to hold the string key-value, and caller is
// responsible for managing the lifetime. If error, `status` will be set and a
// nullptr will be returned.
TF_CAPI_EXPORT extern TF_Buffer* TF_CoordinationServiceGetKeyValue(
    const char* key, TF_CoordinationServiceAgent* agent, TF_Status* status);

TF_CAPI_EXPORT extern void TF_CoordinationServiceDeleteKeyValue(
    const char* key, TF_CoordinationServiceAgent* agent, TF_Status* status);

// ----------------------------  PJRT  -----------------------------------------
TF_CAPI_EXPORT extern void TF_CreateAndSetPjRtCApiClient(
    const char* device_type, TF_Status* status);

// Gets the `PJRT_Client*` stored in TF global ResourceManager.
TF_CAPI_EXPORT extern PJRT_Client* TF_GetPjRtCClient(const char* device_type,
                                                     TF_Status* status);

// Gets the `PJRT_Buffer*` stored in the tensor. The status will contain error
// if the tensor does not have a `PjRtCApiBuffer`.
TF_CAPI_EXPORT extern PJRT_Buffer* TF_GetPjRtCBuffer(TF_Tensor* c_tensor,
                                                     TF_Status* status);

// Creates a `PjRtCApiBuffer` with the `PJRT_Buffer*` passed in and set to the
// tensor.
TF_CAPI_EXPORT extern void TF_CreatePjRtBuffer(TF_Tensor* c_tensor,
                                               PJRT_Buffer* c_buffer,
                                               const char* device_type,
                                               TF_Status* status);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // TENSORFLOW_C_EXPERIMENTAL_NEXT_PLUGGABLE_DEVICE_C_API_H_
