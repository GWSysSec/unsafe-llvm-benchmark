//===--- InstMarker.cpp - Mark and track unsafe instructions --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// InstMarker is the foundation pass for unsafe Rust code analysis. It:
// 1. Identifies instructions with "unsafe_inst" metadata
// 2. Inserts marker_begin/marker_end assembly markers around unsafe blocks
// 3. Calls total_unsafe_block_count() to track block execution
// 4. Provides analysis results for use by other passes (like DynamicLineCount)
// 5. Supports primary package filtering with CARGO_PRIMARY_PACKAGE=1
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Constants.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <set>
#include <map>
#include <string>
#include <algorithm>
#include <vector>
#include <cstring>
#include <cstdlib>

using namespace llvm;

// Implement the Analysis Key
AnalysisKey UnsafeAnalysis::Key;

// Implement utility functions
std::string UnsafeAnalysisResult::sanitizeFileName(const std::string &File) {
  std::string Name = File;
  std::replace(Name.begin(), Name.end(), '/', '_');
  std::replace(Name.begin(), Name.end(), '\\', '_');
  std::replace(Name.begin(), Name.end(), '.', '_');
  return Name;
}

bool UnsafeAnalysisResult::isProjectFile(StringRef File) {
  // Skip standard library and toolchain files
  if (File.contains("/rustc/") || File.contains("/.cargo/") || 
      File.contains("/library/"))
    return false;
  return true;
}

bool UnsafeAnalysisResult::isPrimaryPackage() {
  // Only instrument the primary package if CARGO_PRIMARY_PACKAGE=1
  const char *p = std::getenv("CARGO_PRIMARY_PACKAGE");
  return p && std::strcmp(p, "1") == 0;
}

// Implement UnsafeAnalysis to collect and analyze unsafe instructions
UnsafeAnalysis::Result UnsafeAnalysis::run(Function &F, FunctionAnalysisManager &AM) {
  UnsafeAnalysisResult Result;
  
  if (F.isDeclaration())
    return Result;
    
  // Collect all unsafe instructions and their locations
  for (Instruction &I : instructions(F)) {
    if (!I.getMetadata("unsafe_inst"))
      continue;
      
    Result.TotalUnsafeInst++;
    
    DebugLoc DL = I.getDebugLoc();
    if (!DL)
      continue;
      
    const DILocation *Loc = DL.get();
    StringRef File = Loc->getFilename();
    if (File.empty() || !UnsafeAnalysisResult::isProjectFile(File))
      continue;
      
    unsigned Line = Loc->getLine();
    
    Result.UnsafeInsts.push_back({&I, File, Line});
    Result.UnsafeInstsByBlock[I.getParent()].push_back(&I);
  }
  
  // Sort instructions by their position in each block
  for (auto &BlockEntry : Result.UnsafeInstsByBlock) {
    std::vector<Instruction*> &BlockUnsafeInsts = BlockEntry.second;
    
    // Sort instructions by their position in the block
    std::sort(BlockUnsafeInsts.begin(), BlockUnsafeInsts.end(),
      [&](Instruction *A, Instruction *B) {
        return A->comesBefore(B);
      });
  }
  
  return Result;
}

// Implement the wrapper pass that returns PreservedAnalyses
PreservedAnalyses UnsafeAnalysisPass::run(Function &F, FunctionAnalysisManager &AM) {
  // Run the analysis
  AM.getResult<UnsafeAnalysis>(F);
  
  // The analysis doesn't modify anything
  return PreservedAnalyses::all();
}

// Implement InstMarkerPass to mark unsafe blocks and insert runtime calls
PreservedAnalyses InstMarkerPass::run(Function &F, FunctionAnalysisManager &AM) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();
    
  // Check if we should only instrument the primary package
  if (!UnsafeAnalysisResult::isPrimaryPackage())
    return PreservedAnalyses::all();

  // Get the analysis result
  auto &UnsafeResult = AM.getResult<UnsafeAnalysis>(F);
  
  // If there are no unsafe instructions, nothing to do
  if (UnsafeResult.TotalUnsafeInst == 0)
    return PreservedAnalyses::all();
  
  Module *M = F.getParent();
  LLVMContext &Ctx = M->getContext();
  bool Modified = false;

  // Prepare for inline assembly markers
  Type *VoidTy = Type::getVoidTy(Ctx);
  InlineAsm *AsmMarkerBegin = InlineAsm::get(FunctionType::get(VoidTy, false),
                                             UNSAFE_MARKER_BEGIN, "", true);
  InlineAsm *AsmMarkerEnd = InlineAsm::get(FunctionType::get(VoidTy, false),
                                           UNSAFE_MARKER_END, "", true);

  // Prepare runtime function prototype for block counting
  Type *Int64Ty = Type::getInt64Ty(Ctx);
  FunctionType *TotalBlockFnTy = FunctionType::get(VoidTy, {Int64Ty}, false);
  FunctionCallee TotalUnsafeBlockFn = M->getOrInsertFunction(TOTAL_UNSAFE_BLOCK_FN, TotalBlockFnTy);

  // Set function attributes
  if (auto *Func = dyn_cast<Function>(TotalUnsafeBlockFn.getCallee())) {
    Func->removeFnAttr(Attribute::ReadNone);
    Func->removeFnAttr(Attribute::ReadOnly);
    Func->addFnAttr(Attribute::NoInline);
    Func->setLinkage(GlobalValue::ExternalLinkage);
  }
  
  // Process each basic block
  for (BasicBlock &BB : F) {
    Instruction *FirstUnsafe = nullptr;
    Instruction *LastUnsafe = nullptr;
    
    // Find the first and last unsafe instructions in the block
    for (Instruction &I : BB) {
      if (I.getMetadata("unsafe_inst")) {
        if (!FirstUnsafe) {
          FirstUnsafe = &I;
        }
        LastUnsafe = &I;
      }
    }
    
    // If unsafe instructions were found, insert markers
    if (FirstUnsafe && LastUnsafe) {
      // Insert marker_begin before the first unsafe instruction
      IRBuilder<> Builder(FirstUnsafe);
      Builder.CreateCall(AsmMarkerBegin);
      
      // Add call to total_unsafe_block_count with block size
      int UnsafeCount = 0;
      for (Instruction &I : BB) {
        if (I.getMetadata("unsafe_inst"))
          UnsafeCount++;
      }
      Builder.CreateCall(TotalUnsafeBlockFn, {
        ConstantInt::get(Int64Ty, UnsafeCount)
      });
      
      Modified = true;
      
      // Insert marker_end after the last unsafe instruction
      if (Instruction *NextInst = LastUnsafe->getNextNode()) {
        IRBuilder<> EndBuilder(NextInst);
        EndBuilder.CreateCall(AsmMarkerEnd);
      } else {
        IRBuilder<> EndBuilder(&BB);
        EndBuilder.SetInsertPoint(BB.getTerminator());
        EndBuilder.CreateCall(AsmMarkerEnd);
      }
    }
  }
  
  // Only output summary for functions with significant unsafe blocks
  if (Modified && UnsafeResult.TotalUnsafeInst > 10) {
    errs() << "[InstMarker] " << F.getName() 
         << " - " << UnsafeResult.TotalUnsafeInst << " unsafe instrs\n";
  }
  
  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
