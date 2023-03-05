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

#include "tools/mlir_interpreter/dialects/comparators.h"
#include "tools/mlir_interpreter/dialects/cwise_math.h"
#include "tools/mlir_interpreter/framework/interpreter.h"
#include "tools/mlir_interpreter/framework/interpreter_value.h"
#include "tools/mlir_interpreter/framework/interpreter_value_util.h"
#include "tools/mlir_interpreter/framework/registration.h"

namespace mlir {
namespace interpreter {
namespace {

struct CopySign : CwiseFloat {
  template <typename T>
  static T apply(T a, T b) {
    return std::copysign(a, b);
  }
};

REGISTER_MLIR_INTERPRETER_OP("math.copysign", applyCwiseBinaryMap<CopySign>);

REGISTER_MLIR_INTERPRETER_OP("math.absf", applyCwiseMap<Abs>);
REGISTER_MLIR_INTERPRETER_OP("math.cos", applyCwiseMap<Cos>);
REGISTER_MLIR_INTERPRETER_OP("math.exp", applyCwiseMap<Exp>);
REGISTER_MLIR_INTERPRETER_OP("math.floor", applyCwiseMap<Floor>);
REGISTER_MLIR_INTERPRETER_OP("math.log", applyCwiseMap<Log>);
REGISTER_MLIR_INTERPRETER_OP("math.log1p", applyCwiseMap<Log1P>);
REGISTER_MLIR_INTERPRETER_OP("math.powf", applyCwiseBinaryMap<Power>);
REGISTER_MLIR_INTERPRETER_OP("math.sin", applyCwiseMap<Sin>);

}  // namespace
}  // namespace interpreter
}  // namespace mlir
