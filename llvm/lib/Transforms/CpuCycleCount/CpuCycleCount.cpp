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
const char *llvm::THREAD_START_FN = "record_thread_start";
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

// Instrument thread creation points
bool instrumentThreadCreation(Module &M, Function &F, FunctionCallee ThreadStartFn) {
    bool Modified = false;
    
    for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
            if (auto *Call = dyn_cast<CallBase>(&I)) {
                if (!Call->getCalledFunction()) continue;
                
                StringRef FnName = Call->getCalledFunction()->getName();
                
                // Detect std::thread constructor or pthread_create
                // Note: std::thread in Rust typically goes through std::sys::unix::thread::Thread::new
                // which eventually calls pthread_create
                if (FnName.contains("pthread_create") || 
                    FnName.contains("_ZNSt6thread") ||  // std::thread C++ mangling
                    FnName.contains("thread") && FnName.contains("spawn") || // Rust thread::spawn
                    FnName.contains("std") && FnName.contains("thread") && FnName.contains("Thread")) {
                    
                    // For pthread_create: 3rd argument is the thread function
                    // We need to wrap it
                    if (FnName.contains("pthread_create") && Call->getNumOperands() >= 4) {
                        Value *ThreadFn = Call->getOperand(2); // 3rd arg is thread function
                        
                        // Create wrapper function that calls record_thread_start first
                        if (Function *OrigFn = dyn_cast<Function>(ThreadFn->stripPointerCasts())) {
                            // Create wrapper
                            FunctionType *WrapperTy = OrigFn->getFunctionType();
                            Function *Wrapper = Function::Create(
                                WrapperTy, 
                                GlobalValue::InternalLinkage,
                                OrigFn->getName() + "_thread_wrapper", 
                                &M
                            );
                            
                            BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", Wrapper);
                            IRBuilder<> Builder(Entry);
                            
                            // Call record_thread_start at thread entry
                            Builder.CreateCall(ThreadStartFn);
                            
                            // Call original function with all arguments
                            std::vector<Value*> Args;
                            for (auto &Arg : Wrapper->args()) {
                                Args.push_back(&Arg);
                            }
                            Value *Result = Builder.CreateCall(OrigFn, Args);
                            
                            // Return result
                            if (WrapperTy->getReturnType()->isVoidTy()) {
                                Builder.CreateRetVoid();
                            } else {
                                Builder.CreateRet(Result);
                            }
                            
                            // Replace thread function with wrapper
                            Call->setOperand(2, Wrapper);
                            Modified = true;
                        }
                    }
                    // For Rust thread::spawn, instrument the closure/function being spawned
                    // This is trickier as it's often inlined or using trait objects
                    else if (FnName.contains("spawn")) {
                        // After the spawn call, the new thread should start
                        // We can't easily intercept the closure, but we can ensure
                        // the spawned thread will hit record_thread_start on first unsafe block
                        // This is handled by the runtime fallback
                    }
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

// For Rust: Instrument common thread entry points
bool instrumentRustThreadEntry(Module &M, FunctionCallee ThreadStartFn) {
    bool Modified = false;
    
    for (Function &F : M) {
        if (F.isDeclaration()) continue;
        
        StringRef Name = F.getName();
        
        // Common Rust thread entry patterns
        // These are functions that typically start new threads
        if (Name.contains("thread_start") ||
            Name.contains("thread_main") ||
            Name.contains("spawn_unchecked") ||
            (Name.contains("closure") && Name.contains("thread")) ||
            Name.startswith("_ZN3std6thread")) {
            
            // Insert record_thread_start at function entry
            BasicBlock &Entry = F.getEntryBlock();
            Instruction *FirstInst = &*Entry.getFirstInsertionPt();
            IRBuilder<> Builder(FirstInst);
            Builder.CreateCall(ThreadStartFn);
            Modified = true;
        }
    }
    
    return Modified;
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
    FunctionCallee ThreadStartFn = M.getOrInsertFunction(
        THREAD_START_FN, FunctionType::get(VoidTy, false));
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
    
    // First pass: Instrument thread creation points
    for (Function &F : M) {
        if (F.isDeclaration()) continue;
        
        if (instrumentThreadCreation(M, F, ThreadStartFn)) {
            Modified = true;
        }
    }
    
    // Instrument Rust-specific thread entry points
    if (instrumentRustThreadEntry(M, ThreadStartFn)) {
        Modified = true;
    }
    
    // Second pass: Instrument unsafe blocks
    for (Function &F : M) {
        if (F.isDeclaration()) continue;
        
        // Skip runtime functions
        StringRef Name = F.getName();
        if (Name == PROGRAM_START_FN || Name == THREAD_START_FN ||
            Name == START_MEASUREMENT_FN || Name == END_MEASUREMENT_FN || 
            Name == PRINT_STATS_FN || Name == "cpu_cycle_ctor" ||
            Name.contains("_thread_wrapper")) {
            continue;
        }
        
        if (instrumentUnsafeBlocks(F, StartMeasureFn, EndMeasureFn)) {
            Modified = true;
        }
    }

    return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
