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
///
//===-------------------------------------------------------------------------------===//

#include "llvm/Transforms/HeapTracker/HeapTracker.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
#include <cstdlib>
#include <cstring>

using namespace llvm;

const char *llvm::DYN_MEM_ACCESS_FN = "dyn_mem_access";
const char *llvm::DYN_UNSAFE_MEM_ACCESS_FN = "dyn_unsafe_mem_access";

namespace {

/// \brief Checks if the current build is for the primary package.
///
/// This uses the CARGO_PRIMARY_PACKAGE environment variable.
static bool isPrimaryPackage() {
  const char *P = getenv("CARGO_PRIMARY_PACKAGE");
  return P && strcmp(P, "1") == 0;
}

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
      Builder.CreateCall(DynMemAccessFn, DestAddr);
    }
  }
}

/// \brief Add a call to dyn_unsafe_mem_access() before each unsafe memory instruction.
/// \param F The target function.
/// \param DynUnsafeMemAccessFn The to-be-inserted callee.
void instrumentUnsafeMemInst(Function &F, FunctionCallee DynUnsafeMemAccessFn) {
  for (BasicBlock &BB : F) {
    Instruction *ActiveMarkerBegin = nullptr;

    for (Instruction &I : BB) {
      if (ActiveMarkerBegin) {
        if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
            IRBuilder<> Builder(&I);
            bool IsLoad = isa<LoadInst>(I);
            Value *DestAddr = IsLoad ? cast<LoadInst>(&I)->getPointerOperand() :
                                       cast<StoreInst>(&I)->getPointerOperand();
            Value *IsLoadVal = ConstantInt::get(Type::getInt1Ty(F.getContext()), IsLoad);
            Builder.CreateCall(DynUnsafeMemAccessFn, {DestAddr, IsLoadVal});
        }
      }

      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (auto *IA = dyn_cast<InlineAsm>(CI->getCalledOperand())) {
          StringRef AsmStr = IA->getAsmString();
          if (AsmStr == UNSAFE_MARKER_BEGIN) {
            ActiveMarkerBegin = &I;
          } else if (AsmStr == UNSAFE_MARKER_END) {
            if (ActiveMarkerBegin) {
              ActiveMarkerBegin = nullptr;
            }
          }
        }
      }
    }
  }
}

} // anonymous namespace

bool HeapTrackerPass::isPrimaryPackage() {
  const char *P = getenv("CARGO_PRIMARY_PACKAGE");
  return P && strcmp(P, "1") == 0;
}

PreservedAnalyses HeapTrackerPass::run(Function &F,
                                       FunctionAnalysisManager &AM) {
  if (!HeapTrackerPass::isPrimaryPackage())
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

  instrumentMemInst(F, DynMemAccessFn);

  instrumentUnsafeMemInst(F, DynUnsafeMemAccessFn);
  
  return PreservedAnalyses::all();
}
