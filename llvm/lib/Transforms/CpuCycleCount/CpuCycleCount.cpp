//===-- CpuCycleCount.cpp - Track unsafe instruction execution time -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===-------------------------------------------------------------------------------------===//
///
/// \file
/// This file implements the CpuCycleCount pass for tracking unsafe instruction
/// execution time with thread creation instrumentation.
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

// Define the names of ALL C functions in our Rust runtime
const char *llvm::PROGRAM_START_FN = "record_program_start";
const char *llvm::THREAD_START_FN = "record_thread_start";
const char *llvm::START_MEASUREMENT_FN = "cpu_cycle_start_measurement";
const char *llvm::END_MEASUREMENT_FN = "cpu_cycle_end_measurement";
const char *llvm::PRINT_STATS_FN = "print_cpu_cycle_stats";
// **New runtime function names for external calls**
const char *llvm::EXTERNAL_CALL_START_FN = "external_call_start";
const char *llvm::EXTERNAL_CALL_END_FN = "external_call_end";

namespace {

static bool isPrimaryPackage() {
  const char *P = getenv("CARGO_PRIMARY_PACKAGE");
  return P && strcmp(P, "1") == 0;
}

// Instruments a single function to measure unsafe blocks, adding memory fences for accuracy.
bool instrumentUnsafeBlocks(Function &F, FunctionCallee StartFn, FunctionCallee EndFn) {
    bool Modified = false;
    SmallVector<std::pair<Instruction*, Instruction*>, 16> InstrumentationPairs;
    SmallVector<Instruction*, 16> MarkersToRemove;

    // First pass: collect all markers and instrumentation points - NO IR modification
    for (BasicBlock &BB : F) {
        Instruction* CurrentBeginMarker = nullptr;

        for (Instruction &I : BB) {
            auto *Call = dyn_cast<CallBase>(&I);
            if (!Call) continue;
            auto *IA = dyn_cast<InlineAsm>(Call->getCalledOperand());
            if (!IA) continue;

            StringRef Asm = IA->getAsmString();
            if (Asm == llvm::UNSAFE_MARKER_BEGIN) {
                if (!CurrentBeginMarker) {
                    CurrentBeginMarker = &I;
                }
            } else if (Asm == llvm::UNSAFE_MARKER_END && CurrentBeginMarker) {
                InstrumentationPairs.push_back({CurrentBeginMarker, &I});
                MarkersToRemove.push_back(CurrentBeginMarker);
                MarkersToRemove.push_back(&I);
                CurrentBeginMarker = nullptr;
            }
        }
    }

    // Second pass: apply all instrumentation while markers are still valid
    for (auto [BeginMarker, EndMarker] : InstrumentationPairs) {
        IRBuilder<> BeginBuilder(BeginMarker);
        BeginBuilder.CreateFence(AtomicOrdering::SequentiallyConsistent);
        Value* StartCycleValue = BeginBuilder.CreateCall(StartFn);

        IRBuilder<> EndBuilder(EndMarker);
        EndBuilder.CreateFence(AtomicOrdering::SequentiallyConsistent);
        EndBuilder.CreateCall(EndFn, {StartCycleValue});
        Modified = true;
    }

    // Third pass: now safely remove all markers after instrumentation is complete
    for (Instruction *Marker : MarkersToRemove) {
        if (Marker->getParent() != nullptr) {
            if (!Marker->user_empty()) {
                Value *UndefVal = UndefValue::get(Marker->getType());
                Marker->replaceAllUsesWith(UndefVal);
            }
            Marker->eraseFromParent();
        }
    }
    return Modified;
}

// Sets up a global constructor to call the program start recorder.
void createProgramStartRecorder(Module &M, FunctionCallee RecordStartFn) {
    Function *Ctor = Function::Create(FunctionType::get(Type::getVoidTy(M.getContext()), false),
                                     GlobalValue::InternalLinkage, "cpu_cycle_ctor", &M);
    BasicBlock *BB = BasicBlock::Create(M.getContext(), "entry", Ctor);
    IRBuilder<> Builder(BB);
    Builder.CreateCall(RecordStartFn);
    Builder.CreateRetVoid();
    appendToGlobalCtors(M, Ctor, 0);
}

} // namespace

