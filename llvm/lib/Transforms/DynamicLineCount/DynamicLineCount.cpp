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
/// Phase 1: Compile-time - Collect all unsafe lines and register via constructor
/// Phase 2: Runtime - Insert tracking calls at unsafe instructions
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
#include <set>
#include <string>
#include <vector>

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
  Type *Int64Ty = Type::getInt64Ty(Ctx);
  Type *Int8PtrTy = PointerType::getUnqual(Type::getInt8Ty(Ctx));

  // register_unsafe_line(line, file)
  FunctionType *RegisterLineFnTy = FunctionType::get(VoidTy, {Int64Ty, Int8PtrTy}, false);
  RegisterLineFn = M.getOrInsertFunction(REGISTER_UNSAFE_LINE_FN, RegisterLineFnTy);

  // track_unsafe_line_execution(line, file)
  FunctionType *TrackExecutionFnTy = FunctionType::get(VoidTy, {Int64Ty, Int8PtrTy}, false);
  TrackExecutionFn = M.getOrInsertFunction(TRACK_UNSAFE_LINE_EXECUTION_FN, TrackExecutionFnTy);

  // print_unsafe_coverage_stats()
  FunctionType *PrintFnTy = FunctionType::get(VoidTy, false);
  PrintStatsFn = M.getOrInsertFunction(PRINT_UNSAFE_COVERAGE_STATS_FN, PrintFnTy);
}

/// \brief Creates a global string constant for the given string value.
static Value *createGlobalString(Module &M, IRBuilder<> &Builder, StringRef Str) {
  return Builder.CreateGlobalStringPtr(Str);
}

/// \brief Return true if instruction is a marker, and set isBegin/isEnd accordingly.
static bool isMarkerInstruction(const Instruction &I, bool &isBegin, bool &isEnd) {
  if (const CallBase *CallInst = dyn_cast<CallBase>(&I)) {
    if (const llvm::InlineAsm *InlineAsm = 
        dyn_cast<llvm::InlineAsm>(CallInst->getCalledOperand()->stripPointerCasts())) {
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
         Name != PRINT_UNSAFE_COVERAGE_STATS_FN &&
         Name != "unsafe_lines_module_ctor" &&
         Name != "unsafe_lines_module_dtor";
}

/// \brief Collect unsafe lines and instrument execution tracking in a function.
static bool collectAndInstrumentFunction(Function &F, 
                                        FunctionCallee TrackExecutionFn,
                                        std::set<std::string> &allUnsafeLines) {
  Module &M = *F.getParent();
  LLVMContext &Ctx = F.getContext();
  bool Modified = false;

  for (BasicBlock &BB : F) {
    bool insideUnsafeRegion = false;
    
    for (Instruction &I : BB) {
      bool isBegin = false, isEnd = false;
      
      // Check for unsafe region markers
      if (isMarkerInstruction(I, isBegin, isEnd)) {
        if (isBegin) insideUnsafeRegion = true;
        else if (isEnd) insideUnsafeRegion = false;
        continue;
      }
      
      // Process unsafe instructions
      if (insideUnsafeRegion && I.getMetadata("unsafe_inst")) {
        if (MDNode *LineInfoMD = I.getMetadata("unsafe_line_info")) {
          if (LineInfoMD->getNumOperands() >= 2) {
            if (auto *LineConst = dyn_cast<ConstantAsMetadata>(LineInfoMD->getOperand(0))) {
              if (auto *FileStr = dyn_cast<MDString>(LineInfoMD->getOperand(1))) {
                unsigned Line = LineConst->getValue()->getUniqueInteger().getZExtValue();
                std::string File = FileStr->getString().str();
                std::string LineKey = File + ":" + std::to_string(Line);
                
                // Add to global collection for compile-time registration
                allUnsafeLines.insert(LineKey);
                
                // Insert runtime execution tracking
                IRBuilder<> Builder(&I);
                Value *LineArg = ConstantInt::get(Type::getInt64Ty(Ctx), Line);
                Value *FileArg = createGlobalString(M, Builder, File);
                Builder.CreateCall(TrackExecutionFn, {LineArg, FileArg});
                
                Modified = true;
              }
            }
          }
        }
      }
    }
  }
  
  return Modified;
}

/// \brief Create a module constructor that registers all unsafe lines at startup.
static void createModuleConstructor(Module &M,
                                   const std::set<std::string> &allUnsafeLines,
                                   FunctionCallee RegisterLineFn) {
  LLVMContext &Ctx = M.getContext();
  
  // Create the constructor function
  FunctionType *CtorFnTy = FunctionType::get(Type::getVoidTy(Ctx), false);
  Function *CtorFn = Function::Create(CtorFnTy, GlobalValue::InternalLinkage,
                                      "unsafe_lines_module_ctor", &M);
  
  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", CtorFn);
  IRBuilder<> Builder(BB);
  
  // Register ALL unsafe lines found during compilation
  for (const auto &lineKey : allUnsafeLines) {
    size_t colonPos = lineKey.find(':');
    std::string file = lineKey.substr(0, colonPos);
    unsigned line = std::stoul(lineKey.substr(colonPos + 1));
    
    Value *LineArg = ConstantInt::get(Type::getInt64Ty(Ctx), line);
    Value *FileArg = createGlobalString(M, Builder, file);
    Builder.CreateCall(RegisterLineFn, {LineArg, FileArg});
  }
  
  Builder.CreateRetVoid();
  
  // Add to global constructors with priority 0 (runs before main)
  appendToGlobalCtors(M, CtorFn, 0);
}

/// \brief Create a module destructor that prints coverage stats at exit.
static void createModuleDestructor(Module &M, FunctionCallee PrintStatsFn) {
  LLVMContext &Ctx = M.getContext();
  
  // Create the destructor function
  FunctionType *DtorFnTy = FunctionType::get(Type::getVoidTy(Ctx), false);
  Function *DtorFn = Function::Create(DtorFnTy, GlobalValue::InternalLinkage,
                                      "unsafe_lines_module_dtor", &M);
  
  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", DtorFn);
  IRBuilder<> Builder(BB);
  
  // Call the print stats function
  Builder.CreateCall(PrintStatsFn);
  Builder.CreateRetVoid();
  
  // Add to global destructors with priority 0 (runs at exit)
  appendToGlobalDtors(M, DtorFn, 0);
}

} // anonymous namespace

PreservedAnalyses DynamicLineCountPass::run(Module &M, ModuleAnalysisManager &AM) {
  // Use std::set for deterministic ordering
  std::set<std::string> allUnsafeLines;
  bool Modified = false;
  
  // Setup runtime functions
  FunctionCallee RegisterLineFn, TrackExecutionFn, PrintStatsFn;
  setupRuntimeFunctions(M, RegisterLineFn, TrackExecutionFn, PrintStatsFn);
  
  // Phase 1: Collect all unsafe lines across ALL functions
  // and instrument execution tracking
  for (Function &F : M) {
    if (shouldInstrumentFunction(F)) {
      Modified |= collectAndInstrumentFunction(F, TrackExecutionFn, allUnsafeLines);
    }
  }
  
  // Phase 2: Create module constructor to register all lines at program startup
  // This ensures all lines are registered BEFORE any execution
  if (!allUnsafeLines.empty()) {
    createModuleConstructor(M, allUnsafeLines, RegisterLineFn);
    Modified = true;
  }
  
  // Phase 3: Create module destructor to print stats at program exit
  if (Modified) {
    createModuleDestructor(M, PrintStatsFn);
  }
  
  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
