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

#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>

#include "deallocation/IR/deallocation_ops.h"
#include "deallocation/transforms/passes.h"
#include "deallocation/utils/util.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Casting.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/Region.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"

namespace mlir {
namespace deallocation {
namespace {

bool isMemref(Value v) { return v.getType().isa<BaseMemRefType>(); }

struct TransformResult {
  // Allocs that are no longer owned by the current block. Note that it is valid
  // for an alloc to be both in `acquired` and `released`, if it was temporarily
  // released and then reacquired.
  llvm::SmallVector<Value> released;

  // Allocs that are now owned by the current block.
  llvm::SmallVector<Value> acquired;
};

bool doesAlias(Operation* op, Value v,
               breaks_if_you_move_ops::ValueEquivalenceClasses& aliases,
               bool considerOperands = true) {
  auto eq = [&](Value other) { return aliases.isEquivalent(v, other); };
  return op && ((considerOperands && llvm::any_of(op->getOperands(), eq)) ||
                llvm::any_of(op->getResults(), eq) ||
                llvm::any_of(op->getRegions(), [&](Region& region) {
                  return llvm::any_of(region.getOps(), [&](Operation& subOp) {
                    return doesAlias(&subOp, v, aliases);
                  });
                }));
}

struct Deallocator {
  // Transforms the operation and returns any allocations whose ownership is
  // transferred to the parent block.
  // `ownedMemrefs` contains the memrefs owned by the immediate parent block at
  // the point of `op`.
  TransformResult transformOp(
      Operation* op, const breaks_if_you_move_ops::ValueSet& ownedMemrefs);
  TransformResult transformOp(
      RegionBranchOpInterface op,
      const breaks_if_you_move_ops::ValueSet& ownedMemrefs);
  // Returns the values within the block that are retained, but does not add
  // them to the terminator.
  llvm::SmallVector<Value> transformBlock(Block& block, bool ownsInputs = true);

  // If `value` is guaranteed to be derived from a particular alloc, returns it.
  // Otherwise, returns null.
  Value getUniquePossibleAlloc(Value value);

