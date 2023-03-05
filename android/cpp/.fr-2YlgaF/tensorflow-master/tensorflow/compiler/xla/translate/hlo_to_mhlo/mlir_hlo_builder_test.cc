/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/translate/hlo_to_mhlo/mlir_hlo_builder.h"

#include <string>

#include "llvm/Support/raw_ostream.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/BuiltinAttributes.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/Dialect.h"  // from @llvm-project
#include "mlir/IR/Location.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "tensorflow/compiler/xla/client/xla_computation.h"
#include "tensorflow/compiler/xla/hlo/ir/hlo_module.h"
#include "tensorflow/compiler/xla/mlir_hlo/mhlo/IR/hlo_ops.h"
#include "tensorflow/compiler/xla/service/llvm_ir/llvm_util.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/translate/hlo_to_mhlo/hlo_function_importer.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/tsl/lib/core/status_test_util.h"
#include "tensorflow/tsl/platform/status.h"

namespace xla {

namespace {

static void ExpectHasSubstr(absl::string_view s, absl::string_view expected) {
  EXPECT_TRUE(absl::StrContains(s, expected))
      << s << " does not contain " << expected;
}

class XlaBuilderTest : public ::testing::Test {
 protected:
  XlaBuilderTest()
      : name_(SetupTest()),
        module_(mlir::ModuleOp::create(mlir::UnknownLoc::get(&context_))),
        builder_(&module_->getBodyRegion()),
        xla_builder_(name_, builder_, module_->getLoc(),
                     /*build_functions=*/true) {
    context_.loadDialect<mlir::mhlo::MhloDialect>();
  }

  std::string SetupTest() {
    return ::testing::UnitTest::GetInstance()->current_test_info()->name();
  }

  // Returns the MLIR op string representation of the given XlaOp.
  std::string GetMlirOpString(XlaOp xla_op) {
    return llvm_ir::DumpToString(xla_builder_.GetValue(xla_op));
  }

  std::string GetMlirOpString(mlir::Operation* op) {
    return llvm_ir::DumpToString(op);
  }

  Status ValidateCustomOpCallee(XlaOp op) {
    mlir::Value call_result = xla_builder_.GetValue(op);
    if (!call_result) return InternalError("No MLIR op for the given XlaOp");

    auto call_op = llvm::dyn_cast_or_null<mlir::mhlo::CustomCallOp>(
        call_result.getDefiningOp());
    if (!call_op || call_op.getCalledComputations().size() != 1) {
      return InternalError("Given XlaOp doesn't point to a CustomCallOp");
    }

    if (call_op.getCalledComputations().size() != 1) {
      return InternalError(
          "CustomCallOp should have exactly one called computation");
    }

    auto callee_name =
        call_op.getCalledComputations()[0].dyn_cast<mlir::FlatSymbolRefAttr>();
    if (!callee_name) {
      return InternalError(
          "CustomCallOp called computation isn't a flat symbol ref");
    }

    auto func_op = module_->lookupSymbol<mlir::func::FuncOp>(callee_name);
    if (!func_op) {
      return InternalError(
          "No function found corresponding to the called computations "
          "attribute");
    }

    return tsl::OkStatus();
  }

  XlaComputation BuildTestComparator() {
    // Create a test comparator computation to use as the custom computation.
    auto cmp_builder = xla_builder_.CreateSubBuilder("test_comparator");
    auto p0 = Parameter(cmp_builder.get(), 0,
                        xla::ShapeUtil::MakeScalarShape(xla::F32), "p0");
    auto p1 = Parameter(cmp_builder.get(), 1,
                        xla::ShapeUtil::MakeScalarShape(xla::F32), "p1");
    Gt(p0, p1);
    return cmp_builder->BuildAndNoteError();
  }

  std::string name_;
  mlir::MLIRContext context_;
  mlir::OwningOpRef<mlir::ModuleOp> module_;
  mlir::OpBuilder builder_;
  MlirHloBuilder xla_builder_;
};

TEST_F(XlaBuilderTest, CreateToken) {
  auto token = CreateToken(&xla_builder_);
  auto str = GetMlirOpString(token);

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());

  ExpectHasSubstr(GetMlirOpString(token), R"(mhlo.create_token : !mhlo.token)");
}

TEST_F(XlaBuilderTest, Infeed) {
  auto token = CreateToken(&xla_builder_);
  auto infeed = InfeedWithToken(token, ShapeUtil::MakeShape(F32, {4, 8}), "");

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());
  ExpectHasSubstr(
      GetMlirOpString(infeed),
      R"(mhlo.tuple %1#0, %1#1 : tuple<tensor<4x8xf32>, !mhlo.token>)");
}

