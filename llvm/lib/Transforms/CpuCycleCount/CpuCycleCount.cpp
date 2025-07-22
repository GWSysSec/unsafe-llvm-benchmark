//===-- CpuCycleCount.cpp - Track unsafe instruction execution time -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements the CpuCycleCount pass for tracking unsafe instruction
/// execution time.
///
//===----------------------------------------------------------------------==//

#include "llvm/Transforms/CpuCycleCount/CpuCycleCount.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

// These constants are defined to match the header declarations.
const char *llvm::START_MEASUREMENT_FN = "cpu_cycle_start_measurement";
const char *llvm::END_MEASUREMENT_FN   = "cpu_cycle_end_measurement";
const char *llvm::CPU_CYCLE_PRINT_STATS_FN = "print_cpu_cycle_stats";
const char *llvm::TOUCH_TRACKER_FN     = "touch_thread_tracker";

namespace {

/// \brief Instruments unsafe blocks marked by InstMarkerPass to measure CPU cycles.
/// \param F The target function.
/// \param StartFn The function to call at the beginning of an unsafe block.
/// \param EndFn The function to call at the end of an unsafe block.
/// \returns True if the function was modified, false otherwise.
bool instrumentUnsafeBlocks(Function &F, FunctionCallee StartFn, FunctionCallee EndFn) {
    bool Modified = false;
    LLVMContext &Ctx = F.getContext();

    for (BasicBlock &BB : F) {
        Instruction *ActiveMarkerBegin = nullptr;

        for (Instruction &I : BB) {
            if (auto *CallInst = dyn_cast<CallBase>(&I)) {
                if (auto *InlineAsmCall = dyn_cast<InlineAsm>(CallInst->getCalledOperand()->stripPointerCasts())) {
                    StringRef AsmStr = InlineAsmCall->getAsmString();

                    if (AsmStr == UNSAFE_MARKER_BEGIN) {
                        ActiveMarkerBegin = &I;
                    } else if (AsmStr == UNSAFE_MARKER_END) {
                        if (ActiveMarkerBegin) {
                            IRBuilder<> StartBuilder(ActiveMarkerBegin->getNextNode());
                            Value *StartCycles = StartBuilder.CreateCall(StartFn, {});

                            IRBuilder<> EndBuilder(&I);
                            EndBuilder.CreateCall(EndFn, {StartCycles});

                            ActiveMarkerBegin = nullptr;
                            Modified = true;
                        }
                    }
                }
            }
        }
    }
    return Modified;
}

} // anonymous namespace

PreservedAnalyses CpuCycleCountPass::run(Module &M, ModuleAnalysisManager &AM) {
    LLVMContext &Ctx = M.getContext();
    Type *VoidTy = Type::getVoidTy(Ctx);
    Type *Int64Ty = Type::getInt64Ty(Ctx);

    FunctionCallee TouchTrackerFn = M.getOrInsertFunction(
        TOUCH_TRACKER_FN, FunctionType::get(VoidTy, false));
    FunctionCallee StartMeasureFn = M.getOrInsertFunction(
        START_MEASUREMENT_FN, FunctionType::get(Int64Ty, false));
    FunctionCallee EndMeasureFn = M.getOrInsertFunction(
        END_MEASUREMENT_FN, FunctionType::get(VoidTy, {Int64Ty}, false));
    FunctionCallee StatsFn = M.getOrInsertFunction(
        CPU_CYCLE_PRINT_STATS_FN, FunctionType::get(VoidTy, false));

    // Ensure the runtime functions are not inlined and are externally linked
    for (auto *FnHandle : {&TouchTrackerFn, &StartMeasureFn, &EndMeasureFn, &StatsFn}) {
        if (auto *F = dyn_cast<Function>(FnHandle->getCallee())) {
            F->addFnAttr(Attribute::NoInline);
            F->setLinkage(GlobalValue::ExternalLinkage);
        }
    }

    // Register the stats printing function to be called at program exit
    appendToGlobalDtors(M, cast<Function>(StatsFn.getCallee()), 0);

    bool Modified = false;
    for (Function &F : M) {
        // Skip function declarations and our own runtime functions
        if (F.isDeclaration() || F.getName() == TOUCH_TRACKER_FN ||
            F.getName() == START_MEASUREMENT_FN || F.getName() == END_MEASUREMENT_FN ||
            F.getName() == CPU_CYCLE_PRINT_STATS_FN) {
            continue;
        }

        // Instrument the entry of every function to initialize the thread tracker
        IRBuilder<> Builder(&F.getEntryBlock().front());
        Builder.CreateCall(TouchTrackerFn, {});
        Modified = true;

        // Runs the logic to instrument specific unsafe blocks
        if (instrumentUnsafeBlocks(F, StartMeasureFn, EndMeasureFn)) {
            Modified = true;
        }
    }

    return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