  breaks_if_you_move_ops::ValueEquivalenceClasses aliases;
  llvm::SmallVector<RegionBranchOpInterface> loops;
};

Value Deallocator::getUniquePossibleAlloc(Value v) {
  Value result = {};
  for (auto it = aliases.findLeader(v); it != aliases.member_end(); ++it) {
    if (llvm::isa<BlockArgument>(*it) ||
        llvm::isa<memref::AllocOp>(it->getDefiningOp())) {
      if (result) return {};
      result = *it;
    }
  }
  return result;
}

llvm::SmallVector<Value> Deallocator::transformBlock(Block& block,
                                                     bool ownsInputs) {
  // Introduce block arguments for the owned inputs.
  breaks_if_you_move_ops::ValueSet ownedMemrefs;

  if (ownsInputs) {
    for (auto arg : llvm::to_vector(
             llvm::make_filter_range(block.getArguments(), isMemref))) {
      // Add an argument for a potentially owned memref.
      auto newArg = block.addArgument(getUnrankedMemrefType(arg),
                                      block.getParent()->getLoc());
      ownedMemrefs.insert(newArg);
      aliases.unionSets(arg, newArg);
    }
  }

  for (auto& op : llvm::make_early_inc_range(block.without_terminator())) {
    auto result = transformOp(&op, ownedMemrefs);
    // Remove released memrefs.
    for (auto v : result.released) {
      bool wasRemoved = ownedMemrefs.erase(v);
      (void)wasRemoved;
      assert(wasRemoved && "released an alloc that was not owned");
    }
    ownedMemrefs.insert(result.acquired.begin(), result.acquired.end());
  }
  auto yieldedMemrefs = llvm::to_vector(
      llvm::make_filter_range(block.getTerminator()->getOperands(), isMemref));

  // Handle owned memrefs that don't alias with any yielded memref first.
  for (auto v : ownedMemrefs) {
    if (!llvm::any_of(yieldedMemrefs, [&](Value yielded) {
          return aliases.isEquivalent(yielded, v);
        })) {
      // This owned memref does not escape, so we can put it in its own
      // retain and place it as early as possible.
      auto* insertionPoint = block.getTerminator();
      while (insertionPoint->getPrevNode() &&
             !doesAlias(insertionPoint->getPrevNode(), v, aliases)) {
        insertionPoint = insertionPoint->getPrevNode();
      }
      ImplicitLocOpBuilder b(block.getParent()->getLoc(), insertionPoint);
      b.create<RetainOp>(TypeRange{}, ValueRange{}, ValueRange{v});
    }
  }

  // Group yielded memrefs and owned memrefs by equivalence class leader.
  auto groupByLeader = [&](auto& values) {
    breaks_if_you_move_ops::ValueMap<SmallVector<Value>> result;
    for (auto v : values) {
      aliases.insert(v);
      result[aliases.getLeaderValue(v)].push_back(v);
    }
    return result;
  };
  auto yieldedByLeader = groupByLeader(yieldedMemrefs);
  auto ownedByLeader = groupByLeader(ownedMemrefs);

  // Create one retain per equivalence class.
  ImplicitLocOpBuilder b(block.getParent()->getLoc(), block.getTerminator());
  SmallVector<Value> results(yieldedMemrefs.size());
  for (auto [leader, yielded] : yieldedByLeader) {
    auto& ownedGroup = ownedByLeader[leader];
    if (ownedGroup.size() == 1 && yielded.size() == 1 &&
        getUniquePossibleAlloc(yielded.front()) == ownedGroup.front()) {
      // We know the alloc that the yielded memref is derived from, so we can
      // omit the retain op. This would better be a canonicalization pattern,
      // but it requires an alias analysis, which we already have here.
      auto cast = results[llvm::find(yieldedMemrefs, yielded.front()) -
                          yieldedMemrefs.begin()] =
          b.create<memref::CastOp>(getUnrankedMemrefType(yielded.front()),
                                   ownedGroup.front());
      aliases.unionSets(cast, ownedGroup.front());
    } else {
      auto types = llvm::to_vector(llvm::map_range(
          yielded, [](Value v) { return getUnrankedMemrefType(v); }));
      auto retain = b.create<RetainOp>(types, yielded, ownedGroup);
      for (auto [retained, result] : llvm::zip(retain.getResults(), yielded)) {
        aliases.unionSets(retained, result);
        results[llvm::find(yieldedMemrefs, result) - yieldedMemrefs.begin()] =
            retained;
      }
    }
  }
  for (auto [result, yielded] : llvm::zip(results, yieldedMemrefs)) {
    if (!result) {
      result = b.create<NullOp>(getUnrankedMemrefType(yielded)).getResult();
    }
  }
  return results;
}

struct MergedRetentionSet {
  SmallVector<Type> types;
  // [set index, type index] -> index in set.
  SmallVector<SmallVector<std::optional<int64_t>>> indices;
};

MergedRetentionSet mergeRetentionSets(
    ArrayRef<const SmallVector<Value>*> sets) {
  llvm::DenseMap<Type, SmallVector<size_t>> indicesByType;
  MergedRetentionSet result;
  for (const auto* set : sets) {
    auto& typeIndices = result.indices.emplace_back();
    DenseMap<Type, size_t> usedByType;
    for (auto [setIndex, v] : llvm::enumerate(*set)) {
      auto& indices = indicesByType[v.getType()];
      auto& numUsed = usedByType[v.getType()];
      if (indices.size() <= numUsed) {
        indices.push_back(result.types.size());
        result.types.push_back(v.getType());
      }

      if (typeIndices.size() < indices[numUsed] + 1) {
        typeIndices.resize(indices[numUsed] + 1);
      }
      typeIndices[indices[numUsed]] = setIndex;
      ++numUsed;
    }
  }
  for (auto& typeIndices : result.indices) {
    typeIndices.resize(result.types.size());
  }
  return result;
}

TransformResult Deallocator::transformOp(
    RegionBranchOpInterface op,
    const breaks_if_you_move_ops::ValueSet& ownedMemrefs) {
  SmallVector<int64_t> originalNumArgsByRegion;
  SmallVector<std::optional<int64_t>> successors(op->getNumRegions());
  SmallVector<SmallVector<Value>> retentionSetsByRegion;
  retentionSetsByRegion.reserve(op->getNumRegions());
  SmallVector<const SmallVector<Value>*> exitRegionSets;

  for (auto [index, region] : llvm::enumerate(op->getRegions())) {
    assert(region.getBlocks().size() <= 1 &&
           "expected regions to have at most one block");
    auto edges = getSuccessorRegions(op, index);
    originalNumArgsByRegion.push_back(region.getNumArguments());

    auto& retentionSet = retentionSetsByRegion.emplace_back();
    if (region.empty()) continue;

    // Transform region and collect owned memrefs.
    retentionSet = transformBlock(region.front());
    if (llvm::any_of(edges, [](auto& edge) {
          return edge.successorRegionIndex == std::nullopt;
        })) {
      exitRegionSets.push_back(&retentionSetsByRegion.back());
    } else {
      assert(edges.size() == 1);
      successors[index] = *edges.front().successorRegionIndex;
    }
  }

  // Compute the added result types and mapping to retained memrefs.
  auto merged = mergeRetentionSets(exitRegionSets);

  // Adjust terminator operands.
  for (auto [region, retentionSet, successor] :
       llvm::zip(op->getRegions(), retentionSetsByRegion, successors)) {
    if (region.empty()) continue;
    auto* terminator = region.front().getTerminator();
    if (successor) {
      terminator->setOperands(terminator->getNumOperands(), 0, retentionSet);
    } else {
      ImplicitLocOpBuilder b(op.getLoc(), terminator);
      for (auto [index, type] :
           llvm::zip(merged.indices[region.getRegionNumber()], merged.types)) {
        auto val =
            index ? retentionSet[*index] : b.create<NullOp>(type).getResult();
        terminator->setOperands(terminator->getNumOperands(), 0, val);
      }
    }
  }

  ImplicitLocOpBuilder b(op.getLoc(), op);
  SmallVector<Value> operands = op->getOperands();
  SmallVector<Value> released;
  // If we pass an owned memref to the loop and don't reuse it afterwards, we
  // can transfer ownership.
  for (auto operand : llvm::make_filter_range(operands, isMemref)) {
    auto isLastUse = [&]() {
      for (auto* candidate = op.getOperation(); candidate != nullptr;
           candidate = candidate->getNextNode()) {
        if (doesAlias(candidate, operand, aliases,
                      /*considerOperands=*/candidate != op.getOperation()))
          return false;
      }
      return true;
    };

    auto ty = getUnrankedMemrefType(operand);
    if (llvm::is_contained(ownedMemrefs, operand) && isLastUse() &&
        !llvm::is_contained(released, operand)) {
      // This is an alloc that is not used again, so we can pass ownership
      // to the loop.
      auto cast = b.create<memref::CastOp>(ty, operand);
      op->insertOperands(op->getNumOperands(), cast.getResult());
      released.push_back(operand);
    } else {
      // Either the operand is not an alloc or it's reused.
      op->insertOperands(op->getNumOperands(),
                         b.create<NullOp>(ty).getResult());
    }
  }

  RegionBranchOpInterface newOp = moveRegionsToNewOpButKeepOldOp(op);
  auto numOriginalResults = op->getNumResults();
  auto newResults = newOp->getResults().take_front(numOriginalResults);
  auto retained = newOp->getResults().drop_front(numOriginalResults);
  op->replaceAllUsesWith(newResults);
  op->erase();

  auto setupAliases = [&](std::optional<unsigned> index) {
    for (auto& region : getSuccessorRegions(newOp, index)) {
      for (auto [pred, succ] : llvm::zip(region.getPredecessorOperands(),
                                         region.getSuccessorValues())) {
        aliases.unionSets(pred, succ);
      }
    }
  };
  auto setMemrefAliases = [this](ValueRange a, ValueRange b) {
    for (auto [aa, bb] : llvm::zip(llvm::make_filter_range(a, isMemref), b)) {
      aliases.unionSets(aa, bb);
    }
  };
  setupAliases(std::nullopt);
  for (uint32_t i = 0; i < newOp->getNumRegions(); ++i) {
    setupAliases(i);
    auto args = newOp->getRegion(i).getArguments();
    auto n = originalNumArgsByRegion[i];
    setMemrefAliases(args.take_front(n), args.drop_front(n));
  }
  setMemrefAliases(newResults, retained);
  return {released, retained};
}

// Returns the set of values that are potentially owned by the op.
TransformResult Deallocator::transformOp(
    Operation* op, const breaks_if_you_move_ops::ValueSet& ownedMemrefs) {
  if (auto rbi = llvm::dyn_cast<RegionBranchOpInterface>(op)) {
    return transformOp(rbi, ownedMemrefs);
  }
  if (auto alloc = llvm::dyn_cast<memref::AllocOp>(op)) {
    return {{}, {alloc.getResult()}};
  }
  if (auto func = llvm::dyn_cast<func::FuncOp>(op)) {
    return {{}, transformBlock(func.getBody().front(), /*ownsInputs=*/false)};
  }
  // Assume any memref operand may alias any memref result.
  for (auto result : llvm::make_filter_range(op->getResults(), isMemref)) {
    for (auto arg : llvm::make_filter_range(op->getOperands(), isMemref)) {
      aliases.unionSets(result, arg);
    }
  }
  // No new allocations or releases.
  return {};
}

#define GEN_PASS_DEF_DEALLOCATEPASS
#include "deallocation/transforms/passes.h.inc"

struct DeallocatePass : public impl::DeallocatePassBase<DeallocatePass> {
  void runOnOperation() override {
    Deallocator().transformOp(this->getOperation(), {});
  }
};

}  // namespace

std::unique_ptr<mlir::OperationPass<mlir::func::FuncOp>>
createDeallocatePass() {
  return std::make_unique<DeallocatePass>();
}

}  // namespace deallocation
}  // namespace mlir
