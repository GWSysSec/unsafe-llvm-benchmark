//===--- CpuCycleCount.cpp - Measure CPU cycles in unsafe code ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// CpuCycleCount builds upon InstMarker to provide CPU cycle measurement:
// 1. Scans for marker_begin/marker_end inline assembly calls
// 2. Inserts RDTSCP-based cycle measurement around these regions
// 3. Accumulates cycle counts in thread-safe global variables
// 4. Generates performance reports showing CPU cycle consumption
// 5. Supports primary package filtering with CARGO_PRIMARY_PACKAGE=1
//
// The runtime library tracks total cycles and block counts, reporting
// performance statistics at program exit including average cycles per block.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/CpuCycleCount/CpuCycleCount.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <string>
#include <vector>

using namespace llvm;

PreservedAnalyses CpuCycleCountPass::run(Module &M, ModuleAnalysisManager &AM) {
  LLVMContext &Ctx = M.getContext();
  bool Modified = false;

  // Debug message to confirm pass is running
  errs() << "[CpuCycleCount] Pass started for module: " << M.getName() << "\n";

  // Primary package filtering is handled by InstMarker when it inserts markers
  // CpuCycleCount only instruments where markers already exist, so no additional filtering needed

  // Prepare runtime function prototypes
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *Int64Ty = Type::getInt64Ty(Ctx);
  
  // CPU cycle measurement functions
  FunctionType *StartMeasureFnTy = FunctionType::get(Int64Ty, false);
  FunctionCallee StartMeasureFn = M.getOrInsertFunction(CPU_CYCLE_START_FN, StartMeasureFnTy);
  
  FunctionType *EndMeasureFnTy = FunctionType::get(VoidTy, {Int64Ty}, false);
  FunctionCallee EndMeasureFn = M.getOrInsertFunction(CPU_CYCLE_END_FN, EndMeasureFnTy);
  
  FunctionType *StatsFnTy = FunctionType::get(VoidTy, false);
  FunctionCallee StatsFn = M.getOrInsertFunction(CPU_CYCLE_STATS_FN, StatsFnTy);
  
  // Program cycle tracking functions
  FunctionType *ProgramStartFnTy = FunctionType::get(VoidTy, false);
  FunctionCallee ProgramStartFn = M.getOrInsertFunction("cpu_cycle_program_start", ProgramStartFnTy);
  
  FunctionType *ProgramEndFnTy = FunctionType::get(VoidTy, false);
  FunctionCallee ProgramEndFn = M.getOrInsertFunction("cpu_cycle_program_end", ProgramEndFnTy);
  
  // Set function attributes for runtime calls
  for (auto *RuntimeFn : {
      dyn_cast<Function>(StartMeasureFn.getCallee()),
      dyn_cast<Function>(EndMeasureFn.getCallee()),
      dyn_cast<Function>(StatsFn.getCallee()),
      dyn_cast<Function>(ProgramStartFn.getCallee()),
      dyn_cast<Function>(ProgramEndFn.getCallee())}) {
    if (RuntimeFn) {
      RuntimeFn->removeFnAttr(Attribute::ReadNone);
      RuntimeFn->removeFnAttr(Attribute::ReadOnly);
      RuntimeFn->addFnAttr(Attribute::NoInline);
      RuntimeFn->setLinkage(GlobalValue::ExternalLinkage);
    }
  }

  // Add program cycle tracking and statistics reporting for main function
  if (Function *MainFn = M.getFunction("main")) {
    // Add program start measurement at function entry
    if (!MainFn->empty()) {
      BasicBlock &EntryBB = MainFn->getEntryBlock();
      if (!EntryBB.empty()) {
        IRBuilder<> EntryBuilder(&EntryBB.front());
        EntryBuilder.CreateCall(ProgramStartFn);
        Modified = true;
      }
    }
    
    // Add program end measurement and statistics at function exit
    for (BasicBlock &BB : *MainFn) {
      for (Instruction &I : BB) {
        if (auto *RetInst = dyn_cast<ReturnInst>(&I)) {
          IRBuilder<> Builder(RetInst);
          Builder.CreateCall(ProgramEndFn);
          Builder.CreateCall(StatsFn);
          Modified = true;
          break;
        }
      }
    }
  }

  int instrumentedBlocks = 0;
  
  // Process each function to find and instrument unsafe blocks
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
      
    // Skip LLVM intrinsic and runtime functions
    std::string FnName = F.getName().str();
    if (FnName.find("llvm.") == 0 || 
        FnName.find("__") == 0 || 
        FnName.find("cpu_cycle_") == 0 ||
        FnName == "main" ||
        FnName.find("_ZN") == 0 && (
           FnName.find("_ZN9__dynamic") == 0 ||
           FnName.find("_ZN4core") == 0 || 
           FnName.find("_ZN3std") == 0)) {
      continue;
    }
    
    // Process each basic block to find marker patterns
    for (BasicBlock &BB : F) {
      Instruction *MarkerBegin = nullptr;
      Instruction *MarkerEnd = nullptr;
      
      // Scan for marker_begin and marker_end inline assembly
      for (Instruction &I : BB) {
        if (auto *CallInst = dyn_cast<CallBase>(&I)) {
          if (auto *InlineAsmCall = dyn_cast<InlineAsm>(CallInst->getCalledOperand()->stripPointerCasts())) {
            StringRef AsmStr = InlineAsmCall->getAsmString();
            
            if (AsmStr.contains("marker_begin")) {
              MarkerBegin = &I;
            } else if (AsmStr.contains("marker_end")) {
              MarkerEnd = &I;
              
              // If we have both markers, instrument this unsafe block
              if (MarkerBegin) {
                // Insert cycle measurement start after marker_begin
                IRBuilder<> StartBuilder(MarkerBegin->getNextNode() ? 
                                       MarkerBegin->getNextNode() : 
                                       MarkerBegin);
                Value *StartCycles = StartBuilder.CreateCall(StartMeasureFn, {}, "start_cycles");
                
                // Insert cycle measurement end before marker_end
                IRBuilder<> EndBuilder(MarkerEnd);
                EndBuilder.CreateCall(EndMeasureFn, {StartCycles});
                
                instrumentedBlocks++;
                Modified = true;
                
                // Reset for next potential block in same BB
                MarkerBegin = nullptr;
              }
            }
          }
        }
      }
    }
  }
  
  // Output summary
  errs() << "[CpuCycleCount] Instrumented " << instrumentedBlocks 
         << " unsafe blocks for cycle measurement\n";
  
  if (Function *MainFn = M.getFunction("main")) {
    errs() << "[CpuCycleCount] Added program cycle tracking to main function\n";
  }
  
  errs() << "[CpuCycleCount] Pass completed. Modified=" << (Modified ? "true" : "false") << "\n";
  
  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}