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
#include <cstdlib>
#include <cstring>

using namespace llvm;

namespace {

/// \brief Inserts begin/end markers around each contiguous sequence of unsafe
/// instructions within a basic block.
///
/// This function iterates through each basic block tracking transitions
/// between safe and unsafe instructions. Each contiguous run of instructions
/// tagged with "unsafe_inst" metadata gets its own begin/end marker pair.
/// This avoids the over-approximation of wrapping disjoint unsafe regions
/// (with safe code between them) in a single marker pair.
///
/// \param F The target function to instrument.
/// \returns True if the function was modified, false otherwise.
bool insertUnsafeMarkers(Function &F) {
  bool Modified = false;
  Type *VoidTy = Type::getVoidTy(F.getContext());
  InlineAsm *AsmMarkerBegin =
      InlineAsm::get(FunctionType::get(VoidTy, false), UNSAFE_MARKER_BEGIN,
                     /* Constraints */ "", /* HasSideEffects */ true);
  InlineAsm *AsmMarkerEnd =
      InlineAsm::get(FunctionType::get(VoidTy, false), UNSAFE_MARKER_END,
                     /* Constraints */ "", /* HasSideEffects */ true);

  for (BasicBlock &BB : F) {
    bool inUnsafe = false;
    Instruction *seqStart = nullptr;

    for (Instruction &I : BB) {
      bool isUnsafe = I.getMetadata("unsafe_inst") != nullptr;

      if (isUnsafe && !inUnsafe) {
        // Entering a new unsafe sequence — record where it starts
        seqStart = &I;
        inUnsafe = true;
      } else if (!isUnsafe && inUnsafe) {
        // Exiting an unsafe sequence — insert markers around [seqStart, prev]
        IRBuilder<> BeginBuilder(seqStart);
        BeginBuilder.CreateCall(AsmMarkerBegin);

        // End marker goes before this (first safe) instruction
        IRBuilder<> EndBuilder(&I);
        EndBuilder.CreateCall(AsmMarkerEnd);

        Modified = true;
        inUnsafe = false;
        seqStart = nullptr;
      }
    }

    // Handle an unsafe sequence that extends to the end of the BB.
    // We insert the end marker before the terminator — this is the last
    // valid insertion point in a BB. If the terminator itself is unsafe,
    // it will technically fall outside the markers, but we cannot insert
    // after a terminator in LLVM IR. This matches the original behavior.
    if (inUnsafe && seqStart) {
      IRBuilder<> BeginBuilder(seqStart);
      BeginBuilder.CreateCall(AsmMarkerBegin);

      IRBuilder<> EndBuilder(BB.getTerminator());
      EndBuilder.CreateCall(AsmMarkerEnd);

      Modified = true;
    }
  }

  return Modified;
}

} // anonymous namespace

// These constants are defined in the header for other passes to use.
// We provide their definitions here.
const char *llvm::UNSAFE_MARKER_BEGIN = "nop # marker_begin";
const char *llvm::UNSAFE_MARKER_END = "nop # marker_end";

/// \brief Captures unsafe line information from debug metadata.
/// \param F The target function to process.
void InstMarkerPass::captureUnsafeLineInfo(Function &F) {
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (I.getMetadata("unsafe_inst")) {
        if (const DILocation *Loc = I.getDebugLoc()) {
          unsigned Line = Loc->getLine();
          StringRef File = Loc->getFilename();
          if (Line != 0 && !File.empty()) {
            createUnsafeLineMetadata(I, Line, File);
          }
        }
      }
    }
  }
}

/// \brief Creates unsafe line metadata for an instruction.
/// \param I The instruction to attach metadata to.
/// \param Line The source line number.
/// \param File The source file name.
void InstMarkerPass::createUnsafeLineMetadata(Instruction &I, unsigned Line, 
                                              StringRef File) {
  LLVMContext &Ctx = I.getContext();
  
  // Create metadata: !unsafe_line_info !{line_number, file_name}
  Metadata *LineNum = ConstantAsMetadata::get(
    ConstantInt::get(Type::getInt32Ty(Ctx), Line));
  Metadata *FileName = MDString::get(Ctx, File);
  
  MDNode *LineInfo = MDNode::get(Ctx, {LineNum, FileName});
  I.setMetadata("unsafe_line_info", LineInfo);
}

PreservedAnalyses InstMarkerPass::run(Function &F,
                                      FunctionAnalysisManager &AM) {
  // Capture line information BEFORE inserting markers
  captureUnsafeLineInfo(F);
  
  bool Modified = insertUnsafeMarkers(F);

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
