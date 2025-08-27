//===-- UnsafeCount.cpp - Count unsafe instructions (FunctionPass) -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements the UnsafeCount Function pass. The implementation is
/// organized in a single translation unit with small helper functions in an
/// anonymous namespace so it matches the compact LLVM-style you requested:
///
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/UnsafeCount/UnsafeCount.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
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
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Support/Casting.h"
#include <vector>
#include <cstring>
#include <cstdlib>

using namespace llvm;

const char *RECORD_UNSAFE_REGION_FN = "record_unsafe_region";
const char *RECORD_BLOCK_INSTRUCTIONS_FN = "record_block_instructions";
const char *RECORD_FUNCTION_INFO_FN = "record_function_info";
const char *PRINT_UNSAFE_STATS_FN = "print_execution_statistics";

namespace {

/// \brief Checks if the current build is for the primary package.
///
/// This uses the CARGO_PRIMARY_PACKAGE environment variable.
static bool isPrimaryPackage() {
  const char *P = getenv("CARGO_PRIMARY_PACKAGE");
  return P && strcmp(P, "1") == 0;
}

/// \brief Setup simple runtime functions for raw data collection.
/// Much simpler than before - just declares the basic data collection functions.
static void setupRuntimeFunctions(Module &M,
                                  FunctionCallee &RecordUnsafeRegionFn,
                                  FunctionCallee &RecordBlockInstructionsFn,
                                  FunctionCallee &RecordFunctionInfoFn,
                                  FunctionCallee &PrintStatsFn) {
  LLVMContext &Ctx = M.getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *Int64Ty = Type::getInt64Ty(Ctx);
  Type *Int8PtrTy = PointerType::get(Type::getInt8Ty(Ctx), 0);
  Type *Int8Ty = Type::getInt8Ty(Ctx);

  // Simple function signatures for raw data collection
  std::vector<Type*> UnsafeRegionParams(8, Int64Ty);
  RecordUnsafeRegionFn = M.getOrInsertFunction(RECORD_UNSAFE_REGION_FN,
                                               FunctionType::get(VoidTy, UnsafeRegionParams, false));
  RecordBlockInstructionsFn = M.getOrInsertFunction(RECORD_BLOCK_INSTRUCTIONS_FN,
                                                     FunctionType::get(VoidTy, {Int64Ty, Int64Ty}, false));
  RecordFunctionInfoFn = M.getOrInsertFunction(RECORD_FUNCTION_INFO_FN,
                                               FunctionType::get(VoidTy, {Int8PtrTy, Int8Ty}, false));
  PrintStatsFn = M.getOrInsertFunction(PRINT_UNSAFE_STATS_FN,
                                       FunctionType::get(VoidTy, false));

  // Add attributes and external linkage
  for (auto *FnHandle : {&RecordUnsafeRegionFn, &RecordBlockInstructionsFn, &RecordFunctionInfoFn, &PrintStatsFn}) {
    if (auto *F = dyn_cast<Function>(FnHandle->getCallee())) {
      F->addFnAttr(Attribute::NoInline);
      F->setLinkage(GlobalValue::ExternalLinkage);
    }
  }

  // Register stats printer once per-module/program.
  static bool DestructorRegistered = false;
  if (!DestructorRegistered) {
    if (auto *F = dyn_cast<Function>(PrintStatsFn.getCallee()))
      appendToGlobalDtors(M, F, 0);
    DestructorRegistered = true;
  }
}

/// \brief Return true if instruction is a marker, and set isBegin/isEnd accordingly.
static bool isMarkerInstruction(const Instruction &I, bool &isBegin, bool &isEnd) {
  if (auto *CallInst = dyn_cast<CallBase>(&I)) {
    // InlineAsm marker
    if (auto *InlineAsmCall = dyn_cast<InlineAsm>(CallInst->getCalledOperand()->stripPointerCasts())) {
      StringRef AsmStr = InlineAsmCall->getAsmString();
      if (AsmStr == UNSAFE_MARKER_BEGIN) { isBegin = true; return true; }
      if (AsmStr == UNSAFE_MARKER_END)   { isEnd = true; return true; }
    }
  }
  return false;
}

/// \brief Return true if instruction is any kind of marker.
static bool isMarkerInstruction(const Instruction &I) {
  bool isBegin, isEnd;
  return isMarkerInstruction(I, isBegin, isEnd);
}

/// \brief Return true if instruction has unsafe metadata.
static bool hasUnsafeMetadata(const Instruction &I) {
  return I.getMetadata("unsafe_inst");
}

/// \brief Return true if function should be instrumented.
static bool shouldInstrumentFunction(const Function &F) {
  if (F.isDeclaration() || F.isIntrinsic()) return false;
  StringRef Name = F.getName();
  return Name != RECORD_UNSAFE_REGION_FN && Name != RECORD_BLOCK_INSTRUCTIONS_FN &&
         Name != RECORD_FUNCTION_INFO_FN && Name != PRINT_UNSAFE_STATS_FN;
}

/// \brief Simple instrumentation - just collect raw data for runtime processing.
static bool instrumentFunction(Function &F, FunctionCallee RecordUnsafeRegionFn,
                               FunctionCallee RecordBlockInstructionsFn) {
  LLVMContext &Ctx = F.getContext();
  bool Modified = false;

  for (BasicBlock &BB : F) {
    bool insideUnsafeRegion = false;
    uint64_t blockInstCount = 0, markerInstCount = 0;
    long unsafeStats[8] = {0}; // total, loads, stores, adds, geps, subs, allocas, others

    for (Instruction &I : BB) {
      if (isa<DbgInfoIntrinsic>(&I)) continue;
      
      blockInstCount++;
      bool isBegin = false, isEnd = false;
      
      // Check if this is a marker instruction
      if (isMarkerInstruction(I, isBegin, isEnd)) {
        markerInstCount++;
        
        if (isBegin) {
          insideUnsafeRegion = true;
        } else if (isEnd && insideUnsafeRegion) {
          // Simple data collection - let runtime do the smart processing
          IRBuilder<> Builder(&I);
          SmallVector<Value*, 8> Args;
          for (int k = 0; k < 8; ++k)
            Args.push_back(ConstantInt::get(Type::getInt64Ty(Ctx), unsafeStats[k]));
          Builder.CreateCall(RecordUnsafeRegionFn, Args);
          
          std::memset(unsafeStats, 0, sizeof(unsafeStats));
          insideUnsafeRegion = false;
          Modified = true;
        }
        continue;
      }
      
      // Count unsafe instructions inside marked regions ONLY
      if (insideUnsafeRegion) {
        unsafeStats[0]++; // total
        switch (I.getOpcode()) {
          case Instruction::Load:          unsafeStats[1]++; break;
          case Instruction::Store:         unsafeStats[2]++; break;
          case Instruction::Add:           unsafeStats[3]++; break;
          case Instruction::GetElementPtr: unsafeStats[4]++; break;
          case Instruction::Sub:           unsafeStats[5]++; break;
          case Instruction::Alloca:        unsafeStats[6]++; break;
          default:                         unsafeStats[7]++; break;
        }
      }
    }

    // Simple data collection: pass raw counts to runtime for smart processing
    IRBuilder<> Builder(BB.getTerminator());
    Builder.CreateCall(RecordBlockInstructionsFn, {
      ConstantInt::get(Type::getInt64Ty(Ctx), blockInstCount),
      ConstantInt::get(Type::getInt64Ty(Ctx), markerInstCount)
    });
    Modified = true;
  }
  
  return Modified;
}

} // end anonymous namespace

