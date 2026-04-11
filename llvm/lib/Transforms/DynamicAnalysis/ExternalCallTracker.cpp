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
// UNSAFE-RUST BEGIN
// Phase 6.5: external calls made FROM inside an unsafe SESE region are now
// instrumented with separate hooks so the runtime can split unsafe_cycles
// into unsafe_cycles_internal and unsafe_cycles_external.
const char *llvm::UNSAFE_EXTERNAL_CALL_START_FN = "unsafe_external_call_start";
const char *llvm::UNSAFE_EXTERNAL_CALL_END_FN = "unsafe_external_call_end";
// UNSAFE-RUST END

namespace {

/// \brief Check if a function is a runtime function for the CpuCycleCount +
/// ExternalCallTracker experiment. These must not be instrumented to avoid
/// recursion and measurement distortion.
// UNSAFE-RUST BEGIN
static bool isOwnRuntimeFunction(StringRef Name) {
  return Name.starts_with("cpu_cycle_") ||           // CpuCycleCount
         Name.starts_with("record_program_") ||      // CpuCycleCount
         Name.starts_with("print_cpu_cycle_") ||     // CpuCycleCount
         Name.starts_with("external_call_") ||       // ExternalCallTracker
         Name.starts_with("unsafe_external_call_");  // ExternalCallTracker (unsafe ctx)
}
// UNSAFE-RUST END

/// Wraps a single external call instruction with Start/End timing hooks.
/// Returns true if instrumentation was inserted.
static bool wrapCallWithTimingHooks(Instruction *I,
                                    FunctionCallee StartFn,
                                    FunctionCallee EndFn) {
  if (I->isTerminator())
    return false;

  IRBuilder<> Builder(I);
  Builder.CreateFence(AtomicOrdering::SequentiallyConsistent);
  auto *StartCall = Builder.CreateCall(StartFn);
  StartCall->setDebugLoc(getInstrumentationDebugLoc(I));

  Instruction *NextInst = I->getNextNonDebugInstruction();
  if (!NextInst)
    return false;

  IRBuilder<> EndBuilder(NextInst);
  EndBuilder.CreateFence(AtomicOrdering::SequentiallyConsistent);
  auto *EndCall = EndBuilder.CreateCall(EndFn, {StartCall});
  EndCall->setDebugLoc(getInstrumentationDebugLoc(NextInst));
  return true;
}

/// Instruments external function calls within a function.
///
/// Phase 6.5: we now instrument external calls BOTH inside and outside unsafe
/// SESE regions, but into SEPARATE runtime hooks:
///   - Outside SESE: external_call_start/end  → safe external cycles
///   - Inside SESE:  unsafe_external_call_start/end → unsafe_external cycles
///
/// The "unsafe external" timing is a strict subset of the surrounding SESE
/// region's cpu_cycle measurement; it's recorded as a separate atomic so
/// the runtime can split unsafe_cycles_total into internal vs external.
///
/// Uses SESE region detection (DomTree + PostDomTree) for cross-BB correctness.
bool instrumentExternalCalls(Function &F,
                             FunctionCallee ExtStartFn,
                             FunctionCallee ExtEndFn,
                             FunctionCallee UnsafeExtStartFn,
                             FunctionCallee UnsafeExtEndFn) {
  // Build SESE regions for cross-BB unsafe region detection
  DominatorTree DT(F);
  PostDominatorTree PDT(F);

  std::vector<CallInst *> BeginMarkers, EndMarkers;
  llvm::collectMarkers(F, BeginMarkers, EndMarkers);

  std::vector<SESERegion> ValidRegions;
  if (!BeginMarkers.empty())
    llvm::validateSESERegions(BeginMarkers, EndMarkers, DT, PDT, ValidRegions);

  // Two buckets: external calls inside vs. outside any unsafe SESE region.
  SmallVector<Instruction*, 32> SafeCallsToInstrument;
  SmallVector<Instruction*, 32> UnsafeCallsToInstrument;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *Call = dyn_cast<CallBase>(&I);
      if (!Call) continue;

      // Skip markers themselves
      bool isBegin = false, isEnd = false;
      if (isMarkerInstruction(I, isBegin, isEnd))
        continue;

      Function *CalledFn = Call->getCalledFunction();
      if (!CalledFn || !CalledFn->isDeclaration() || CalledFn->isIntrinsic())
        continue;

      if (isOwnRuntimeFunction(CalledFn->getName()))
        continue;

      bool InUnsafe = !ValidRegions.empty() &&
                      llvm::isInSESERegion(I, ValidRegions, DT, PDT);
      if (InUnsafe)
        UnsafeCallsToInstrument.push_back(&I);
      else
        SafeCallsToInstrument.push_back(&I);
    }
  }

  bool Modified = false;
  for (Instruction *I : SafeCallsToInstrument)
    Modified |= wrapCallWithTimingHooks(I, ExtStartFn, ExtEndFn);
  for (Instruction *I : UnsafeCallsToInstrument)
    Modified |= wrapCallWithTimingHooks(I, UnsafeExtStartFn, UnsafeExtEndFn);

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
  // UNSAFE-RUST BEGIN
  FunctionCallee UnsafeExtStartFn = M.getOrInsertFunction(
      UNSAFE_EXTERNAL_CALL_START_FN,
      FunctionType::get(Int64Ty, {}, false));
  FunctionCallee UnsafeExtEndFn = M.getOrInsertFunction(
      UNSAFE_EXTERNAL_CALL_END_FN,
      FunctionType::get(VoidTy, {Int64Ty}, false));
  // UNSAFE-RUST END

  bool Modified = false;

  // Instrument all non-declaration functions
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;

    // Skip runtime functions
    if (isOwnRuntimeFunction(F.getName()))
      continue;

    if (instrumentExternalCalls(F, ExtStartFn, ExtEndFn,
                                UnsafeExtStartFn, UnsafeExtEndFn))
      Modified = true;
  }

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
