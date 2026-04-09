//===-- UnsafeInstCounter.h - Count unsafe instructions -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Function pass that counts unsafe instructions in basic blocks.
/// Must run after UnsafeFunctionTracker.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UNSAFECOUNT_UNSAFEINSTCOUNTER_H
#define LLVM_TRANSFORMS_UNSAFECOUNT_UNSAFEINSTCOUNTER_H

#include "llvm/IR/PassManager.h"
#include <cstdint>
#include <vector>

namespace llvm {

class Function;
class BasicBlock;
class Instruction;
class DominatorTree;
class PostDominatorTree;
struct SESERegion;

/// \brief Count unsafe instructions in functions.
///
/// This pass instruments basic blocks to count unsafe instructions.
/// It expects functions to already have ID metadata from UnsafeFunctionTracker.
struct UnsafeInstCounterPass : public PassInfoMixin<UnsafeInstCounterPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  
  static bool isRequired() { return true; }
  
private:
  /// \brief Categories of unsafe instructions
  enum UnsafeCategory : uint8_t {
    UNSAFE_LOAD = 0,
    UNSAFE_STORE = 1,
    UNSAFE_CALL_DIRECT = 2,
    UNSAFE_CALL_INDIRECT = 3,
    UNSAFE_CALL_INTRINSIC = 4,
    UNSAFE_CAST = 5,
    UNSAFE_GEP = 6,
    UNSAFE_ATOMIC = 7,
    UNSAFE_OTHER = 8,
    MAX_UNSAFE_CATEGORIES = 9
  };
  
  /// \brief Counts for a basic block
  struct BlockCounts {
    uint32_t totalInsts;
    uint32_t totalUnsafeInsts;
    uint16_t unsafeCounts[MAX_UNSAFE_CATEGORIES];
    
    BlockCounts();
    bool hasInstructions() const { return totalInsts > 0; }
    bool hasUnsafeInstructions() const { return totalUnsafeInsts > 0; }
  };
  
  static bool getUnsafeCategory(const Instruction &I, UnsafeCategory &category);
  static BlockCounts analyzeBasicBlock(BasicBlock &BB,
                                       const std::vector<SESERegion> &Regions,
                                       DominatorTree &DT,
                                       PostDominatorTree &PDT);
  static uint32_t getFunctionId(Function &F);
};

} // namespace llvm

#endif