namespace llvm {

bool UnsafeCountPass::isPrimaryPackage() {
  const char *P = getenv("CARGO_PRIMARY_PACKAGE");
  return P && strcmp(P, "1") == 0;
}

PreservedAnalyses UnsafeCountPass::run(Function &F, FunctionAnalysisManager &AM) {
  if (!UnsafeCountPass::isPrimaryPackage())
    return PreservedAnalyses::all();
    
  if (!shouldInstrumentFunction(F))
    return PreservedAnalyses::all();

  Module *M = F.getParent();

  FunctionCallee RecordUnsafeRegionFn, RecordBlockInstructionsFn, RecordFunctionInfoFn, PrintStatsFn;
  setupRuntimeFunctions(*M, RecordUnsafeRegionFn, RecordBlockInstructionsFn, RecordFunctionInfoFn, PrintStatsFn);

  bool Modified = false;
  
  // Early exit for empty functions
  if (F.empty() || F.getEntryBlock().empty()) {
    return PreservedAnalyses::all();
  }

  // Simple function info collection - pass raw data to runtime
  IRBuilder<> EntryBuilder(&F.getEntryBlock().front());
  Value *FuncName = EntryBuilder.CreateGlobalStringPtr(F.getName());
  
  // Simple metadata check - let runtime do the smart classification
  bool hasUnsafeMetadata = false;
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      if (I.getMetadata("unsafe_inst")) {
        hasUnsafeMetadata = true;
        break;
      }
    }
    if (hasUnsafeMetadata) break;
  }
  
  Value *HasUnsafeMetadata = EntryBuilder.getInt8(hasUnsafeMetadata ? 1 : 0);
  EntryBuilder.CreateCall(RecordFunctionInfoFn, {FuncName, HasUnsafeMetadata});
  Modified = true;

  // Simple instrumentation - pass raw data to runtime for smart processing
  if (instrumentFunction(F, RecordUnsafeRegionFn, RecordBlockInstructionsFn))
    Modified = true;

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace llvm
