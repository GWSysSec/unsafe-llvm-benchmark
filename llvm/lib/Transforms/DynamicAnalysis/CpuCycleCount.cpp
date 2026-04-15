//===-- CpuCycleCount.cpp - Track unsafe instruction execution time -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===-------------------------------------------------------------------------------------===//
///
/// \file
/// This file implements the CpuCycleCount pass for tracking CPU cycles spent
/// executing unsafe code blocks.
///
//===--------------------------------------------------------------------------------------==//

#include "llvm/Transforms/DynamicAnalysis/CpuCycleCount.h"
#include "llvm/Transforms/DynamicAnalysis/UnsafeAnalysisUtils.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

// Runtime function names
const char *llvm::PROGRAM_START_FN = "record_program_start";
const char *llvm::START_MEASUREMENT_FN = "cpu_cycle_start_measurement";
const char *llvm::END_MEASUREMENT_FN = "cpu_cycle_end_measurement";
const char *llvm::PRINT_STATS_FN = "print_cpu_cycle_stats";

namespace {

/// Instruments unsafe blocks within a function to measure CPU cycles.
///
/// Uses the same SESE matcher as ExternalCallTracker (DominatorTree +
/// PostDominatorTree). A previous per-BasicBlock scan silently skipped
/// regions whose BEGIN and END markers ended up in different BBs — which
/// happens for any unsafe block containing a call with unwind, a loop, a
/// match, or an early return. The asymmetry vs. ExternalCallTracker (which
/// already used the DomTree matcher) caused unsafe_external_cycles to be
/// recorded for regions where unsafe_cycles was not, violating the
/// invariant unsafe_ext ⊆ unsafe_total.
bool instrumentUnsafeBlocks(Function &F, FunctionCallee StartFn,
                             FunctionCallee EndFn) {
  DominatorTree DT(F);
  PostDominatorTree PDT(F);

  std::vector<CallInst *> BeginMarkers, EndMarkers;
  llvm::collectMarkers(F, BeginMarkers, EndMarkers);

  if (BeginMarkers.empty())
    return false;

  std::vector<SESERegion> ValidRegions;
  llvm::validateSESERegions(BeginMarkers, EndMarkers, DT, PDT, ValidRegions);

  if (ValidRegions.empty())
    return false;

  // Pass 1: insert instrumentation while markers are still in place.
  //   StartFn goes BEFORE the begin marker; EndFn goes AFTER the end marker,
  //   each wrapped in a SeqCst fence to prevent reorder across the boundary.
  SmallPtrSet<Instruction *, 32> MarkersToRemove;
  for (const SESERegion &R : ValidRegions) {
    Instruction *BeginMarker = R.Begin;
    Instruction *EndMarker = R.End;

    IRBuilder<> BeginBuilder(BeginMarker);
    BeginBuilder.CreateFence(AtomicOrdering::SequentiallyConsistent);
    auto *StartCall = BeginBuilder.CreateCall(StartFn);
    StartCall->setDebugLoc(getInstrumentationDebugLoc(BeginMarker));

    // End marker: insert AFTER it to match the semantic "timer stops after the
    // last instruction of the region", while keeping the fence tight to the
    // end of the region.
    Instruction *AfterEnd = EndMarker->getNextNonDebugInstruction();
    IRBuilder<> EndBuilder(AfterEnd ? AfterEnd : EndMarker);
    EndBuilder.CreateFence(AtomicOrdering::SequentiallyConsistent);
    auto *EndCall = EndBuilder.CreateCall(EndFn, {StartCall});
    EndCall->setDebugLoc(getInstrumentationDebugLoc(EndMarker));

    MarkersToRemove.insert(BeginMarker);
    MarkersToRemove.insert(EndMarker);
  }

  // Pass 2: erase markers that participated in a validated region. Unmatched
  // markers are deliberately left alone — removing them silently would hide
  // pairing bugs from downstream passes.
  for (Instruction *Marker : MarkersToRemove) {
    if (!Marker->getParent())
      continue;
    if (!Marker->user_empty()) {
      Value *UndefVal = UndefValue::get(Marker->getType());
      Marker->replaceAllUsesWith(UndefVal);
    }
    Marker->eraseFromParent();
  }

  return true;
}

/// Sets up runtime function declarations.
void setupRuntimeFunctions(Module &M, FunctionCallee &RecordStartFn,
                            FunctionCallee &StartMeasureFn,
                            FunctionCallee &EndMeasureFn,
                            FunctionCallee &PrintStatsFn) {
  LLVMContext &Ctx = M.getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *Int64Ty = Type::getInt64Ty(Ctx);

  RecordStartFn = M.getOrInsertFunction(PROGRAM_START_FN, VoidTy);
  StartMeasureFn = M.getOrInsertFunction(START_MEASUREMENT_FN,
                                         FunctionType::get(Int64Ty, {}, false));
  EndMeasureFn = M.getOrInsertFunction(END_MEASUREMENT_FN,
                                       FunctionType::get(VoidTy, {Int64Ty}, false));
  PrintStatsFn = M.getOrInsertFunction(PRINT_STATS_FN, VoidTy);
}

/// Sets up module-level hooks (constructors and destructors).
void setupModuleHooks(Module &M, FunctionCallee RecordStartFn,
                      FunctionCallee PrintStatsFn) {
  // Create global constructor to initialize program tracking
  Function *Ctor = Function::Create(
      FunctionType::get(Type::getVoidTy(M.getContext()), false),
      GlobalValue::InternalLinkage, "cpu_cycle_ctor", &M);
  BasicBlock *BB = BasicBlock::Create(M.getContext(), "entry", Ctor);
  IRBuilder<> Builder(BB);
  Builder.CreateCall(RecordStartFn);
  Builder.CreateRetVoid();
  appendToGlobalCtors(M, Ctor, 0);

  // Register destructor to print statistics at program exit
  if (Function *PrintStatsFunc = dyn_cast<Function>(PrintStatsFn.getCallee()))
    appendToGlobalDtors(M, PrintStatsFunc, 0);
}

} // namespace

PreservedAnalyses CpuCycleCountPass::run(Module &M, ModuleAnalysisManager &AM) {
  if (!isPrimaryPackage())
    return PreservedAnalyses::all();

  // Setup runtime function declarations
  FunctionCallee RecordStartFn, StartMeasureFn, EndMeasureFn, PrintStatsFn;
  setupRuntimeFunctions(M, RecordStartFn, StartMeasureFn, EndMeasureFn, PrintStatsFn);

  // Setup module-level hooks (ctors/dtors)
  setupModuleHooks(M, RecordStartFn, PrintStatsFn);

  // Instrument unsafe blocks in all non-declaration functions
  bool Modified = false;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;

    if (instrumentUnsafeBlocks(F, StartMeasureFn, EndMeasureFn))
      Modified = true;
  }

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
