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
#include "llvm/Analysis/ValueTracking.h"
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

/// \brief Should this memory access be instrumented at all?
///
/// No, when the address is a global variable or a stack slot. Two reasons,
/// and the second is why this guard has to exist rather than merely being an
/// optimisation.
///
/// It changes no measurement. Both runtime hooks resolve the address against
/// the set of tracked heap allocations and increment nothing unless it falls
/// inside one; see access_heap_obj and access_unsafe_heap_obj in
/// lib/perf/src/heap_tracker.rs. A static or a stack slot never lies in that
/// set, so the call fires and does nothing. Skipping it removes a call, not a
/// count.
///
/// It also fixes a link failure. This pass runs after every optimisation, so
/// a load whose address is a `static` may reach it in a codegen unit that
/// only DECLARES that static, its definition having been dropped elsewhere as
/// unused. Passing the address to a hook materialises a reference to a symbol
/// nothing defines, and the link fails with "undefined hidden symbol". On the
/// 100-crate corpus this took out colored (colored::style::CLEAR) and tokio
/// (tokio::runtime::io::EXPOSE_IO), losing both crates' heap data entirely.
/// Verified on colored: without this pass no object references CLEAR at all
/// and the build links; with it, one codegen unit carries an undefined
/// reference.
///
/// getUnderlyingObject does not see through a load, so a pointer that was
/// READ from a global is still instrumented -- only accesses TO the global's
/// own storage are skipped.
bool skipMemInst(const Value *Addr) {
  const Value *Obj = getUnderlyingObject(Addr);
  return isa<GlobalValue>(Obj) || isa<AllocaInst>(Obj);
}

} // anonymous namespace

namespace {

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
      if (skipMemInst(DestAddr))
        continue;
      auto *Call = Builder.CreateCall(DynMemAccessFn, DestAddr);
      Call->setDebugLoc(getInstrumentationDebugLoc(MemInst));
    }
  }
}

/// \brief instrument unsafe memory accesses using sese region detection.
/// replaces the old per-bb linear scan with cross-bb aware region checking.
bool instrumentUnsafeMemInst(Function &F, FunctionCallee DynUnsafeMemAccessFn,
                             DominatorTree &DT, PostDominatorTree &PDT) {
  // phase 1: collect markers
  std::vector<CallInst*> BeginMarkers, EndMarkers;
  llvm::collectMarkers(F, BeginMarkers, EndMarkers);

  if (BeginMarkers.empty())
    return false;

  // phase 2: validate sese regions
  std::vector<SESERegion> ValidRegions;
  llvm::validateSESERegions(BeginMarkers, EndMarkers, DT, PDT, ValidRegions);

  if (ValidRegions.empty())
    return false;

  // phase 3: collect instructions inside valid regions
  SmallVector<Instruction*, 16> UnsafeMemInsts;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if ((isa<LoadInst>(I) || isa<StoreInst>(I)) &&
          llvm::isInSESERegion(I, ValidRegions, DT, PDT)) {
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
    if (skipMemInst(DestAddr))
      continue;
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
