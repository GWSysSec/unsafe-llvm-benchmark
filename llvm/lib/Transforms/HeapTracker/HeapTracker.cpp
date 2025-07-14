//===----- HeapTracker.cpp - Tracking memory access to heap -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/HeapTracker/HeapTracker.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"

static const char *DYN_MEM_ACCESS_FN        = "dyn_mem_access";
static const char *DYN_UNSAFE_MEM_ACCESS_FN = "dyn_unsafe_mem_access";

using namespace llvm;

/// @brief Add a call to dyn_mem_access() before each memory instruction.
/// @param F The target function.
/// @param dynMemAccessFn The to-be-inserted callee.
static void instrumentMemInst(Function &F, FunctionCallee dynMemAccessFn) {
  for (BasicBlock &BB : F) {
    SmallVector<Instruction*, 8> memInsts;
    for (Instruction &I : BB) {
      if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
        memInsts.push_back(&I);
      }
    }

    // Insert a call to dyn_mem_access() before each memory instruction.
    for (Instruction *memInst : memInsts) {
      IRBuilder<> Builder(memInst);
      Value *destAddr = isa<LoadInst>(memInst) ?
          cast<LoadInst>(memInst)->getPointerOperand() :
          cast<StoreInst>(memInst)->getPointerOperand();
      Builder.CreateCall(dynMemAccessFn, destAddr);
    }
  }
}

/// @brief Add a call to dyn_unsafe_mem_access() before each unsafe memory instruction.
/// @param F The target function.
/// @param dynUnsafeMemAccessFn The to-be-inserted callee.
static void instrumentUnsafeMemInst(Function &F, FunctionCallee dynUnsafeMemAccessFn) {
  for (BasicBlock &BB : F) {
    // This flag is reset for each basic block, enforcing the validation rule.
    bool unsafeBlockStarted = false;

    for (Instruction &I : BB) {
      // If we are in an unsafe block, find memory instructions and instrument them immediately.
      if (unsafeBlockStarted) {
        if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
            IRBuilder<> Builder(&I);
            bool isLoad = isa<LoadInst>(I);
            Value *destAddr = isLoad ? cast<LoadInst>(&I)->getPointerOperand() :
                                       cast<StoreInst>(&I)->getPointerOperand();
            Value *isLoadVal = ConstantInt::get(Type::getInt1Ty(F.getContext()), isLoad);
            Builder.CreateCall(dynUnsafeMemAccessFn, {destAddr, isLoadVal});
        }
      }

      // Check for markers to toggle the state of unsafeBlockStarted.
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (auto *IA = dyn_cast<InlineAsm>(CI->getCalledOperand())) {
          StringRef AsmStr = IA->getAsmString();
          if (AsmStr == UNSAFE_MARKER_BEGIN) {
            unsafeBlockStarted = true;
          } else if (AsmStr == UNSAFE_MARKER_END) {
            unsafeBlockStarted = false;
          }
        }
      }
    }
  }
}

PreservedAnalyses HeapTrackerPass::run(Function &F,
                                                      FunctionAnalysisManager &AM) {
  // Define fn prototypes of dyn_mem_access() and dyn_unsafe_mem_access()
  // defined in the Rust runlib lib.
  LLVMContext &C = F.getContext();
  Module *M = F.getParent();
  Type *voidTy = Type::getVoidTy(C);
  Type *rawPtrTy = PointerType::getUnqual(Type::getInt8Ty(C));
  Type *booleanTy = Type::getInt1Ty(C);
  FunctionType *dynMemAccessFnTy = FunctionType::get(voidTy, rawPtrTy, false);
  FunctionCallee dynMemAccessFn = M->getOrInsertFunction(
    DYN_MEM_ACCESS_FN, dynMemAccessFnTy);
  FunctionType *dynUnsafeMemAccessFnTy = FunctionType::get(
    voidTy, {rawPtrTy, booleanTy}, false);
  FunctionCallee dynUnsafeMemAccessFn = M->getOrInsertFunction(
    DYN_UNSAFE_MEM_ACCESS_FN, dynUnsafeMemAccessFnTy);

  // First, insert calls to dyn_mem_access for all memory accesses.
  instrumentMemInst(F, dynMemAccessFn);

  // Then, insert calls to dyn_unsafe_mem_access for memory accesses within unsafe blocks.
  instrumentUnsafeMemInst(F, dynUnsafeMemAccessFn);
  
  return PreservedAnalyses::all();
}
