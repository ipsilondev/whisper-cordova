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

#include <string>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "mhlo/IR/hlo_ops.h"
#include "mhlo/transforms/map_stablehlo_to_hlo_op.h"
#include "mhlo/transforms/rewriters.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Types.h"
#include "mlir/Support/DebugStringHelper.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/DialectConversion.h"
#include "stablehlo/dialect/StablehloOps.h"

namespace mlir {
namespace stablehlo {
namespace {

// PRIVATE MHLO features are internal to XLA and not used by any ML frontends.
// These should never be converted to StableHLO, as they are not a good fit for
// StableHLO.
template <typename HloOpTy>
bool hasPrivateFeaturesNotInStablehlo(HloOpTy hloOp) {
  // To the best of our knowledge, none of the ML frontends are using these ops
  // directly or indirectly, so we categorized them as private to XLA.
  // Please let us know if we missed something, and we'll recategorize them.
  if (isa<mhlo::AddDependencyOp, mhlo::AsyncDoneOp, mhlo::AsyncStartOp,
          mhlo::AsyncUpdateOp, mhlo::BitcastOp, mhlo::CopyOp, mhlo::DomainOp,
          mhlo::FusionOp, mhlo::StochasticConvertOp,
          mhlo::XlaRngGetAndUpdateStateOp>(hloOp.getOperation())) {
    return true;
  }
  if constexpr (std::is_same<HloOpTy, mhlo::ConvolutionOp>::value) {
    // StableHLO convolution doesn't support "unknown" dimensions.
    // This is an esoteric feature of MHLO convolutions, and it's different
    // from the notion of dynamic dimensions. For more context, here's the
    // commit which introduced it:
    // https://github.com/tensorflow/mlir-hlo/commit/4d6dc3163c1c9289d86455d9f4de5711465c50fb
    // This feature isn't supported in HLO and doesn't have documentation, so
    // we may end up removing it from MHLO as well.
    auto dimensionNumbers = debugString(hloOp.getDimensionNumbers());
    if (dimensionNumbers.find('?') != std::string::npos) return true;
  }
  if constexpr (std::is_same<HloOpTy, mhlo::CustomCallOp>::value) {
    // To the best of our knowledge, none of the ML frontends are using this
    // enum, so we categorized it as private to XLA.
    // Please let us know if we missed something, and we'll recategorize it.
    if (hloOp.getCustomCallSchedule() != mhlo::CustomCallSchedule::NONE)
      return true;
  }
  return false;
}

bool hasPackedNibble(Optional<ArrayAttr> precisionConfigAttr) {
  if (!precisionConfigAttr) return false;
  return llvm::any_of(*precisionConfigAttr, [&](Attribute attr) {
    auto precisionAttr = attr.cast<mhlo::PrecisionAttr>();
    return precisionAttr.getValue() == mhlo::Precision::PACKED_NIBBLE;
  });
}

// EXPERIMENTAL MHLO features are being explored by ML frontends but do not have
// any agreed upon compatibility guarantees. By default, these features cannot
// be converted to StableHLO, although the allow-experimental-features flag can
// be used to manually enable the conversion. Such features might be a good fit
// for StableHLO, and they are usually accompanied by a StableHLO GitHub ticket.
template <typename HloOpTy>
bool hasExperimentalFeaturesNotInStablehlo(HloOpTy hloOp) {
  if constexpr (std::is_same<HloOpTy, mhlo::AllToAllOp>::value) {
    // StableHLO AllToAll doesn't support the tuple form yet.
    // Proposal: https://github.com/openxla/stablehlo/issues/574.
    if (hloOp.getNumOperands() != 1) return true;
  }
  if constexpr (std::is_same<HloOpTy, mhlo::ConvolutionOp>::value) {
    // StableHLO ConvolutionOp doesn't support PACKED_NIBBLE yet.
    // Proposal: https://github.com/openxla/stablehlo/issues/742.
    if (hasPackedNibble(hloOp.getPrecisionConfig())) return true;
  }
  if constexpr (std::is_same<HloOpTy, mhlo::CustomCallOp>::value) {
    // StableHLO CustomCall doesn't support API_VERSION_TYPED_FFI yet.
    // Proposal: https://github.com/openxla/stablehlo/issues/637.
    if (hloOp.getApiVersion() ==
        mhlo::CustomCallApiVersion::API_VERSION_TYPED_FFI)
      return true;
  }
  if constexpr (std::is_same<HloOpTy, mhlo::DotGeneralOp>::value) {
    // StableHLO DotGeneral doesn't support PACKED_NIBBLE yet.
    // Proposal: https://github.com/openxla/stablehlo/issues/742.
    if (hasPackedNibble(hloOp.getPrecisionConfig())) return true;
  }
  if constexpr (std::is_same<HloOpTy, mhlo::DotOp>::value) {
    // StableHLO Dot doesn't support PACKED_NIBBLE yet.
    // Proposal: https://github.com/openxla/stablehlo/issues/742.
    if (hasPackedNibble(hloOp.getPrecisionConfig())) return true;
  }
  return false;
}

// PUBLIC MHLO features are not yet in StableHLO but are agreed upon internally
// to have limited compatibility guarantees. These features are used by ML
// frontends but are not yet part of StableHLO. Such features might be a good
// fit for StableHLO, and are usually accompanied by a StableHLO GitHub ticket.
template <typename HloOpTy>
bool hasPublicFeaturesNotInStablehlo(HloOpTy) {
  return false;
}

#define RETURN_CONVERTED_ENUM_ATTR(Name)                      \
  auto hloValue = mhlo::stringify##Name(attr.getValue());     \
  auto stablehloValue = stablehlo::symbolize##Name(hloValue); \
  if (!stablehloValue.has_value()) return {};                 \
  return stablehlo::Name##Attr::get(attr.getContext(), stablehloValue.value())

Attribute convertAttr(Attribute hloAttr) {
  // Handle MHLO attributes.
  // The logic that handles attributes from other dialects (e.g. builtin
  // attributes) lives below.
  if (auto attr = hloAttr.dyn_cast<mhlo::ChannelHandleAttr>()) {
    return stablehlo::ChannelHandleAttr::get(attr.getContext(),
                                             attr.getHandle(), attr.getType());
  }
  if (auto attr = hloAttr.dyn_cast<mhlo::ComparisonDirectionAttr>()) {
    RETURN_CONVERTED_ENUM_ATTR(ComparisonDirection);
  }
  if (auto attr = hloAttr.dyn_cast<mhlo::ComparisonTypeAttr>()) {
    RETURN_CONVERTED_ENUM_ATTR(ComparisonType);
  }
  if (auto attr = hloAttr.dyn_cast<mhlo::ConvDimensionNumbersAttr>()) {
    return stablehlo::ConvDimensionNumbersAttr::get(
        attr.getContext(), attr.getInputBatchDimension(),
        attr.getInputFeatureDimension(), attr.getInputSpatialDimensions(),
        attr.getKernelInputFeatureDimension(),
        attr.getKernelOutputFeatureDimension(),
        attr.getKernelSpatialDimensions(), attr.getOutputBatchDimension(),
        attr.getOutputFeatureDimension(), attr.getOutputSpatialDimensions());
  }
  // NOTE: We cannot process CustomCallApiVersionAttr here because
  // `dyn_cast<mhlo::CustomCallApiVersionAttr>()` succeeds for IntegerAttr too.
  if (auto attr = hloAttr.dyn_cast<mhlo::DotDimensionNumbersAttr>()) {
    return stablehlo::DotDimensionNumbersAttr::get(
        attr.getContext(), attr.getLhsBatchingDimensions(),
        attr.getRhsBatchingDimensions(), attr.getLhsContractingDimensions(),
        attr.getRhsContractingDimensions());
  }
  if (auto attr = hloAttr.dyn_cast<mhlo::FftTypeAttr>()) {
    RETURN_CONVERTED_ENUM_ATTR(FftType);
  }
  if (auto attr = hloAttr.dyn_cast<mhlo::GatherDimensionNumbersAttr>()) {
    return stablehlo::GatherDimensionNumbersAttr::get(
        attr.getContext(), attr.getOffsetDims(), attr.getCollapsedSliceDims(),
        attr.getStartIndexMap(), attr.getIndexVectorDim());
  }
  if (auto attr = hloAttr.dyn_cast<mhlo::OutputOperandAliasAttr>()) {
    return stablehlo::OutputOperandAliasAttr::get(
        attr.getContext(), attr.getOutputTupleIndices(), attr.getOperandIndex(),
        attr.getOperandTupleIndices());
  }
  if (auto attr = hloAttr.dyn_cast<mhlo::PrecisionAttr>()) {
    // StableHLO Precision doesn't support PACKED_NIBBLE yet.
    // Proposal: https://github.com/openxla/stablehlo/issues/742.
    if (attr.getValue() == mhlo::Precision::PACKED_NIBBLE) return {};
    RETURN_CONVERTED_ENUM_ATTR(Precision);
  }
  if (auto attr = hloAttr.dyn_cast<mhlo::RngAlgorithmAttr>()) {
    RETURN_CONVERTED_ENUM_ATTR(RngAlgorithm);
  }
  if (auto attr = hloAttr.dyn_cast<mhlo::RngDistributionAttr>()) {
    RETURN_CONVERTED_ENUM_ATTR(RngDistribution);
  }
  if (auto attr = hloAttr.dyn_cast<mhlo::ScatterDimensionNumbersAttr>()) {
    return stablehlo::ScatterDimensionNumbersAttr::get(
        attr.getContext(), attr.getUpdateWindowDims(),
        attr.getInsertedWindowDims(), attr.getScatterDimsToOperandDims(),
        attr.getIndexVectorDim());
  }
  if (auto attr = hloAttr.dyn_cast<mhlo::TransposeAttr>()) {
    RETURN_CONVERTED_ENUM_ATTR(Transpose);
  }
  if (hloAttr.getDialect().getNamespace() ==
      mhlo::MhloDialect::getDialectNamespace()) {
    // Our guiding principle is to support all StableHLO functionality in MHLO.
    // The inverse is not necessarily true - some MHLO attributes are missing
    // from StableHLO (either deliberately or haven't yet been proposed).
    // As a result, these MHLO attributes will fail here.
    return {};
  }

  // Handle non-MHLO attributes.
  // If an attribute is not defined in MHLO, then it is unchanged,
  // with the exception of ArrayAttr which is converted recursively.
  if (auto hloAttrs = hloAttr.dyn_cast<ArrayAttr>()) {
    SmallVector<Attribute> stablehloAttrs;
    for (auto hloAttr : hloAttrs) {
      auto stablehloAttr = convertAttr(hloAttr);
      if (!stablehloAttr) return {};
      stablehloAttrs.push_back(stablehloAttr);
    }
    return ArrayAttr::get(hloAttrs.getContext(), stablehloAttrs);
  }
  return hloAttr;
}

#undef RETURN_CONVERTED_ENUM_ATTR

// Convert array of enum attrs to an array of enum strings
//   [#mhlo<precision PACKED_NIBBLE>] -> ["PACKED_NIBBLE"]
//
// This is stable as long as enum names are not changed. This is needed to avoid
// a dependency on upstream printing / parsing. If an attribute name is changed,
// we can fork and  modify the code of `stringifyPrecision` as needed for
// compatibility.
Attribute encodePrecisionConfig(Attribute hloAttrs) {
  auto hloArrayAttr = hloAttrs.dyn_cast<ArrayAttr>();
  if (!hloArrayAttr) return {};
  SmallVector<Attribute> stablehloAttrs;
  for (auto hloAttr : hloArrayAttr) {
    auto precisionAttr = hloAttr.dyn_cast<mhlo::PrecisionAttr>();
    if (!precisionAttr) return {};
    StringRef precisionStr = mhlo::stringifyPrecision(precisionAttr.getValue());
    if (precisionStr.empty()) return {};
    stablehloAttrs.push_back(
        StringAttr::get(hloAttr.getContext(), precisionStr));
  }
  return ArrayAttr::get(hloAttrs.getContext(), stablehloAttrs);
}

// Experimental and public ops in MHLO that do not exist yet in StableHLO can be
// encoded as a StableHLO CustomCallOp to allow round-tripping between dialects.
//
// Example:
//   %0 = "mhlo.dot"(%arg0, %arg1) {
//     precision_config = [#mhlo<precision PACKED_NIBBLE>] } ...
//  ==>
//  %0 = stablehlo.custom_call @mhlo.dot {
//    mhlo.attributes = {precision_config = ["PACKED_NIBBLE"]}}
LogicalResult rewriteMhloOpAsCustomCall(Operation* hloOp,
                                        ConversionPatternRewriter& rewriter,
                                        TypeRange stablehloTypes,
                                        ValueRange stablehloOperands) {
  if (hloOp->getNumRegions() != 0) {
    // Extensibility protocol for regions hasn't been implemented yet.
    // In principle, it should be straightforward to implement by
    // converting regions into functions and calling them out in
    // "called_computations".
    // https://github.com/openxla/stablehlo/issues/593.
    return failure();
  }

  // Convert MHLO attributes to StableHLO equivalents.
  // If an attribute is not defined in MHLO, then it is unchanged,
  // with the exception of ArrayAttr which is converted recursively.
  SmallVector<NamedAttribute> stablehloConvertedAttrs;
  for (NamedAttribute hloAttr : hloOp->getAttrs()) {
    // Special case Attrs/Values not in StableHLO
    // precision_config exists in both MHLO and StableHLO, but MHLO's version
    // has additional enum values not supported in StableHLO.
    Attribute stablehloAttr;
    if (hloAttr.getName() == "precision_config") {
      stablehloAttr = encodePrecisionConfig(hloAttr.getValue());
    } else {
      stablehloAttr = convertAttr(hloAttr.getValue());
    }
    if (!stablehloAttr) return failure();
    stablehloConvertedAttrs.push_back({hloAttr.getName(), stablehloAttr});
  }

  auto stablehloCallTargetName = hloOp->getName().getStringRef();
  SmallVector<NamedAttribute> stablehloAttrs;
  stablehloAttrs.push_back(rewriter.getNamedAttr(
      "call_target_name", rewriter.getStringAttr(stablehloCallTargetName)));
  stablehloAttrs.push_back(rewriter.getNamedAttr(
      "mhlo.attributes", rewriter.getDictionaryAttr(stablehloConvertedAttrs)));
  rewriter.replaceOpWithNewOp<stablehlo::CustomCallOp>(
      hloOp, stablehloTypes, stablehloOperands, stablehloAttrs);
  return success();
}

template <typename HloOpTy>
class HloToStablehloOpConverter : public OpConversionPattern<HloOpTy> {
 public:
  HloToStablehloOpConverter(TypeConverter& converter, MLIRContext* context,
                            bool allowExperimentalFeatures)
      : OpConversionPattern<HloOpTy>::OpConversionPattern(converter, context),
        allowExperimentalFeatures(allowExperimentalFeatures) {}

