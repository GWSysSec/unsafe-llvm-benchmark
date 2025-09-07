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
/// execution time.
///
//===--------------------------------------------------------------------------------------==//

#include "llvm/Transforms/CpuCycleCount/CpuCycleCount.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace llvm;

const char *llvm::PROGRAM_START_FN = "record_program_start";
const char *llvm::START_MEASUREMENT_FN = "cpu_cycle_start_measurement";
const char *llvm::END_MEASUREMENT_FN = "cpu_cycle_end_measurement";
const char *llvm::PRINT_STATS_FN = "print_cpu_cycle_stats";

namespace {

static bool isPrimaryPackage() {
  const char *P = getenv("CARGO_PRIMARY_PACKAGE");
  return P && strcmp(P, "1") == 0;
}

bool instrumentUnsafeBlocks(Function &F, FunctionCallee StartFn, FunctionCallee EndFn) {
    bool Modified = false;
    std::vector<Instruction*> AllMarkers;
    
    // Process each basic block independently
    for (BasicBlock &BB : F) {
        // Look for begin/end pairs ONLY within this basic block
        std::vector<std::pair<Instruction*, Instruction*>> PairsToInstrument;
        Instruction *CurrentBegin = nullptr;
        
        for (Instruction &I : BB) {
            if (auto *Call = dyn_cast<CallBase>(&I)) {
                if (auto *IA = dyn_cast<InlineAsm>(Call->getCalledOperand())) {
                    StringRef Asm = IA->getAsmString();
                    
                    if (Asm == llvm::UNSAFE_MARKER_BEGIN) {
                        CurrentBegin = &I;
                        AllMarkers.push_back(&I);
                    } else if (Asm == llvm::UNSAFE_MARKER_END) {
                        AllMarkers.push_back(&I);
                        
                        // Only create a pair if we have a begin in THE SAME BB
                        if (CurrentBegin) {
                            PairsToInstrument.push_back({CurrentBegin, &I});
                            CurrentBegin = nullptr;  // Reset for next potential pair
                        }
                    }
                }
            }
        }
        
        // Instrument the pairs we found in this BB
        for (const auto &Pair : PairsToInstrument) {
            IRBuilder<> BeginBuilder(Pair.first);
            Value *Start = BeginBuilder.CreateCall(StartFn);
            
            IRBuilder<> EndBuilder(Pair.second);
            EndBuilder.CreateCall(EndFn, {Start});
            
            Modified = true;
        }
    }
    
    // Safely remove markers with validation
    for (Instruction *Marker : AllMarkers) {
        // Safety checks before removal
        if (!Marker->use_empty() || Marker->isTerminator() || !Marker->getParent()) {
            continue;  // Skip problematic markers
        }
        
        // Verify it's still a marker
        if (auto *Call = dyn_cast<CallBase>(Marker)) {
            if (auto *IA = dyn_cast<InlineAsm>(Call->getCalledOperand())) {
                StringRef Asm = IA->getAsmString();
                if (Asm == llvm::UNSAFE_MARKER_BEGIN || Asm == llvm::UNSAFE_MARKER_END) {
                    Marker->eraseFromParent();
                }
            }
        }
    }
    
    return Modified;
}

// Create module constructor to record program start
void createProgramStartRecorder(Module &M, FunctionCallee RecordStartFn) {
    LLVMContext &Ctx = M.getContext();
    
    FunctionType *CtorTy = FunctionType::get(Type::getVoidTy(Ctx), false);
    Function *Ctor = Function::Create(CtorTy, GlobalValue::InternalLinkage,
                                     "cpu_cycle_ctor", &M);
    
    BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Ctor);
    IRBuilder<> Builder(BB);
    Builder.CreateCall(RecordStartFn);
    Builder.CreateRetVoid();
    
    // Priority 0 ensures this runs before main
    appendToGlobalCtors(M, Ctor, 0);
}

} // namespace

PreservedAnalyses CpuCycleCountPass::run(Module &M, ModuleAnalysisManager &AM) {
    if (!isPrimaryPackage())
        return PreservedAnalyses::all();
        
    LLVMContext &Ctx = M.getContext();
    Type *VoidTy = Type::getVoidTy(Ctx);
    Type *Int64Ty = Type::getInt64Ty(Ctx);

    // Setup runtime functions
    FunctionCallee RecordStartFn = M.getOrInsertFunction(
        PROGRAM_START_FN, FunctionType::get(VoidTy, false));
    FunctionCallee StartMeasureFn = M.getOrInsertFunction(
        START_MEASUREMENT_FN, FunctionType::get(Int64Ty, false));
    FunctionCallee EndMeasureFn = M.getOrInsertFunction(
        END_MEASUREMENT_FN, FunctionType::get(VoidTy, {Int64Ty}, false));
    FunctionCallee PrintStatsFn = M.getOrInsertFunction(
        PRINT_STATS_FN, FunctionType::get(VoidTy, false));

    bool Modified = false;
    
    // Create module constructor to record program start TSC
    createProgramStartRecorder(M, RecordStartFn);
    Modified = true;
    
    // Add stats printing to destructors
    appendToGlobalDtors(M, cast<Function>(PrintStatsFn.getCallee()), 0);
    
    // Instrument functions
    for (Function &F : M) {
        if (F.isDeclaration()) continue;
        
        // Skip runtime functions
        StringRef Name = F.getName();
        if (Name == PROGRAM_START_FN || Name == START_MEASUREMENT_FN ||
            Name == END_MEASUREMENT_FN || Name == PRINT_STATS_FN ||
            Name == "cpu_cycle_ctor") {
            continue;
        }
        
        if (instrumentUnsafeBlocks(F, StartMeasureFn, EndMeasureFn)) {
            Modified = true;
        }
    }

    return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
