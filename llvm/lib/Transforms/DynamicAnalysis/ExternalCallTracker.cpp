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
#include "llvm/IR/DebugInfoMetadata.h"
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
/// Uses a three-pass strategy to avoid iterator invalidation.
/// Skips external calls inside unsafe marker regions — those calls are already
/// measured by CpuCycleCount, and the runtime would no-op them anyway
/// (IN_UNSAFE > 0). Skipping avoids wasted MFENCE + function call overhead
/// that would otherwise inflate unsafe cycle measurements.
bool instrumentExternalCalls(Function &F, FunctionCallee ExtStartFn,
                              FunctionCallee ExtEndFn) {
  // First pass: collect external calls to instrument, skipping those inside
  // unsafe marker regions (between marker_begin and marker_end).
  SmallVector<Instruction*, 32> CallsToInstrument;

  for (BasicBlock &BB : F) {
    bool InsideMarkerRegion = false;

    for (Instruction &I : BB) {
      auto *Call = dyn_cast<CallBase>(&I);
      if (!Call) continue;

      // Track marker region boundaries
      if (isMarkerAsm(Call, llvm::UNSAFE_MARKER_BEGIN)) {
        InsideMarkerRegion = true;
        continue;
      }
      if (isMarkerAsm(Call, llvm::UNSAFE_MARKER_END)) {
        InsideMarkerRegion = false;
        continue;
      }

      // Skip calls inside unsafe marker regions
      if (InsideMarkerRegion)
        continue;

      Function *CalledFn = Call->getCalledFunction();
      // Check if the called function is external (declaration) and not an intrinsic
      if (!CalledFn || !CalledFn->isDeclaration() || CalledFn->isIntrinsic())
        continue;

      // Skip runtime functions to avoid recursion
      if (isOwnRuntimeFunction(CalledFn->getName()))
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
    auto *StartCall = Builder.CreateCall(ExtStartFn);
    StartCall->setDebugLoc(getInstrumentationDebugLoc(I));

    // Insert timer end after the call
    Instruction *NextInst = I->getNextNonDebugInstruction();
    if (NextInst) {
      IRBuilder<> EndBuilder(NextInst);
      EndBuilder.CreateFence(AtomicOrdering::SequentiallyConsistent);
      auto *EndCall = EndBuilder.CreateCall(ExtEndFn, {StartCall});
      EndCall->setDebugLoc(getInstrumentationDebugLoc(NextInst));
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
    if (isOwnRuntimeFunction(F.getName()))
      continue;

    if (instrumentExternalCalls(F, ExtStartFn, ExtEndFn))
      Modified = true;
  }

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