  LogicalResult matchAndRewrite(
      HloOpTy hloOp, typename HloOpTy::Adaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    // Most MHLO ops which end up here are fully supported by StableHLO.
    // However, some of these ops are supported only partially because they
    // have features that are not supported in StableHLO.
    // These MHLO features fall into two distinct categories:
    //   1) Features that are private to the XLA compiler, so they are not
    //      a good fit for StableHLO. Conversion of such features should fail.
    //   2) Features that might be a good fit for StableHLO but haven't yet
    //      been proposed or approved in StableHLO. Conversion of such features
    //      should succeed using custom_call extensibility protocol (see below).
    if (hasPrivateFeaturesNotInStablehlo(hloOp)) {
      return failure();
    }

    // Convert MHLO types to StableHLO equivalents.
    // If a type is not defined in MHLO, then it is unchanged,
    // with the exception of RankedTensorType and TupleType which are
    // converted recursively.
    // See `HloToStablehloTypeConverter` for more information on when this
    // conversion will succeed or fail.
    SmallVector<Type> stablehloTypes;
    if (failed(this->getTypeConverter()->convertTypes(hloOp->getResultTypes(),
                                                      stablehloTypes)))
      return failure();

    // These operands have already been converted to StableHLO by
    // the dialect conversion infrastructure.
    ValueRange stablehloOperands = adaptor.getOperands();

    // Extensibility protocol for MHLO ops with public MHLO features that
    // are not yet supported in StableHLO.
    //   1) The op is represented by stablehlo::CustomCallOp.
    //   2) The full name, e.g. "mhlo.all_to_all" is stored in the
    //      `call_target_name` attribute of the CustomCallOp.
    //   3) The operands become operands of the CustomCallOp.
    //   4) The attributes are wrapped in a DictionaryAttr, which is
    //      prettyprinted and then stored in the `backend_config` attribute
    //      of the CustomCallOp.
    //   5) The result types become result types of the CustomCallOp.
    //
    // This StableHLO representation does not come with any compatibility
    // guarantees. For example, when it is roundtripped back to MHLO, it may
    // turn out that the original MHLO op no longer exists or has different
    // attributes in the current version.
    bool hasExperimentalFeatures = hasExperimentalFeaturesNotInStablehlo(hloOp);
    if (!allowExperimentalFeatures && hasExperimentalFeatures) return failure();
    if (hasPublicFeaturesNotInStablehlo(hloOp) || hasExperimentalFeatures) {
      return rewriteMhloOpAsCustomCall(hloOp, rewriter, stablehloTypes,
                                       stablehloOperands);
    }

    // Convert MHLO attributes to StableHLO equivalents.
    // If an attribute is not defined in MHLO, then it is unchanged,
    // with the exception of ArrayAttr which is converted recursively.
    SmallVector<NamedAttribute> stablehloAttrs;
    for (NamedAttribute hloAttr : hloOp->getAttrs()) {
      if constexpr (std::is_same<HloOpTy, mhlo::CustomCallOp>::value) {
        // custom_call_schedule is private to XLA, but we still want to allow
        // #mhlo<custom_call_schedule NONE> (by ignoring it).
        if (hloAttr.getName() == "custom_call_schedule" &&
            hloOp.getCustomCallSchedule() == mhlo::CustomCallSchedule::NONE)
          continue;
      }
      auto stablehloAttr = convertAttr(hloAttr.getValue());
      if (!stablehloAttr) return failure();
      stablehloAttrs.push_back({hloAttr.getName(), stablehloAttr});
    }

    // Convert the MHLO operation to a StableHLO equivalent.
    // This can almost be done in a generic fashion, except for stablehlo.case
    // that uses a variadic number of regions which means an additional argument
    // for the generic builder.
    HloToStablehloOp<HloOpTy> stablehloOp;
    if constexpr (std::is_same<HloOpTy, mhlo::CaseOp>::value) {
      stablehloOp = rewriter.replaceOpWithNewOp<stablehlo::CaseOp>(
          hloOp, stablehloTypes, stablehloOperands, stablehloAttrs,
          hloOp.getBranches().size());
    } else {
      stablehloOp = rewriter.replaceOpWithNewOp<HloToStablehloOp<HloOpTy>>(
          hloOp, stablehloTypes, stablehloOperands, stablehloAttrs);
    }

    // Finally, populate the regions while converting argument types
    // and nested operations.
    for (auto [hloRegion, stablehloRegion] :
         llvm::zip(hloOp->getRegions(), stablehloOp->getRegions())) {
      rewriter.inlineRegionBefore(hloRegion, stablehloRegion,
                                  stablehloRegion.end());
      if (failed(rewriter.convertRegionTypes(&stablehloRegion,
                                             *this->getTypeConverter(),
                                             /*entryConversion=*/nullptr)))
        return failure();
    }
    return success();
  }

  bool allowExperimentalFeatures;
};

template <typename... StablehloOpTypes>
void populateHloToStablehloPatterns(RewritePatternSet* patterns,
                                    TypeConverter* converter,
                                    MLIRContext* context,
                                    bool allowExperimentalFeatures) {
  patterns
      ->add<HloToStablehloOpConverter<StablehloToHloOp<StablehloOpTypes>>...>(
          *converter, context, allowExperimentalFeatures);
}

}  // namespace

void populateHloToStablehloPatterns(RewritePatternSet* patterns,
                                    TypeConverter* converter,
                                    MLIRContext* context,
                                    bool allowExperimentalFeatures) {
  // Populate conversion patterns for all StableHLO ops.
  // Our guiding principle is to support all StableHLO functionality in MHLO.
  // The inverse is not necessarily true - some MHLO ops are missing from
  // StableHLO (either deliberately or haven't yet been proposed to StableHLO).
  // As a result, these MHLO ops will not be added to these patterns and
  // will fail the conversion.
  populateHloToStablehloPatterns<
#define GET_OP_LIST
#include "stablehlo/dialect/StablehloOps.cpp.inc"
      >(patterns, converter, context, allowExperimentalFeatures);
}

}  // namespace stablehlo
}  // namespace mlir