bool instrumentExternalCalls(Function &F, FunctionCallee ExtStartFn, FunctionCallee ExtEndFn) {
    bool Modified = false;
    // We need to collect the calls first to avoid iterator invalidation
    SmallVector<Instruction*, 32> CallsToInstrument;

    for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
            if (auto *Call = dyn_cast<CallBase>(&I)) {
                Function *CalledFn = Call->getCalledFunction();
                // Check if the called function is a declaration (external) and not an intrinsic
                if (CalledFn && CalledFn->isDeclaration() && !CalledFn->isIntrinsic()) {
                    // Also, don't instrument our own runtime functions!
                    StringRef Name = CalledFn->getName();
                    if (!Name.startswith("cpu_cycle_") && !Name.startswith("record_") && !Name.startswith("external_")) {
                         CallsToInstrument.push_back(&I);
                    }
                }
            }
        }
    }

    if (CallsToInstrument.empty()) {
        return false;
    }

    for (Instruction *I : CallsToInstrument) {
        // Skip terminator instructions to avoid IR corruption
        if (I->isTerminator()) {
            continue;
        }

        IRBuilder<> Builder(I);
        // Insert timer start before the call
        Builder.CreateFence(AtomicOrdering::SequentiallyConsistent);
        Value *StartTime = Builder.CreateCall(ExtStartFn);

        // The original call instruction remains here

        // Insert timer end after the call - ONLY if there's a clear next instruction
        Instruction *NextInst = I->getNextNonDebugInstruction();
        if (NextInst) {
            // Safe case: insert before next instruction
            IRBuilder<> EndBuilder(NextInst);
            EndBuilder.CreateFence(AtomicOrdering::SequentiallyConsistent);
            EndBuilder.CreateCall(ExtEndFn, {StartTime});
        }
        // Skip instrumentation for calls at end of blocks to avoid IR corruption
        // This means we'll miss some external calls, but ensures compilation succeeds
        Modified = true;
    }

    return Modified;
}

// The main run method is now clean and performs a single pass over the module.
PreservedAnalyses CpuCycleCountPass::run(Module &M, ModuleAnalysisManager &AM) {
    if (!isPrimaryPackage())
        return PreservedAnalyses::all();
        
    LLVMContext &Ctx = M.getContext();
    Type *VoidTy = Type::getVoidTy(Ctx);
    Type *Int64Ty = Type::getInt64Ty(Ctx);

    // Get declarations for ALL runtime functions
    FunctionCallee RecordStartFn = M.getOrInsertFunction(PROGRAM_START_FN, VoidTy);
    FunctionCallee StartMeasureFn = M.getOrInsertFunction(START_MEASUREMENT_FN,
        FunctionType::get(Int64Ty, {}, false));
    FunctionCallee EndMeasureFn = M.getOrInsertFunction(END_MEASUREMENT_FN,
        FunctionType::get(VoidTy, {Int64Ty}, false));
    FunctionCallee PrintStatsFn = M.getOrInsertFunction(PRINT_STATS_FN, VoidTy);
    // **Get declarations for the new external call hooks**
    FunctionCallee ExtStartFn = M.getOrInsertFunction(EXTERNAL_CALL_START_FN,
        FunctionType::get(Int64Ty, {}, false));
    FunctionCallee ExtEndFn = M.getOrInsertFunction(EXTERNAL_CALL_END_FN,
        FunctionType::get(VoidTy, {Int64Ty}, false));

    // Set up module-level hooks
    createProgramStartRecorder(M, RecordStartFn);
    if (Function *PrintStatsFunc = dyn_cast<Function>(PrintStatsFn.getCallee())) {
        appendToGlobalDtors(M, PrintStatsFunc, 0);
    }

    bool Modified = false;
    
    for (Function &F : M) {
        if (F.isDeclaration() || F.getName().startswith("cpu_cycle_")) continue;
        
        // Instrument unsafe blocks as before
        if (instrumentUnsafeBlocks(F, StartMeasureFn, EndMeasureFn)) {
            Modified = true;
        }
        
        // **Also instrument external calls within this function**
        if (instrumentExternalCalls(F, ExtStartFn, ExtEndFn)) {
            Modified = true;
        }
    }

    return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