TEST_F(XlaBuilderTest, Outfeed) {
  auto outfeed_shape = ShapeUtil::MakeShape(F32, {4, 8});
  auto data = ConstantLiteral(
      &xla_builder_,
      LiteralUtil::CreateFromDimensions(F32, outfeed_shape.dimensions()));
  auto token = CreateToken(&xla_builder_);
  auto outfeed = OutfeedWithToken(data, token, outfeed_shape, "");

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());
  ExpectHasSubstr(
      GetMlirOpString(outfeed),
      R"("mhlo.outfeed"(%0, %1) {outfeed_config = ""} : (tensor<4x8xf32>, !mhlo.token) -> !mhlo.token)");
}

TEST_F(XlaBuilderTest, ConcatInDim) {
  auto data0 = ConstantLiteral(
      &xla_builder_, LiteralUtil::CreateFromDimensions(F32, {2, 4, 5}));
  auto data1 = ConstantLiteral(
      &xla_builder_, LiteralUtil::CreateFromDimensions(F32, {2, 6, 5}));
  auto concat = ConcatInDim(&xla_builder_, {data0, data1}, 1);

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());
  ExpectHasSubstr(
      GetMlirOpString(concat),
      R"("mhlo.concatenate"(%0, %1) {dimension = 1 : i64} : (tensor<2x4x5xf32>, tensor<2x6x5xf32>) -> tensor<2x10x5xf32>)");
}

TEST_F(XlaBuilderTest, Tuple) {
  auto data0 = ConstantLiteral(&xla_builder_,
                               LiteralUtil::CreateFromDimensions(F32, {3, 7}));
  auto data1 = ConstantLiteral(&xla_builder_,
                               LiteralUtil::CreateFromDimensions(F32, {}));
  auto tuple = Tuple(&xla_builder_, {data0, data1});

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());
  ExpectHasSubstr(GetMlirOpString(tuple),
                  R"(mhlo.tuple %0, %1 : tuple<tensor<3x7xf32>, tensor<f32>>)");
}

TEST_F(XlaBuilderTest, GetTupleElement) {
  auto data0 = ConstantLiteral(&xla_builder_,
                               LiteralUtil::CreateFromDimensions(F32, {3, 7}));
  auto data1 = ConstantLiteral(&xla_builder_,
                               LiteralUtil::CreateFromDimensions(F32, {}));
  auto tuple_data = Tuple(&xla_builder_, {data0, data1});
  auto gte = GetTupleElement(tuple_data, 1);

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());
  ExpectHasSubstr(
      GetMlirOpString(gte),
      R"(mhlo.get_tuple_element %2[1] : (tuple<tensor<3x7xf32>, tensor<f32>>) -> tensor<f32>)");
}

TEST_F(XlaBuilderTest, Slice) {
  auto data = ConstantLiteral(&xla_builder_,
                              LiteralUtil::CreateFromDimensions(F32, {3, 7}));
  auto slice = Slice(data, {0, 1}, {2, 5}, {1, 1});

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());
  ExpectHasSubstr(
      GetMlirOpString(slice),
      R"("mhlo.slice"(%0) {limit_indices = dense<[2, 5]> : tensor<2xi64>, start_indices = dense<[0, 1]> : tensor<2xi64>, strides = dense<1> : tensor<2xi64>} : (tensor<3x7xf32>) -> tensor<2x4xf32>)");
}

TEST_F(XlaBuilderTest, Pad) {
  auto data = ConstantLiteral(&xla_builder_,
                              LiteralUtil::CreateFromDimensions(F32, {3, 7}));
  auto zero = ConstantLiteral(&xla_builder_, LiteralUtil::Zero(F32));

  PaddingConfig padding_config;
  auto* dims0 = padding_config.add_dimensions();
  dims0->set_edge_padding_low(1);
  dims0->set_interior_padding(0);
  dims0->set_edge_padding_high(2);
  auto* dims1 = padding_config.add_dimensions();
  dims1->set_edge_padding_low(3);
  dims1->set_interior_padding(1);
  dims1->set_edge_padding_high(0);
  auto pad = Pad(data, zero, padding_config);

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());
  ExpectHasSubstr(
      GetMlirOpString(pad),
      R"("mhlo.pad"(%0, %1) {edge_padding_high = dense<[2, 0]> : tensor<2xi64>, edge_padding_low = dense<[1, 3]> : tensor<2xi64>, interior_padding = dense<[0, 1]> : tensor<2xi64>} : (tensor<3x7xf32>, tensor<f32>) -> tensor<6x16xf32>)");
}

TEST_F(XlaBuilderTest, CustomCallWithComputation) {
  XlaComputation test_comparator = BuildTestComparator();
  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());

  // Finally, add the CustomCallOp (with computation) to the module.
  Shape shape(PrimitiveType::PRED, /*dimensions=*/{}, /*dynamic_dimensions=*/{},
              /*tuple_shapes=*/{});
  auto custom_call = CustomCallWithComputation(
      &xla_builder_, "test_call_target", {}, test_comparator, shape,
      "{\"option1\": foo, \"option2\": bar, \"option3\": \"baz\"}");

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());

  ExpectHasSubstr(
      GetMlirOpString(custom_call),
      R"(%0 = mhlo.custom_call @test_call_target() {backend_config = "{\22option1\22: foo, \22option2\22: bar, \22option3\22: \22baz\22}", called_computations = [@test_comparator.4]} : () -> tensor<i1>)");

  // We should also expect there to be a new function added for the comparator.
  auto actual_func_op = module_->lookupSymbol<mlir::func::FuncOp>(
      test_comparator.proto().computations().at(0).name());
  EXPECT_TRUE(actual_func_op);
  EXPECT_EQ(
      GetMlirOpString(actual_func_op),
      R"(func.func private @test_comparator.4(%arg0: tensor<f32>, %arg1: tensor<f32>) -> tensor<i1> {
  %0 = mhlo.compare  GT, %arg0, %arg1 : (tensor<f32>, tensor<f32>) -> tensor<i1>
  return %0 : tensor<i1>
})");
}

