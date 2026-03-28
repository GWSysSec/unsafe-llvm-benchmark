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

#include "llvm/Transforms/DynamicAnalysis/DynamicLineCount.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

using namespace llvm;

const char *REGISTER_UNSAFE_LINE_FN = "register_unsafe_line";
const char *TRACK_UNSAFE_LINE_EXECUTION_FN = "track_unsafe_line_execution";
const char *PRINT_UNSAFE_COVERAGE_STATS_FN = "print_unsafe_coverage_stats";

namespace {

static bool isPrimaryPackage() {
  const char *P = getenv("CARGO_PRIMARY_PACKAGE");
  return P && strcmp(P, "1") == 0;
}

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

/// \brief Instrument execution tracking in a function using markers and !dbg.
///
/// Walks marker regions (which survive O2) and checks each instruction's
/// !dbg location (which also survives O2 with -C debuginfo). If the source
/// line matches a registered unsafe line, inserts a tracking call.
/// Deduplicates per-BB to avoid excessive overhead from multiple instructions
/// mapping to the same source line.
// UNSAFE-RUST BEGIN
static bool instrumentFunction(Function &F,
                               FunctionCallee TrackExecutionFn,
                               const std::set<std::string> &registeredLines) {
  Module &M = *F.getParent();
  LLVMContext &Ctx = F.getContext();
  bool Modified = false;

  for (BasicBlock &BB : F) {
    bool insideUnsafeRegion = false;
    std::set<std::string> trackedInBB; // deduplicate per-BB

    for (Instruction &I : BB) {
      bool isBegin = false, isEnd = false;

      if (isMarkerInstruction(I, isBegin, isEnd)) {
        if (isBegin) {
          insideUnsafeRegion = true;

          // At O2, the optimizer may remove all instructions between markers
          // (empty marker pairs), but the marker itself carries a !dbg location
          // from the original unsafe code. Use it for tracking.
          if (const DILocation *Loc = I.getDebugLoc()) {
            unsigned Line = Loc->getLine();
            StringRef File = Loc->getFilename();
            if (Line != 0 && !File.empty()) {
              std::string LineKey = File.str() + ":" + std::to_string(Line);
              if (registeredLines.count(LineKey) &&
                  trackedInBB.insert(LineKey).second) {
                // Insert tracking call after the marker_begin
                Instruction *Next = I.getNextNonDebugInstruction();
                if (Next) {
                  IRBuilder<> Builder(Next);
                  Value *LineArg =
                      ConstantInt::get(Type::getInt64Ty(Ctx), Line);
                  Value *FileArg = createGlobalString(M, Builder, File);
                  Builder.CreateCall(TrackExecutionFn, {LineArg, FileArg});
                  Modified = true;
                }
              }
            }
          }
        } else if (isEnd) {
          insideUnsafeRegion = false;
        }
        continue;
      }

      if (!insideUnsafeRegion)
        continue;

      // Use standard !dbg location (survives O2 with -C debuginfo)
      const DILocation *Loc = I.getDebugLoc();
      if (!Loc)
        continue;

      unsigned Line = Loc->getLine();
      StringRef File = Loc->getFilename();
      if (Line == 0 || File.empty())
        continue;

      std::string LineKey = File.str() + ":" + std::to_string(Line);

      // Only track lines that were registered as unsafe by InstMarker
      if (!registeredLines.count(LineKey))
        continue;

      // Deduplicate: one tracking call per unique line per BB
      if (!trackedInBB.insert(LineKey).second)
        continue;

      IRBuilder<> Builder(&I);
      Value *LineArg = ConstantInt::get(Type::getInt64Ty(Ctx), Line);
      Value *FileArg = createGlobalString(M, Builder, File);
      Builder.CreateCall(TrackExecutionFn, {LineArg, FileArg});
      Modified = true;
    }
  }

  return Modified;
}
// UNSAFE-RUST END

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

// UNSAFE-RUST BEGIN
PreservedAnalyses DynamicLineCountPass::run(Module &M, ModuleAnalysisManager &AM) {
  if (!isPrimaryPackage())
    return PreservedAnalyses::all();

  // Phase 1: Read registered unsafe lines from NamedMDNode.
  // InstMarker populates this pre-optimization; it survives O2 unlike
  // instruction-level unsafe_line_info metadata.
  std::set<std::string> registeredLines;
  if (NamedMDNode *UnsafeLinesMD =
          M.getNamedMetadata(UNSAFE_SOURCE_LINES_MD)) {
    for (unsigned i = 0; i < UnsafeLinesMD->getNumOperands(); i++) {
      MDNode *Entry = UnsafeLinesMD->getOperand(i);
      if (Entry->getNumOperands() >= 2) {
        auto *LineConst = dyn_cast<ConstantAsMetadata>(Entry->getOperand(0));
        auto *FileStr = dyn_cast<MDString>(Entry->getOperand(1));
        if (LineConst && FileStr) {
          unsigned Line =
              LineConst->getValue()->getUniqueInteger().getZExtValue();
          std::string LineKey =
              FileStr->getString().str() + ":" + std::to_string(Line);
          registeredLines.insert(LineKey);
        }
      }
    }
  }

  if (registeredLines.empty())
    return PreservedAnalyses::all();

  // Setup runtime functions
  FunctionCallee RegisterLineFn, TrackExecutionFn, PrintStatsFn;
  setupRuntimeFunctions(M, RegisterLineFn, TrackExecutionFn, PrintStatsFn);

  // Phase 2: Create module constructor to register all unsafe lines at startup
  createModuleConstructor(M, registeredLines, RegisterLineFn);
  bool Modified = true;

  // Phase 3: Instrument execution tracking using markers + !dbg locations.
  // At O2, markers survive (inline asm with hasSideEffects) and !dbg
  // locations survive (with -C debuginfo). We match !dbg lines against
  // the registered set to only track genuinely unsafe lines.
  for (Function &F : M) {
    if (shouldInstrumentFunction(F)) {
      Modified |= instrumentFunction(F, TrackExecutionFn, registeredLines);
    }
  }

  // Phase 4: Create module destructor to print stats at program exit
  createModuleDestructor(M, PrintStatsFn);

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
// UNSAFE-RUST END
