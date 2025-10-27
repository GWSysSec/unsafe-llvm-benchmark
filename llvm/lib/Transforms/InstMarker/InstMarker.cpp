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

/// \brief Checks if the current build is for the primary package.
///
/// This uses the CARGO_PRIMARY_PACKAGE environment variable.
bool isPrimaryPackageImpl() {
  const char *P = getenv("CARGO_PRIMARY_PACKAGE");
  return P && strcmp(P, "1") == 0;
}

/// \brief Inserts begin/end markers around sequences of unsafe instructions.
///
/// This function iterates through each basic block to find instructions that
/// have been tagged with "unsafe_inst" metadata. It then inserts a
/// `UNSAFE_MARKER_BEGIN` before the first unsafe instruction and an
/// `UNSAFE_MARKER_END` after the last one in each contiguous sequence
/// within a basic block.
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
    Instruction *FirstUnsafeInst = nullptr;
    Instruction *LastUnsafeInst = nullptr;

    // Find the first and last unsafe instructions in the basic block.
    for (Instruction &I : BB) {
      if (I.getMetadata("unsafe_inst")) {
        if (!FirstUnsafeInst) {
          FirstUnsafeInst = &I;
        }
        LastUnsafeInst = &I;
      }
    }

    // If a sequence was found, insert the markers.
    if (FirstUnsafeInst && LastUnsafeInst) {
      // Insert the begin marker before the first unsafe instruction.
      IRBuilder<> Builder(FirstUnsafeInst);
      Builder.CreateCall(AsmMarkerBegin);
      Modified = true;

      // Insert the end marker after the last unsafe instruction.
      if (Instruction *NextInst = LastUnsafeInst->getNextNode()) {
        IRBuilder<> EndBuilder(NextInst);
        EndBuilder.CreateCall(AsmMarkerEnd);
      } else {
        // If the last unsafe instruction is the terminator, insert before it.
        IRBuilder<> EndBuilder(BB.getTerminator());
        EndBuilder.CreateCall(AsmMarkerEnd);
      }
    }
  }

  return Modified;
}

} // anonymous namespace

// These constants are defined in the header for other passes to use.
// We provide their definitions here.
const char *llvm::UNSAFE_MARKER_BEGIN = "nop # marker_begin";
const char *llvm::UNSAFE_MARKER_END = "nop # marker_end";

/// \brief Checks if the current build is for the primary package.
/// \returns True if this is the primary package build, false otherwise.
bool InstMarkerPass::isPrimaryPackage() {
  return isPrimaryPackageImpl();
}

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
  if (!isPrimaryPackage())
    return PreservedAnalyses::all();

  // Capture line information BEFORE inserting markers
  captureUnsafeLineInfo(F);
  
  bool Modified = insertUnsafeMarkers(F);

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
