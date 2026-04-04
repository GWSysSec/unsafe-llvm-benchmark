//===-- HeapTracker.cpp - Track memory access to heap ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===-------------------------------------------------------------------------------===//
///
/// \file
/// This file implements the HeapTracker pass for tracking memory access to heap.
/// Uses SESE (Single-Entry Single-Exit) region detection via DomTree and
/// PostDomTree for robust cross-BB unsafe region handling.
///
//===-------------------------------------------------------------------------------===//

#include "llvm/Transforms/DynamicAnalysis/HeapTracker.h"
#include "llvm/Transforms/DynamicAnalysis/UnsafeAnalysisUtils.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"

#define DEBUG_TYPE "heap-tracker"

using namespace llvm;

const char *llvm::DYN_MEM_ACCESS_FN = "dyn_mem_access";
const char *llvm::DYN_UNSAFE_MEM_ACCESS_FN = "dyn_unsafe_mem_access";

namespace {

/// \brief represents a validated sese region bounded by begin/end markers.
struct SESERegion {
  CallInst *Begin;
  CallInst *End;
};

/// \brief Add a call to dyn_mem_access() before each memory instruction.
/// \param F The target function.
/// \param DynMemAccessFn The to-be-inserted callee.
void instrumentMemInst(Function &F, FunctionCallee DynMemAccessFn) {
  for (BasicBlock &BB : F) {
    SmallVector<Instruction*, 8> memInsts;
    for (Instruction &I : BB) {
      if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
        memInsts.push_back(&I);
      }
    }

    for (Instruction *MemInst : memInsts) {
      IRBuilder<> Builder(MemInst);
      Value *DestAddr = isa<LoadInst>(MemInst) ?
          cast<LoadInst>(MemInst)->getPointerOperand() :
          cast<StoreInst>(MemInst)->getPointerOperand();
      auto *Call = Builder.CreateCall(DynMemAccessFn, DestAddr);
      Call->setDebugLoc(getInstrumentationDebugLoc(MemInst));
    }
  }
}

/// \brief collect all unsafe marker begin/end callsites in the function.
void collectMarkers(Function &F,
                    std::vector<CallInst*> &BeginMarkers,
                    std::vector<CallInst*> &EndMarkers) {
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (isMarkerBegin(I))
          BeginMarkers.push_back(CI);
        else if (isMarkerEnd(I))
          EndMarkers.push_back(CI);
      }
    }
  }
}

/// \brief validate sese regions by matching begin/end markers using dominance.
/// a valid sese region requires: begin dominates end AND end post-dominates begin.
void validateSESERegions(const std::vector<CallInst*> &BeginMarkers,
                         const std::vector<CallInst*> &EndMarkers,
                         DominatorTree &DT, PostDominatorTree &PDT,
                         std::vector<SESERegion> &ValidRegions) {
  for (CallInst *Begin : BeginMarkers) {
    bool Matched = false;
    for (CallInst *End : EndMarkers) {
      if (DT.dominates(Begin, End) && PDT.dominates(End, Begin)) {
        ValidRegions.push_back({Begin, End});
        LLVM_DEBUG(dbgs() << "heap-tracker: valid sese region found in "
                          << Begin->getFunction()->getName() << "\n");
        Matched = true;
        break;
      }
    }
    if (!Matched) {
      LLVM_DEBUG(dbgs() << "heap-tracker: unmatched marker in "
                        << Begin->getFunction()->getName() << "\n");
    }
  }
}

/// \brief check if an instruction's bb lies inside any valid sese region.
bool isInSESERegion(const Instruction &I,
                    const std::vector<SESERegion> &ValidRegions,
                    DominatorTree &DT, PostDominatorTree &PDT) {
  for (const auto &R : ValidRegions) {
    if (DT.dominates(R.Begin->getParent(), I.getParent()) &&
        PDT.dominates(R.End->getParent(), I.getParent())) {
      return true;
    }
  }
  return false;
}

/// \brief instrument unsafe memory accesses using sese region detection.
/// replaces the old per-bb linear scan with cross-bb aware region checking.
bool instrumentUnsafeMemInst(Function &F, FunctionCallee DynUnsafeMemAccessFn,
                             DominatorTree &DT, PostDominatorTree &PDT) {
  // phase 1: collect markers
  std::vector<CallInst*> BeginMarkers, EndMarkers;
  collectMarkers(F, BeginMarkers, EndMarkers);

  if (BeginMarkers.empty())
    return false;

  // phase 2: validate sese regions
  std::vector<SESERegion> ValidRegions;
  validateSESERegions(BeginMarkers, EndMarkers, DT, PDT, ValidRegions);

  if (ValidRegions.empty())
    return false;

  // phase 3: collect instructions inside valid regions
  SmallVector<Instruction*, 16> UnsafeMemInsts;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if ((isa<LoadInst>(I) || isa<StoreInst>(I)) &&
          isInSESERegion(I, ValidRegions, DT, PDT)) {
        UnsafeMemInsts.push_back(&I);
      }
    }
  }

  if (UnsafeMemInsts.empty())
    return false;

  // phase 4: instrument
  for (Instruction *MemInst : UnsafeMemInsts) {
    IRBuilder<> Builder(MemInst);
    bool IsLoad = isa<LoadInst>(MemInst);
    Value *DestAddr = IsLoad ? cast<LoadInst>(MemInst)->getPointerOperand()
                             : cast<StoreInst>(MemInst)->getPointerOperand();
    Value *IsLoadVal = ConstantInt::get(
        Type::getInt1Ty(F.getContext()), IsLoad);
    auto *Call = Builder.CreateCall(DynUnsafeMemAccessFn, {DestAddr, IsLoadVal});
    Call->setDebugLoc(getInstrumentationDebugLoc(MemInst));
  }

  return true;
}

} // anonymous namespace

PreservedAnalyses HeapTrackerPass::run(Function &F,
                                       FunctionAnalysisManager &AM) {
  if (!isPrimaryPackage())
    return PreservedAnalyses::all();

  LLVMContext &C = F.getContext();
  Module *M = F.getParent();
  Type *VoidTy = Type::getVoidTy(C);
  Type *RawPtrTy = PointerType::getUnqual(Type::getInt8Ty(C));
  Type *BooleanTy = Type::getInt1Ty(C);
  FunctionType *DynMemAccessFnTy = FunctionType::get(VoidTy, RawPtrTy, false);
  FunctionCallee DynMemAccessFn = M->getOrInsertFunction(
    DYN_MEM_ACCESS_FN, DynMemAccessFnTy);
  FunctionType *DynUnsafeMemAccessFnTy = FunctionType::get(
    VoidTy, {RawPtrTy, BooleanTy}, false);
  FunctionCallee DynUnsafeMemAccessFn = M->getOrInsertFunction(
    DYN_UNSAFE_MEM_ACCESS_FN, DynUnsafeMemAccessFnTy);

  // instrument all memory accesses (unchanged)
  instrumentMemInst(F, DynMemAccessFn);

  // instrument unsafe memory accesses using sese validation
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &PDT = AM.getResult<PostDominatorTreeAnalysis>(F);
  bool UnsafeModified = instrumentUnsafeMemInst(F, DynUnsafeMemAccessFn,
                                                 DT, PDT);

  // invalidate analyses if we modified the function
  return UnsafeModified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
