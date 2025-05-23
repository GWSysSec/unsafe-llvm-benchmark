//===-- InstMarker.h - Marking and tracking unsafe instructions --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines a function pass that marks unsafe Rust instructions with
// inline assembly markers and tracks their execution through runtime calls.
// It also provides a foundational analysis for other passes to build upon.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_INSTMARKER_INSTMARKER_H
#define LLVM_TRANSFORMS_INSTMARKER_INSTMARKER_H

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include <string>
#include <map>
#include <vector>
#include <set>

namespace llvm {

// Constants for inline assembly markers
static const char *UNSAFE_MARKER_BEGIN = "nop # marker_begin";
static const char *UNSAFE_MARKER_END = "nop # marker_end";

// Runtime function name constants
static const char *TOTAL_UNSAFE_BLOCK_FN = "total_unsafe_block_count";

// Information about an unsafe instruction
struct UnsafeInstrInfo {
  Instruction *Inst;
  StringRef File;
  unsigned Line;
};

// Analysis result that can be shared between passes
struct UnsafeAnalysisResult {
  std::vector<UnsafeInstrInfo> UnsafeInsts;
  std::map<BasicBlock*, std::vector<Instruction*>> UnsafeInstsByBlock;
  int TotalUnsafeInst = 0;
  std::set<std::pair<std::string, unsigned>> InstrumentedLines;
  
  // Utility functions for sanitizing filenames
  static std::string sanitizeFileName(const std::string &File);
  
  // Check if a file is a project file (vs standard lib)
  static bool isProjectFile(StringRef File);
  
  // Check if we should only instrument the primary package
  static bool isPrimaryPackage();
};

// The foundational analysis pass
class UnsafeAnalysis : public AnalysisInfoMixin<UnsafeAnalysis> {
public:
  using Result = UnsafeAnalysisResult;
  Result run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
  
private:
  static AnalysisKey Key;
  friend struct AnalysisInfoMixin<UnsafeAnalysis>;
};

// Wrapper pass to run UnsafeAnalysis
struct UnsafeAnalysisPass : PassInfoMixin<UnsafeAnalysisPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

// The main InstMarker pass that uses the analysis results
struct InstMarkerPass : PassInfoMixin<InstMarkerPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  
  // This pass is required for unsafe code tracking
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_INSTMARKER_INSTMARKER_H
