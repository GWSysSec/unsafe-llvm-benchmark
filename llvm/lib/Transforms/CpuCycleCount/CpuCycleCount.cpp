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

#include "llvm/Transforms/CpuCycleCount/CpuCycleCount.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <cstdlib>
#include <cstring>

using namespace llvm;

// Runtime function names
const char *llvm::PROGRAM_START_FN = "record_program_start";
const char *llvm::START_MEASUREMENT_FN = "cpu_cycle_start_measurement";
const char *llvm::END_MEASUREMENT_FN = "cpu_cycle_end_measurement";
const char *llvm::PRINT_STATS_FN = "print_cpu_cycle_stats";

namespace {

static bool isPrimaryPackage() {
  const char *P = getenv("CARGO_PRIMARY_PACKAGE");
  return P && strcmp(P, "1") == 0;
}

/// Instruments unsafe blocks within a function to measure CPU cycles.
/// Uses a three-pass strategy to avoid iterator invalidation:
/// 1. Collect begin/end marker pairs
/// 2. Insert instrumentation calls with memory fences
/// 3. Remove markers
bool instrumentUnsafeBlocks(Function &F, FunctionCallee StartFn,
                             FunctionCallee EndFn) {
  SmallVector<std::pair<Instruction *, Instruction *>, 16> InstrumentationPairs;
  SmallVector<Instruction *, 16> MarkersToRemove;

  // First pass: collect all markers and instrumentation points
  for (BasicBlock &BB : F) {
    Instruction *CurrentBeginMarker = nullptr;

    for (Instruction &I : BB) {
      auto *Call = dyn_cast<CallBase>(&I);
      if (!Call)
        continue;

      auto *IA = dyn_cast<InlineAsm>(Call->getCalledOperand());
      if (!IA)
        continue;

      StringRef Asm = IA->getAsmString();
      if (Asm == llvm::UNSAFE_MARKER_BEGIN) {
        if (!CurrentBeginMarker)
          CurrentBeginMarker = &I;
      } else if (Asm == llvm::UNSAFE_MARKER_END && CurrentBeginMarker) {
        InstrumentationPairs.push_back({CurrentBeginMarker, &I});
        MarkersToRemove.push_back(CurrentBeginMarker);
        MarkersToRemove.push_back(&I);
        CurrentBeginMarker = nullptr;
      }
    }
  }

  if (InstrumentationPairs.empty())
    return false;

  // Second pass: insert instrumentation while markers are still valid
  for (auto [BeginMarker, EndMarker] : InstrumentationPairs) {
    IRBuilder<> BeginBuilder(BeginMarker);
    BeginBuilder.CreateFence(AtomicOrdering::SequentiallyConsistent);
    Value *StartCycleValue = BeginBuilder.CreateCall(StartFn);

    IRBuilder<> EndBuilder(EndMarker);
    EndBuilder.CreateFence(AtomicOrdering::SequentiallyConsistent);
    EndBuilder.CreateCall(EndFn, {StartCycleValue});
  }

  // Third pass: safely remove all markers after instrumentation
  for (Instruction *Marker : MarkersToRemove) {
    if (Marker->getParent()) {
      if (!Marker->user_empty()) {
        Value *UndefVal = UndefValue::get(Marker->getType());
        Marker->replaceAllUsesWith(UndefVal);
      }
      Marker->eraseFromParent();
    }
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