// Tests that the same comparator can be used in different custom call ops with
// appropriate rename.
TEST_F(XlaBuilderTest, DuplicateCustomCallComparator) {
  XlaComputation test_comparator = BuildTestComparator();
  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());

  Shape shape(PrimitiveType::PRED, /*dimensions=*/{}, /*dynamic_dimensions=*/{},
              /*tuple_shapes=*/{});
  {
    auto custom_call = CustomCallWithComputation(
        &xla_builder_, "test_call_target", {}, test_comparator, shape);

    TF_ASSERT_OK(xla_builder_.GetCurrentStatus());
    TF_ASSERT_OK(ValidateCustomOpCallee(custom_call))
        << GetMlirOpString(*module_);
  }

  {
    auto custom_call = CustomCallWithComputation(
        &xla_builder_, "test_call_target", {}, test_comparator, shape);
    TF_ASSERT_OK(xla_builder_.GetCurrentStatus());

    TF_ASSERT_OK(ValidateCustomOpCallee(custom_call))
        << GetMlirOpString(*module_);
  }

  // Verify that there are no duplicated symbols by creating a SymbolTable.
  mlir::SymbolTable symbol_table(*module_);
  (void)symbol_table;
}

TEST_F(XlaBuilderTest, CustomCallWithFrontendAttributes) {
  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());

  // Create frontend attributes and set it for the CustomCall op.
  FrontendAttributes attr;
  attr.mutable_map()->insert({"test_name", "test_value"});

  xla_builder_.SetFrontendAttributes(attr);

  // Add the CustomCallOp to the module.
  Shape shape(PrimitiveType::PRED, /*dimensions=*/{}, /*dynamic_dimensions=*/{},
              /*tuple_shapes=*/{});
  auto custom_call = CustomCall(&xla_builder_, "test_call_target", {}, shape);

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());

  // Verify that the frontend attributes are correctly set for the CustomCall
  // op.
  ExpectHasSubstr(
      GetMlirOpString(custom_call),
      R"(%0 = mhlo.custom_call @test_call_target() {backend_config = "", mhlo.frontend_attributes = {test_name = "test_value"}} : () -> tensor<i1>)");
}

TEST_F(XlaBuilderTest, CustomCallWithLiteral) {
  auto input = ConstantLiteral(&xla_builder_,
                               LiteralUtil::CreateFromDimensions(F32, {5, 7}));
  xla::Literal literal = xla::LiteralUtil::CreateR0<int32_t>(16);
  auto custom_call = CustomCall(&xla_builder_, "OpWithLiteral", {input},
                                xla_builder_.GetShape(input).value(),
                                /*opaque=*/"", /*has_side_effect=*/false,
                                /*output_operand_aliasing=*/{}, &literal);

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());

  ExpectHasSubstr(
      GetMlirOpString(custom_call),
      R"(mhlo.custom_call @OpWithLiteral(%0) {backend_config = "", mhlo.literal = dense<16> : tensor<i32>} : (tensor<5x7xf32>) -> tensor<5x7xf32>)");
}

TEST_F(XlaBuilderTest, InfeedWithTokenWithFrontendAttributes) {
  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());

  // Create frontend attributes and set it for the CustomCall op.
  FrontendAttributes attr;
  attr.mutable_map()->insert({"test_name", "test_value"});

  xla_builder_.SetFrontendAttributes(attr);

  auto token = CreateToken(&xla_builder_);
  InfeedWithToken(token, ShapeUtil::MakeShape(F32, {4, 8}), "");

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());

  // Verify that the frontend attributes are correctly set for the entire
  // module.
  ExpectHasSubstr(
      GetMlirOpString(module_.get()),
      R"(%0 = mhlo.create_token {mhlo.frontend_attributes = {test_name = "test_value"}} : !mhlo.token
  %1:2 = "mhlo.infeed"(%0) {infeed_config = "", mhlo.frontend_attributes = {test_name = "test_value"}} : (!mhlo.token) -> (tensor<4x8xf32>, !mhlo.token)
  %2 = mhlo.tuple %1#0, %1#1 {mhlo.frontend_attributes = {test_name = "test_value"}} : tuple<tensor<4x8xf32>, !mhlo.token>)");
}

}  // namespace
}  // namespace xla
