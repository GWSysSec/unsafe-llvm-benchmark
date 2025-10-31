//===-- ExternalCallTracker.cpp - Track external function call time -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------------===//
///
/// \file
/// This file implements the ExternalCallTracker pass for tracking time spent in
/// external library function calls.
///
//===----------------------------------------------------------------------------===//

#include "llvm/Transforms/CpuCycleCount/ExternalCallTracker.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include <cstdlib>
#include <cstring>

using namespace llvm;

// Runtime function names
const char *llvm::EXTERNAL_CALL_START_FN = "external_call_start";
const char *llvm::EXTERNAL_CALL_END_FN = "external_call_end";

namespace {

static bool isPrimaryPackage() {
  const char *P = getenv("CARGO_PRIMARY_PACKAGE");
  return P && strcmp(P, "1") == 0;
}

/// Checks if a function name is a runtime function that should not be instrumented.
static bool isRuntimeFunction(StringRef Name) {
  return Name.startswith("cpu_cycle_") ||
         Name.startswith("record_") ||
         Name.startswith("external_call_");
}

/// Instruments external function calls within a function.
/// Uses a three-pass strategy to avoid iterator invalidation.
bool instrumentExternalCalls(Function &F, FunctionCallee ExtStartFn,
                              FunctionCallee ExtEndFn) {
  // First pass: collect all external calls to instrument
  SmallVector<Instruction*, 32> CallsToInstrument;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *Call = dyn_cast<CallBase>(&I);
      if (!Call) continue;

      Function *CalledFn = Call->getCalledFunction();
      // Check if the called function is external (declaration) and not an intrinsic
      if (!CalledFn || !CalledFn->isDeclaration() || CalledFn->isIntrinsic())
        continue;

      // Skip runtime functions to avoid recursion
      if (isRuntimeFunction(CalledFn->getName()))
        continue;

      CallsToInstrument.push_back(&I);
    }
  }

  if (CallsToInstrument.empty())
    return false;

  // Second pass: insert instrumentation around collected calls
  bool Modified = false;
  for (Instruction *I : CallsToInstrument) {
    // Skip terminator instructions to avoid IR corruption
    if (I->isTerminator())
      continue;

    // Insert timer start before the call
    IRBuilder<> Builder(I);
    Builder.CreateFence(AtomicOrdering::SequentiallyConsistent);
    Value *StartTime = Builder.CreateCall(ExtStartFn);

    // Insert timer end after the call
    Instruction *NextInst = I->getNextNonDebugInstruction();
    if (NextInst) {
      IRBuilder<> EndBuilder(NextInst);
      EndBuilder.CreateFence(AtomicOrdering::SequentiallyConsistent);
      EndBuilder.CreateCall(ExtEndFn, {StartTime});
      Modified = true;
    }
    // Note: Calls at block end without a next instruction are skipped to avoid
    // IR corruption. The runtime will handle this gracefully via the TSC == 0 check.
  }

  return Modified;
}

} // namespace

PreservedAnalyses ExternalCallTrackerPass::run(Module &M, ModuleAnalysisManager &AM) {
  if (!isPrimaryPackage())
    return PreservedAnalyses::all();

  LLVMContext &Ctx = M.getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *Int64Ty = Type::getInt64Ty(Ctx);

  // Get declarations for external call tracking runtime functions
  FunctionCallee ExtStartFn = M.getOrInsertFunction(EXTERNAL_CALL_START_FN,
      FunctionType::get(Int64Ty, {}, false));
  FunctionCallee ExtEndFn = M.getOrInsertFunction(EXTERNAL_CALL_END_FN,
      FunctionType::get(VoidTy, {Int64Ty}, false));

  bool Modified = false;

  // Instrument all non-declaration functions
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;

    // Skip runtime functions
    if (isRuntimeFunction(F.getName()))
      continue;

    if (instrumentExternalCalls(F, ExtStartFn, ExtEndFn))
      Modified = true;
  }

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
