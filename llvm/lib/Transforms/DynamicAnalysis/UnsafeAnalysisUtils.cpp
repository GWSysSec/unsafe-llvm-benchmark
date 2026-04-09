//===-- UnsafeAnalysisUtils.cpp - Shared utilities for unsafe analysis -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------------===//
///
/// \file
/// Implementation of shared utilities for all dynamic analysis passes.
///
//===----------------------------------------------------------------------------===//

// UNSAFE-RUST BEGIN
#include "llvm/Transforms/DynamicAnalysis/UnsafeAnalysisUtils.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>
#include <cstring>

#define DEBUG_TYPE "unsafe-analysis"

using namespace llvm;

bool llvm::isPrimaryPackage() {
  const char *P = getenv("CARGO_PRIMARY_PACKAGE");
  return P && strcmp(P, "1") == 0;
}

DebugLoc llvm::getInstrumentationDebugLoc(Instruction *InsertBefore) {
  if (DebugLoc DL = InsertBefore->getDebugLoc())
    return DL;
  for (Instruction &I : *InsertBefore->getParent()) {
    if (DebugLoc DL = I.getDebugLoc())
      return DL;
  }
  if (DISubprogram *SP = InsertBefore->getFunction()->getSubprogram())
    return DILocation::get(SP->getContext(), 0, 0, SP);
  return DebugLoc();
}

/// Helper: extract inline asm string from instruction if it's an asm call.
static const InlineAsm *getInlineAsm(const Instruction &I) {
  if (const auto *CB = dyn_cast<CallBase>(&I))
    if (const auto *IA =
            dyn_cast<InlineAsm>(CB->getCalledOperand()->stripPointerCasts()))
      return IA;
  return nullptr;
}

bool llvm::isMarkerBegin(const Instruction &I) {
  if (const InlineAsm *IA = getInlineAsm(I))
    return IA->getAsmString() == UNSAFE_MARKER_BEGIN;
  return false;
}

bool llvm::isMarkerEnd(const Instruction &I) {
  if (const InlineAsm *IA = getInlineAsm(I))
    return IA->getAsmString() == UNSAFE_MARKER_END;
  return false;
}

bool llvm::isMarkerInstruction(const Instruction &I, bool &IsBegin,
                               bool &IsEnd) {
  IsBegin = false;
  IsEnd = false;
  if (const InlineAsm *IA = getInlineAsm(I)) {
    StringRef AsmStr = IA->getAsmString();
    if (AsmStr == UNSAFE_MARKER_BEGIN) {
      IsBegin = true;
      return true;
    }
    if (AsmStr == UNSAFE_MARKER_END) {
      IsEnd = true;
      return true;
    }
  }
  return false;
}

bool llvm::isMarkerAsm(const CallBase *Call, const char *MarkerStr) {
  if (!Call)
    return false;
  if (const auto *IA = dyn_cast<InlineAsm>(Call->getCalledOperand()))
    return IA->getAsmString() == MarkerStr;
  return false;
}

void llvm::collectMarkers(Function &F,
                          std::vector<CallInst *> &BeginMarkers,
                          std::vector<CallInst *> &EndMarkers) {
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (isMarkerBegin(I))
          BeginMarkers.push_back(CI);
        else if (isMarkerEnd(I))
          EndMarkers.push_back(CI);
      }
    }
  }
}

void llvm::validateSESERegions(const std::vector<CallInst *> &BeginMarkers,
                               const std::vector<CallInst *> &EndMarkers,
                               DominatorTree &DT, PostDominatorTree &PDT,
                               std::vector<SESERegion> &ValidRegions) {
  for (CallInst *Begin : BeginMarkers) {
    bool Matched = false;
    for (CallInst *End : EndMarkers) {
      if (DT.dominates(Begin, End) && PDT.dominates(End, Begin)) {
        ValidRegions.push_back({Begin, End});
        LLVM_DEBUG(dbgs() << "sese: valid region in "
                          << Begin->getFunction()->getName() << "\n");
        Matched = true;
        break;
      }
    }
    if (!Matched) {
      LLVM_DEBUG(dbgs() << "sese: unmatched marker in "
                        << Begin->getFunction()->getName() << "\n");
    }
  }
}

bool llvm::isInSESERegion(const Instruction &I,
                          const std::vector<SESERegion> &ValidRegions,
                          DominatorTree &DT, PostDominatorTree &PDT) {
  for (const auto &R : ValidRegions) {
    if (DT.dominates(R.Begin->getParent(), I.getParent()) &&
        PDT.dominates(R.End->getParent(), I.getParent())) {
      return true;
    }
  }
  return false;
}

bool llvm::verifyUnsafeMarkers(Function &F) {
#ifdef NDEBUG
  return true; // No-op in release builds
#else
  bool Valid = true;

  for (BasicBlock &BB : F) {
    bool InRegion = false;
    unsigned BeginCount = 0, EndCount = 0;

    for (Instruction &I : BB) {
      bool IsBegin = false, IsEnd = false;
      if (!isMarkerInstruction(I, IsBegin, IsEnd))
        continue;

      if (IsBegin) {
        BeginCount++;
        if (InRegion) {
          LLVM_DEBUG(dbgs() << "verify-markers: NESTED begin in "
                            << F.getName() << " BB " << BB.getName()
                            << " — marker_begin while already inside region\n");
          Valid = false;
        }
        InRegion = true;
      } else if (IsEnd) {
        EndCount++;
        if (!InRegion) {
          LLVM_DEBUG(dbgs() << "verify-markers: ORPHANED end in "
                            << F.getName() << " BB " << BB.getName()
                            << " — marker_end without preceding begin\n");
          Valid = false;
        }
        InRegion = false;
      }
    }

    if (InRegion) {
      LLVM_DEBUG(dbgs() << "verify-markers: UNCLOSED region in "
                        << F.getName() << " BB " << BB.getName()
                        << " — marker_begin without marker_end\n");
      Valid = false;
    }

    if (BeginCount != EndCount) {
      LLVM_DEBUG(dbgs() << "verify-markers: MISMATCH in "
                        << F.getName() << " BB " << BB.getName()
                        << " — " << BeginCount << " begin(s) vs "
                        << EndCount << " end(s)\n");
      Valid = false;
    }
  }

  return Valid;
#endif
}
// UNSAFE-RUST END
