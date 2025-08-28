//===-- DynamicLineCount.cpp - Track unsafe source line coverage -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements the DynamicLineCount pass for tracking unsafe
/// source line coverage using a two-phase approach:
/// Phase 1: Registration - collect unique unsafe lines, generate constructor
/// Phase 2: Execution - insert tracking calls at unsafe instructions
///
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/DynamicLineCount/DynamicLineCount.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <functional>

using namespace llvm;

const char *REGISTER_UNSAFE_LINE_FN = "register_unsafe_line";
const char *TRACK_UNSAFE_LINE_EXECUTION_FN = "track_unsafe_line_execution";
const char *PRINT_UNSAFE_COVERAGE_STATS_FN = "print_unsafe_coverage_stats";

namespace {


/// \brief Setup runtime functions for unsafe line coverage tracking.
static void setupRuntimeFunctions(Module &M,
                                  FunctionCallee &RegisterLineFn,
                                  FunctionCallee &TrackExecutionFn,
                                  FunctionCallee &PrintStatsFn) {
  LLVMContext &Ctx = M.getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *Int32Ty = Type::getInt32Ty(Ctx);
  Type *Int64Ty = Type::getInt64Ty(Ctx);
  Type *Int8PtrTy = PointerType::getUnqual(Type::getInt8Ty(Ctx));

  // register_unsafe_line(line_id, line, file)
  FunctionType *RegisterLineFnTy = FunctionType::get(VoidTy, {Int32Ty, Int64Ty, Int8PtrTy}, false);
  RegisterLineFn = M.getOrInsertFunction(REGISTER_UNSAFE_LINE_FN, RegisterLineFnTy);

  // track_unsafe_line_execution(line_id, line, file)
  FunctionType *TrackExecutionFnTy = FunctionType::get(VoidTy, {Int32Ty, Int64Ty, Int8PtrTy}, false);
  TrackExecutionFn = M.getOrInsertFunction(TRACK_UNSAFE_LINE_EXECUTION_FN, TrackExecutionFnTy);

  FunctionType *PrintFnTy = FunctionType::get(VoidTy, false);
  PrintStatsFn = M.getOrInsertFunction(PRINT_UNSAFE_COVERAGE_STATS_FN, PrintFnTy);

  // Register stats printer once per module
  static bool DestructorRegistered = false;
  if (!DestructorRegistered) {
    if (auto *F = dyn_cast<Function>(PrintStatsFn.getCallee()))
      appendToGlobalDtors(M, F, 0);
    DestructorRegistered = true;
  }
}

/// \brief Creates a global string constant for the given string value.
static Value *createGlobalString(IRBuilder<> &Builder, StringRef Str) {
  Module *M = Builder.GetInsertBlock()->getParent()->getParent();
  return Builder.CreateGlobalStringPtr(Str);
}

/// \brief Return true if instruction is a marker, and set isBegin/isEnd accordingly.
static bool isMarkerInstruction(const Instruction &I, bool &isBegin, bool &isEnd) {
  if (const CallBase *CallInst = dyn_cast<CallBase>(&I)) {
    // Need to strip pointer casts to get to the actual InlineAsm
    if (const llvm::InlineAsm *InlineAsm = dyn_cast<llvm::InlineAsm>(CallInst->getCalledOperand()->stripPointerCasts())) {
      StringRef AsmStr = InlineAsm->getAsmString();
      if (AsmStr == llvm::UNSAFE_MARKER_BEGIN) { isBegin = true; return true; }
      if (AsmStr == llvm::UNSAFE_MARKER_END)   { isEnd = true; return true; }
    }
  }
  return false;
}

/// \brief Return true if function should be instrumented.
static bool shouldInstrumentFunction(const Function &F) {
  if (F.isDeclaration() || F.isIntrinsic()) return false;
  StringRef Name = F.getName();
  return Name != REGISTER_UNSAFE_LINE_FN &&
         Name != TRACK_UNSAFE_LINE_EXECUTION_FN &&
         Name != PRINT_UNSAFE_COVERAGE_STATS_FN;
}

/// \brief Basic Block registration + execution tracking
static bool instrumentFunction(Function &F, FunctionCallee RegisterLineFn, FunctionCallee TrackExecutionFn) {
  LLVMContext &Ctx = F.getContext();
  bool Modified = false;

  for (BasicBlock &BB : F) {
    std::unordered_set<std::string> UnsafeLinesInBB;
    bool BBHasUnsafeRegion = false;
    
    // First pass: collect all unsafe lines in this basic block
    bool insideUnsafeRegion = false;
    for (Instruction &I : BB) {
      bool isBegin = false, isEnd = false;
      
      if (isMarkerInstruction(I, isBegin, isEnd)) {
        if (isBegin) {
          insideUnsafeRegion = true;
          BBHasUnsafeRegion = true;
        } else if (isEnd) {
          insideUnsafeRegion = false;
        }
        continue;
      }
      
      if (insideUnsafeRegion && I.getMetadata("unsafe_inst")) {
        if (MDNode *LineInfoMD = I.getMetadata("unsafe_line_info")) {
          if (LineInfoMD->getNumOperands() >= 2) {
            if (auto *LineConst = dyn_cast<ConstantAsMetadata>(LineInfoMD->getOperand(0))) {
              if (auto *FileStr = dyn_cast<MDString>(LineInfoMD->getOperand(1))) {
                unsigned Line = LineConst->getValue()->getUniqueInteger().getZExtValue();
                std::string File = FileStr->getString().str();
                std::string LineKey = File + ":" + std::to_string(Line);
                UnsafeLinesInBB.insert(LineKey);
              }
            }
          }
        }
      }
    }
    
    // Second pass: instrument the basic block
    if (BBHasUnsafeRegion && !UnsafeLinesInBB.empty()) {
      // Register all unsafe lines at BB entry (after PHI nodes)
      Instruction *FirstNonPHI = &*BB.getFirstNonPHI();
      IRBuilder<> EntryBuilder(FirstNonPHI);
      for (const auto &lineKey : UnsafeLinesInBB) {
        size_t colonPos = lineKey.find(':');
        std::string file = lineKey.substr(0, colonPos);
        unsigned line = std::stoul(lineKey.substr(colonPos + 1));
        
        Value *LineIdArg = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
        Value *LineArg = ConstantInt::get(Type::getInt64Ty(Ctx), line);
        Value *FileArg = createGlobalString(EntryBuilder, file);
        EntryBuilder.CreateCall(RegisterLineFn, {LineIdArg, LineArg, FileArg});
      }
      
      // Insert execution tracking at each unsafe instruction
      insideUnsafeRegion = false;
      for (Instruction &I : BB) {
        bool isBegin = false, isEnd = false;
        
        if (isMarkerInstruction(I, isBegin, isEnd)) {
          if (isBegin) insideUnsafeRegion = true;
          else if (isEnd) insideUnsafeRegion = false;
          continue;
        }
        
        if (insideUnsafeRegion && I.getMetadata("unsafe_inst")) {
          if (MDNode *LineInfoMD = I.getMetadata("unsafe_line_info")) {
            if (LineInfoMD->getNumOperands() >= 2) {
              if (auto *LineConst = dyn_cast<ConstantAsMetadata>(LineInfoMD->getOperand(0))) {
                if (auto *FileStr = dyn_cast<MDString>(LineInfoMD->getOperand(1))) {
                  unsigned Line = LineConst->getValue()->getUniqueInteger().getZExtValue();
                  std::string File = FileStr->getString().str();
                  
                  IRBuilder<> Builder(&I);
                  Value *LineIdArg = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
                  Value *LineArg = ConstantInt::get(Type::getInt64Ty(Ctx), Line);
                  Value *FileArg = createGlobalString(Builder, File);
                  Builder.CreateCall(TrackExecutionFn, {LineIdArg, LineArg, FileArg});
                }
              }
            }
          }
        }
      }
      
      Modified = true;
    }
  }
  
  return Modified;
}

} // anonymous namespace

PreservedAnalyses DynamicLineCountPass::run(Function &F, FunctionAnalysisManager &AM) {
  if (!shouldInstrumentFunction(F))
    return PreservedAnalyses::all();

  Module *M = F.getParent();

  FunctionCallee RegisterLineFn, TrackExecutionFn, PrintStatsFn;
  setupRuntimeFunctions(*M, RegisterLineFn, TrackExecutionFn, PrintStatsFn);

  // Basic block registration + execution tracking
  bool Modified = instrumentFunction(F, RegisterLineFn, TrackExecutionFn);

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
