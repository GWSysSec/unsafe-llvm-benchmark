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

#include "llvm/Transforms/DynamicAnalysis/ExternalCallTracker.h"
#include "llvm/Transforms/DynamicAnalysis/UnsafeAnalysisUtils.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"

using namespace llvm;

// Runtime function names
const char *llvm::EXTERNAL_CALL_START_FN = "external_call_start";
const char *llvm::EXTERNAL_CALL_END_FN = "external_call_end";

namespace {

/// \brief Check if a function is a runtime function for the CpuCycleCount +
/// ExternalCallTracker experiment. These must not be instrumented to avoid
/// recursion and measurement distortion.
// UNSAFE-RUST BEGIN
static bool isOwnRuntimeFunction(StringRef Name) {
  return Name.starts_with("cpu_cycle_") ||        // CpuCycleCount
         Name.starts_with("record_program_") ||   // CpuCycleCount
         Name.starts_with("print_cpu_cycle_") ||  // CpuCycleCount
         Name.starts_with("external_call_");      // ExternalCallTracker
}
// UNSAFE-RUST END

/// Instruments external function calls within a function.
/// Skips external calls inside unsafe SESE regions — those calls are already
/// measured by CpuCycleCount, and the runtime would no-op them anyway
/// (IN_UNSAFE > 0). Skipping avoids wasted MFENCE + function call overhead
/// that would otherwise inflate unsafe cycle measurements.
///
/// Uses SESE region detection (DomTree + PostDomTree) for cross-BB correctness.
bool instrumentExternalCalls(Function &F, FunctionCallee ExtStartFn,
                              FunctionCallee ExtEndFn) {
  // Build SESE regions for cross-BB unsafe region detection
  DominatorTree DT(F);
  PostDominatorTree PDT(F);

  std::vector<CallInst *> BeginMarkers, EndMarkers;
  llvm::collectMarkers(F, BeginMarkers, EndMarkers);

  std::vector<SESERegion> ValidRegions;
  if (!BeginMarkers.empty())
    llvm::validateSESERegions(BeginMarkers, EndMarkers, DT, PDT, ValidRegions);

  // Collect external calls to instrument, skipping those inside SESE regions
  SmallVector<Instruction*, 32> CallsToInstrument;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *Call = dyn_cast<CallBase>(&I);
      if (!Call) continue;

      // Skip markers themselves
      bool isBegin = false, isEnd = false;
      if (isMarkerInstruction(I, isBegin, isEnd))
        continue;

      // Skip calls inside unsafe SESE regions
      if (!ValidRegions.empty() && llvm::isInSESERegion(I, ValidRegions, DT, PDT))
        continue;

      Function *CalledFn = Call->getCalledFunction();
      if (!CalledFn || !CalledFn->isDeclaration() || CalledFn->isIntrinsic())
        continue;

      if (isOwnRuntimeFunction(CalledFn->getName()))
        continue;

      CallsToInstrument.push_back(&I);
    }
  }

  if (CallsToInstrument.empty())
    return false;

  // Insert instrumentation around collected calls
  bool Modified = false;
  for (Instruction *I : CallsToInstrument) {
    if (I->isTerminator())
      continue;

    IRBuilder<> Builder(I);
    Builder.CreateFence(AtomicOrdering::SequentiallyConsistent);
    auto *StartCall = Builder.CreateCall(ExtStartFn);
    StartCall->setDebugLoc(getInstrumentationDebugLoc(I));

    Instruction *NextInst = I->getNextNonDebugInstruction();
    if (NextInst) {
      IRBuilder<> EndBuilder(NextInst);
      EndBuilder.CreateFence(AtomicOrdering::SequentiallyConsistent);
      auto *EndCall = EndBuilder.CreateCall(ExtEndFn, {StartCall});
      EndCall->setDebugLoc(getInstrumentationDebugLoc(NextInst));
      Modified = true;
    }
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
    if (isOwnRuntimeFunction(F.getName()))
      continue;

    if (instrumentExternalCalls(F, ExtStartFn, ExtEndFn))
      Modified = true;
  }

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
