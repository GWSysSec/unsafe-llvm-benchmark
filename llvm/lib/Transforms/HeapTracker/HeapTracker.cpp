//===----- HeapTracker.cpp - Tracking memory access to heap -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/HeapTracker/HeapTracker.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Constants.h"

static const char *UNSAFE_MARKER_BEGIN      = "nop # marker_begin";
static const char *UNSAFE_MARKER_END        = "nop # marker_end";
static const char *DYN_UNSAFE_MEM_ACCESS_FN = "dyn_unsafe_mem_access";

using namespace llvm;

PreservedAnalyses HeapTrackerPass::run(Function &F,
                                                    FunctionAnalysisManager &AM) {
  // Define a fn prototype for dyn_unsafe_mem_access() defined in our Rust lib.
  LLVMContext &C = F.getContext();                                                 
  Module *M = F.getParent();
  Type *voidTy = Type::getVoidTy(C);
  Type *rawPtrTy = PointerType::getUnqual(Type::getInt8Ty(C));
  Type *booleanTy = Type::getInt1Ty(C);
  FunctionType *dynUnsafeMemAccessFnTy = FunctionType::get(
    voidTy, {rawPtrTy, booleanTy}, false);
  FunctionCallee dynUnsafeMemAccessFn = M->getOrInsertFunction(
    DYN_UNSAFE_MEM_ACCESS_FN, dynUnsafeMemAccessFnTy);

  for (BasicBlock &BB : F) {
    // Indicating whether the the pass has entered into an unsafe block.
    bool unsafeBlockStarted = false;
    SmallVector<Instruction *, 8> unsafeMemInsts;

    for (Instruction &I : BB) {
      Instruction *Inst = &I;
      // Collect memory instructions.
      if (unsafeBlockStarted && (isa<LoadInst>(I) || isa<StoreInst>(I))) {
        unsafeMemInsts.push_back(Inst);
        continue;
      }

      if (CallInst *CI = dyn_cast<CallInst>(&I)) {
        if (InlineAsm *IA = dyn_cast<InlineAsm>(CI->getCalledOperand())) {
          StringRef AsmStr = IA->getAsmString();
          if (AsmStr == UNSAFE_MARKER_BEGIN) {
            unsafeBlockStarted = true;
          } else if (AsmStr == UNSAFE_MARKER_END) {
            unsafeBlockStarted = false;
          }
        }
      }
    }

    // Insert a call to dyn_unsafe_mem_access() before each unsafe memory instruction.
    for (Instruction *memInst : unsafeMemInsts) {
      bool isLoad = isa<LoadInst>(memInst);
      Value *destAddr = isLoad ? cast<LoadInst>(memInst)->getPointerOperand() :
                                 cast<StoreInst>(memInst)->getPointerOperand();
      Value *isLoadVal = ConstantInt::get(booleanTy, isLoad);
      CallInst::Create(dynUnsafeMemAccessFn, {destAddr, isLoadVal}, "", memInst);
    }
  }
  return PreservedAnalyses::all();
}