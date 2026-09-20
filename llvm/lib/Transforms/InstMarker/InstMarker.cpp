//===-- InstMarker.cpp - Mark unsafe code blocks ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===-----------------------------------------------------------------------------===//
///
/// \file
/// This file implements the InstMarker pass for marking unsafe code blocks.
///
//===----------------------------------------------------------------------------===//

#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include <cstdlib>
#include <cstring>

#define DEBUG_TYPE "instmarker"

using namespace llvm;

// UNSAFE-RUST BEGIN
static cl::opt<bool> ReportMarkerCount(
    "report-marker-count", cl::init(false), cl::Hidden,
    cl::desc("Store marker pair count in unsafe_marker_count NamedMDNode"));
// UNSAFE-RUST END

namespace {

/// \brief Inserts one begin/end marker pair per basic block, bracketing the
/// span from the first to the last unsafe instruction in that BB.
///
/// The over-approximation is intended. A safe instruction lying between two
/// unsafe ones in the same block is bracketed with them, because later
/// optimisation can make it part of the unsafe operation. One pair per block
/// also perturbs the optimiser less than one pair per unsafe run would.
///
unsigned insertUnsafeMarkers(Function &F) {
  unsigned MarkerPairs = 0;
  Type *VoidTy = Type::getVoidTy(F.getContext());
  InlineAsm *AsmMarkerBegin =
      InlineAsm::get(FunctionType::get(VoidTy, false), UNSAFE_MARKER_BEGIN,
                     /* Constraints */ "", /* HasSideEffects */ true);
  InlineAsm *AsmMarkerEnd =
      InlineAsm::get(FunctionType::get(VoidTy, false), UNSAFE_MARKER_END,
                     /* Constraints */ "", /* HasSideEffects */ true);

  for (BasicBlock &BB : F) {
    Instruction *First = nullptr;
    Instruction *Last = nullptr;

    for (Instruction &I : BB) {
      if (I.getMetadata("unsafe_inst")) {
        if (!First)
          First = &I;
        Last = &I;
      }
    }

    if (!First)
      continue;

    IRBuilder<>(First).CreateCall(AsmMarkerBegin);

    // End marker goes after the last unsafe instruction. If that's the
    // terminator, fall back to inserting before it (no insertion point exists
    // after a terminator).
    if (Instruction *Next = Last->getNextNode())
      IRBuilder<>(Next).CreateCall(AsmMarkerEnd);
    else
      IRBuilder<>(BB.getTerminator()).CreateCall(AsmMarkerEnd);

    MarkerPairs++;
  }

  return MarkerPairs;
}

} // anonymous namespace

const char *llvm::UNSAFE_MARKER_BEGIN = "nop # marker_begin";
const char *llvm::UNSAFE_MARKER_END = "nop # marker_end";
const char *llvm::UNSAFE_SOURCE_LINES_MD = "unsafe_source_lines";

/// Records each unsafe instruction's source line twice: on the instruction,
/// where -O2 may drop it, and in a module-level NamedMDNode, which survives.
/// DynamicLineCount reads the latter, which is why it works above -O0.
// UNSAFE-RUST BEGIN
void InstMarkerPass::captureUnsafeLineInfo(Function &F) {
  Module *M = F.getParent();
  NamedMDNode *UnsafeLinesMD =
      M->getOrInsertNamedMetadata(UNSAFE_SOURCE_LINES_MD);
  LLVMContext &Ctx = F.getContext();

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (I.getMetadata("unsafe_inst")) {
        if (const DILocation *Loc = I.getDebugLoc()) {
          unsigned Line = Loc->getLine();
          StringRef File = Loc->getFilename();
          if (Line != 0 && !File.empty()) {
            createUnsafeLineMetadata(I, Line, File);

            Metadata *LineNum = ConstantAsMetadata::get(
                ConstantInt::get(Type::getInt32Ty(Ctx), Line));
            Metadata *FileName = MDString::get(Ctx, File);
            UnsafeLinesMD->addOperand(MDNode::get(Ctx, {LineNum, FileName}));
          }
        }
      }
    }
  }
}
// UNSAFE-RUST END

void InstMarkerPass::createUnsafeLineMetadata(Instruction &I, unsigned Line, 
                                              StringRef File) {
  LLVMContext &Ctx = I.getContext();

  Metadata *LineNum = ConstantAsMetadata::get(
    ConstantInt::get(Type::getInt32Ty(Ctx), Line));
  Metadata *FileName = MDString::get(Ctx, File);
  
  MDNode *LineInfo = MDNode::get(Ctx, {LineNum, FileName});
  I.setMetadata("unsafe_line_info", LineInfo);
}

// UNSAFE-RUST BEGIN
PreservedAnalyses InstMarkerPass::run(Function &F,
                                      FunctionAnalysisManager &AM) {
  // Before the markers: inserting them would shift what carries the
  // unsafe_inst metadata this reads.
  captureUnsafeLineInfo(F);

  unsigned MarkerPairs = insertUnsafeMarkers(F);

  if (MarkerPairs > 0) {
    unsigned UnsafeInsts = 0;
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (I.getMetadata("unsafe_inst"))
          UnsafeInsts++;

    LLVM_DEBUG(dbgs() << "instmarker: " << F.getName()
                      << " — " << MarkerPairs << " marker pair(s), "
                      << UnsafeInsts << " unsafe instruction(s)\n");

    if (ReportMarkerCount) {
      Module *M = F.getParent();
      NamedMDNode *CountMD =
          M->getOrInsertNamedMetadata("unsafe_marker_count");
      LLVMContext &Ctx = F.getContext();
      Metadata *FnName = MDString::get(Ctx, F.getName());
      Metadata *Count = ConstantAsMetadata::get(
          ConstantInt::get(Type::getInt32Ty(Ctx), MarkerPairs));
      CountMD->addOperand(MDNode::get(Ctx, {FnName, Count}));
    }
  }

  return MarkerPairs > 0 ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
// UNSAFE-RUST END
