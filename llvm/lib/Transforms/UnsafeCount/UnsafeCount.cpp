//===-- UnsafeCount.cpp - Count unsafe instructions -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements the UnsafeCount pass for counting unsafe instructions.
///
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/UnsafeCount/UnsafeCount.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

const char *llvm::UPDATE_UNSAFE_COUNTER_FN = "update_counter";
const char *llvm::UPDATE_INSTRUCTION_COUNT_FN = "update_instruction_count";
const char *llvm::RECORD_FUNCTION_EXEC_FN = "record_function_execution";
const char *llvm::PRINT_UNSAFE_STATS_FN = "print_execution_statistics";

//===----------------------------------------------------------------------===//
// UnsafeCountPass Implementation
//===----------------------------------------------------------------------===//

UnsafeCountPass::RuntimeContext UnsafeCountPass::setupRuntimeFunctions(Module &M) {
  LLVMContext &Ctx = M.getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *Int64Ty = Type::getInt64Ty(Ctx);
  Type *Int8PtrTy = PointerType::get(Type::getInt8Ty(Ctx), 0);
  Type *Int1Ty = Type::getInt1Ty(Ctx);

  RuntimeContext RC;
  
  // Setup runtime functions
  std::vector<Type*> UnsafeCounterParams(8, Int64Ty);
  RC.UpdateCounterFn = M.getOrInsertFunction(
      UPDATE_UNSAFE_COUNTER_FN, FunctionType::get(VoidTy, UnsafeCounterParams, false));
  
  RC.UpdateInstCountFn = M.getOrInsertFunction(
      UPDATE_INSTRUCTION_COUNT_FN, FunctionType::get(VoidTy, {Int64Ty}, false));
  
  std::vector<Type*> RecordFuncParams = {Int8PtrTy, Int1Ty};
  RC.RecordFuncFn = M.getOrInsertFunction(
      RECORD_FUNCTION_EXEC_FN, FunctionType::get(VoidTy, RecordFuncParams, false));
  
  RC.PrintStatsFn = M.getOrInsertFunction(
      PRINT_UNSAFE_STATS_FN, FunctionType::get(VoidTy, false));

  // Set function attributes
  for (auto *FnHandle : {&RC.UpdateCounterFn, &RC.UpdateInstCountFn, &RC.RecordFuncFn, &RC.PrintStatsFn}) {
    if (auto *F = dyn_cast<Function>(FnHandle->getCallee())) {
      F->addFnAttr(Attribute::NoInline);
      F->setLinkage(GlobalValue::ExternalLinkage);
    }
  }

  // Register stats printer to run at program exit (one-time setup)
  static bool DestructorRegistered = false;
  if (!DestructorRegistered) {
    appendToGlobalDtors(M, cast<Function>(RC.PrintStatsFn.getCallee()), 0);
    DestructorRegistered = true;
  }

  return RC;
}

bool UnsafeCountPass::shouldInstrumentFunction(const Function &F) {
  if (F.isDeclaration() || F.isIntrinsic()) return false;
  
  StringRef Name = F.getName();
  // Skip our own runtime functions
  return Name != UPDATE_UNSAFE_COUNTER_FN && Name != UPDATE_INSTRUCTION_COUNT_FN &&
         Name != RECORD_FUNCTION_EXEC_FN && Name != PRINT_UNSAFE_STATS_FN;
}

bool UnsafeCountPass::isFunctionUnsafe(const Function &F) {
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      if (auto *CallInst = dyn_cast<CallBase>(&I)) {
        if (auto *InlineAsmCall = dyn_cast<InlineAsm>(CallInst->getCalledOperand()->stripPointerCasts())) {
          if (InlineAsmCall->getAsmString() == UNSAFE_MARKER_BEGIN) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

bool UnsafeCountPass::instrumentUnsafeBlocks(Function &F, const RuntimeContext &Ctx) {
  bool Modified = false;
  LLVMContext &LLVMCtx = F.getContext();
  
  for (BasicBlock &BB : F) {
    Instruction *ActiveMarkerBegin = nullptr;
    uint64_t blockInstCount = 0;
    long unsafeStats[8] = {0}; // total, loads, stores, adds, geps, subs, allocas, others
    
    for (Instruction &I : BB) {
      // Count all non-debug instructions for block counting
      if (!isa<DbgInfoIntrinsic>(&I)) {
        blockInstCount++;
      }
      
      if (auto *CallInst = dyn_cast<CallBase>(&I)) {
        if (auto *InlineAsmCall = dyn_cast<InlineAsm>(CallInst->getCalledOperand()->stripPointerCasts())) {
          StringRef AsmStr = InlineAsmCall->getAsmString();
          
          if (AsmStr == UNSAFE_MARKER_BEGIN) {
            ActiveMarkerBegin = &I;
          } else if (AsmStr == UNSAFE_MARKER_END && ActiveMarkerBegin) {
            // Insert counter update call before end marker
            IRBuilder<> Builder(&I);
            std::vector<Value*> Args;
            for (int i = 0; i < 8; i++) {
              Args.push_back(ConstantInt::get(Type::getInt64Ty(LLVMCtx), unsafeStats[i]));
            }
            Builder.CreateCall(Ctx.UpdateCounterFn, Args);
            
            // Reset counters for next region
            memset(unsafeStats, 0, sizeof(unsafeStats));
            ActiveMarkerBegin = nullptr;
            Modified = true;
          }
        }
      } else if (ActiveMarkerBegin && !isa<DbgInfoIntrinsic>(&I)) {
        // Count unsafe instruction by type
        unsafeStats[0]++; // total
        switch (I.getOpcode()) {
          case Instruction::Load: unsafeStats[1]++; break;
          case Instruction::Store: unsafeStats[2]++; break;
          case Instruction::Add: unsafeStats[3]++; break;
          case Instruction::GetElementPtr: unsafeStats[4]++; break;
          case Instruction::Sub: unsafeStats[5]++; break;
          case Instruction::Alloca: unsafeStats[6]++; break;
          default: unsafeStats[7]++; break;
        }
      }
    }
    
    // Insert instruction count update at end of block
    if (blockInstCount > 0) {
      IRBuilder<> Builder(BB.getTerminator());
      Value *CountValue = ConstantInt::get(Type::getInt64Ty(LLVMCtx), blockInstCount);
      Builder.CreateCall(Ctx.UpdateInstCountFn, CountValue);
      Modified = true;
    }
  }
  
  return Modified;
}

PreservedAnalyses UnsafeCountPass::run(Function &F, FunctionAnalysisManager &AM) {
  if (!shouldInstrumentFunction(F)) {
    return PreservedAnalyses::all();
  }

  Module *M = F.getParent();
  RuntimeContext Ctx = setupRuntimeFunctions(*M);

  bool Modified = false;

  // Record function execution at entry
  IRBuilder<> Builder(&F.getEntryBlock().front());
  Value *FuncName = Builder.CreateGlobalStringPtr(F.getName());
  Value *IsUnsafe = Builder.getInt1(isFunctionUnsafe(F));
  Builder.CreateCall(Ctx.RecordFuncFn, {FuncName, IsUnsafe});
  Modified = true;

  // Instrument unsafe blocks and instruction counting
  if (instrumentUnsafeBlocks(F, Ctx)) {
    Modified = true;
  }

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}