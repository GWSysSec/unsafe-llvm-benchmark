//===--- InstMarker.cpp - Mark and track unsafe instructions --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
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

using namespace llvm;

// Constants for inline assembly markers
static const char *UNSAFE_MARKER_BEGIN = "nop # marker_begin";
static const char *UNSAFE_MARKER_END = "nop # marker_end";

// Runtime function name constants
static const char *TOTAL_UNSAFE_BLOCK_FN = "total_unsafe_block_count";

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

PreservedAnalyses InstMarkerPass::run(Function &F, FunctionAnalysisManager &AM) {
  if (F.isDeclaration())
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
  if (auto *F = dyn_cast<Function>(TotalUnsafeBlockFn.getCallee())) {
    F->removeFnAttr(Attribute::ReadNone);
    F->removeFnAttr(Attribute::ReadOnly);
    F->addFnAttr(Attribute::NoInline);
    F->setLinkage(GlobalValue::ExternalLinkage);
  }
  
  // First, collect all unsafe instructions and their locations
  struct UnsafeInstrInfo {
    Instruction *Inst;
    StringRef File;
    unsigned Line;
  };
  
  std::vector<UnsafeInstrInfo> UnsafeInsts;
  std::map<BasicBlock*, std::vector<Instruction*>> UnsafeInstsByBlock;
  
  int totalUnsafeInst = 0;
  
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
  
  // Now process each basic block with unsafe instructions
  for (auto &BlockEntry : UnsafeInstsByBlock) {
    BasicBlock *BB = BlockEntry.first;
    std::vector<Instruction*> &BlockUnsafeInsts = BlockEntry.second;
    
    if (BlockUnsafeInsts.empty())
      continue;
      
    // Sort instructions by their position in the block
    std::sort(BlockUnsafeInsts.begin(), BlockUnsafeInsts.end(),
      [&](Instruction *A, Instruction *B) {
        return A->comesBefore(B);
      });
      
    // Find the first and last unsafe instructions
    Instruction *FirstUnsafe = BlockUnsafeInsts.front();
    Instruction *LastUnsafe = BlockUnsafeInsts.back();
    
    // Insert marker_begin before the first unsafe instruction
    IRBuilder<> Builder(FirstUnsafe);
    Builder.CreateCall(AsmMarkerBegin);
    
    // Add call to total_unsafe_block_count with block size
    Builder.CreateCall(TotalUnsafeBlockFn, {
      ConstantInt::get(Int64Ty, BlockUnsafeInsts.size())
    });
    
    Modified = true;
    
    // Insert marker_end after the last unsafe instruction
    if (Instruction *NextInst = LastUnsafe->getNextNode()) {
      IRBuilder<> EndBuilder(NextInst);
      EndBuilder.CreateCall(AsmMarkerEnd);
    } else {
      IRBuilder<> EndBuilder(BB);
      EndBuilder.SetInsertPoint(BB->getTerminator());
      EndBuilder.CreateCall(AsmMarkerEnd);
    }
  }
  
  // Output summary if any unsafe instructions were found
  if (totalUnsafeInst > 0) {
    // Use llvm's errs() stream for minimal output
    errs() << "[InstMarker] Function: " << F.getName() 
           << " - Unsafe: " << totalUnsafeInst 
           << ", Blocks: " << UnsafeInstsByBlock.size() << "\n";
  }
  
  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
