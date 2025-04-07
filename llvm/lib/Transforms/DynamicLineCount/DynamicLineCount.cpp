//===--- DynamicLineCount.cpp - Track unsafe instructions ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/DynamicLineCount/DynamicLineCount.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Constants.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
#include <string>
#include <algorithm>
#include <map>

using namespace llvm;

// Runtime function name constants - same as InstMarker
static const char *UPDATE_UNSAFE_LINE_FN = "update_unsafe_line_counter";
static const char *MARK_UNSAFE_LINE_FN = "mark_unsafe_line_executed";

// Utility functions
static std::string sanitizeFileName(const std::string &File) {
  std::string Name = File;
  std::replace(Name.begin(), Name.end(), '/', '_');
  std::replace(Name.begin(), Name.end(), '\\', '_');
  std::replace(Name.begin(), Name.end(), '.', '_');
  return Name;
}

static bool isProjectFile(StringRef File) {
  // Skip standard library and toolchain files
  if (File.contains("/rustc/") || File.contains("/.cargo/") || 
      File.contains("/library/"))
    return false;
  return true;
}

PreservedAnalyses DynamicLineCountPass::run(Function &F, FunctionAnalysisManager &AM) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();

  Module *M = F.getParent();
  LLVMContext &Ctx = M->getContext();
  bool Modified = false;

  // Prepare runtime function prototypes
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *Int64Ty = Type::getInt64Ty(Ctx);
  Type *Int8Ty = Type::getInt8Ty(Ctx);
  Type *Int8PtrTy = PointerType::getUnqual(Int8Ty);
  
  FunctionType *RuntimeFnTy = FunctionType::get(VoidTy, {Int64Ty, Int8PtrTy}, false);
  FunctionCallee UpdateUnsafeLine = M->getOrInsertFunction(UPDATE_UNSAFE_LINE_FN, RuntimeFnTy);
  FunctionCallee MarkUnsafeLine = M->getOrInsertFunction(MARK_UNSAFE_LINE_FN, RuntimeFnTy);

  // Set function attributes
  for (auto *RuntimeFn : {
      dyn_cast<Function>(UpdateUnsafeLine.getCallee()),
      dyn_cast<Function>(MarkUnsafeLine.getCallee())}) {
    if (RuntimeFn) {
      RuntimeFn->removeFnAttr(Attribute::ReadNone);
      RuntimeFn->removeFnAttr(Attribute::ReadOnly);
      RuntimeFn->addFnAttr(Attribute::NoInline);
      RuntimeFn->setLinkage(GlobalValue::ExternalLinkage);
    }
  }

  // Track unique source locations to avoid duplicate instrumentation
  std::set<std::pair<std::string, unsigned>> InstrumentedLines;
  
  // First, collect all unsafe instructions and their locations
  struct UnsafeInstrInfo {
    Instruction *Inst;
    StringRef File;
    unsigned Line;
  };
  
  std::vector<UnsafeInstrInfo> UnsafeInsts;
  std::map<BasicBlock*, std::vector<Instruction*>> UnsafeInstsByBlock;
  
  int totalUnsafeInst = 0;
  int instrumentedCount = 0;
  
  for (Instruction &I : instructions(F)) {
    if (!I.getMetadata("unsafe_inst"))
      continue;
      
    totalUnsafeInst++;
    
    DebugLoc DL = I.getDebugLoc();
    if (!DL)
      continue;
      
    const DILocation *Loc = DL.get();
    StringRef File = Loc->getFilename();
    if (File.empty() || !isProjectFile(File))
      continue;
      
    unsigned Line = Loc->getLine();
    
    UnsafeInsts.push_back({&I, File, Line});
    UnsafeInstsByBlock[I.getParent()].push_back(&I);
  }
  
  // Process each unsafe instruction for line tracking
  for (const auto &Info : UnsafeInsts) {
    auto Key = std::make_pair(Info.File.str(), Info.Line);
    
    // Only instrument each line once
    if (!InstrumentedLines.insert(Key).second)
      continue;
    
    Instruction *UnsafeInst = Info.Inst;
    BasicBlock *BB = UnsafeInst->getParent();
    
    // Pick a safe insertion point
    IRBuilder<> Builder(Ctx);
    if (isa<PHINode>(*UnsafeInst) || isa<LandingPadInst>(*UnsafeInst)) {
      Instruction *InsertPoint = nullptr;
      for (Instruction &I : *BB) {
        if (!isa<PHINode>(I) && !isa<LandingPadInst>(I)) {
          InsertPoint = &I;
          break;
        }
      }
      if (!InsertPoint)
        continue;
      Builder.SetInsertPoint(InsertPoint);
    } else {
      Builder.SetInsertPoint(UnsafeInst);
    }
    
    // Create or reuse a global string for File
    std::string GlobalName = "unsafe_str_" + sanitizeFileName(Info.File.str());
    GlobalVariable *GV = M->getNamedGlobal(GlobalName);
    if (!GV) {
      auto *FileConstant = ConstantDataArray::getString(Ctx, Info.File.str(), true);
      GV = new GlobalVariable(
        *M, FileConstant->getType(), /*isConstant=*/true,
        GlobalValue::InternalLinkage, FileConstant, GlobalName
      );
      GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    }
    Value *FileArg = Builder.CreateBitCast(GV, Int8PtrTy);
    
    // Insert calls to runtime functions
    Builder.CreateCall(UpdateUnsafeLine, {
      ConstantInt::get(Int64Ty, Info.Line),
      FileArg
    });
    
    Builder.CreateCall(MarkUnsafeLine, {
      ConstantInt::get(Int64Ty, Info.Line),
      FileArg
    });
    
    instrumentedCount++;
    Modified = true;
  }
  
  // Output summary if any unsafe instructions were found
  if (totalUnsafeInst > 0) {
    // Use llvm's errs() stream for minimal output
    errs() << "[DynamicLineCount] Function: " << F.getName() 
           << " - Unsafe: " << totalUnsafeInst 
           << ", Instrumented: " << instrumentedCount << "\n";
  }
  
  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
