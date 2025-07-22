//===-- InstMarker.h - Mark unsafe code blocks ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file declares the InstMarker pass for marking unsafe code blocks.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_INSTMARKER_INSTMARKER_H
#define LLVM_TRANSFORMS_INSTMARKER_INSTMARKER_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Function;
class Instruction;
class StringRef;

extern const char *UNSAFE_MARKER_BEGIN;
extern const char *UNSAFE_MARKER_END;

/// \brief Pass that marks unsafe code blocks with inline assembly markers.
///
/// This pass identifies instructions tagged with "unsafe_inst" metadata and
/// inserts begin/end markers around contiguous sequences of such instructions
/// within basic blocks. The markers are implemented as inline assembly to
/// ensure they are preserved through optimization passes.
class InstMarkerPass : public PassInfoMixin<InstMarkerPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

  static bool isRequired() { return true; }
  static bool isPrimaryPackage();

private:
  /// \brief Captures line information from unsafe instructions while debug info exists.
  /// \param F The function to process for line information capture.
  void captureUnsafeLineInfo(Function &F);
  
  /// \brief Creates custom metadata containing line and file information.
  /// \param I The instruction to attach metadata to.
  /// \param Line The source line number.
  /// \param File The source file name.
  void createUnsafeLineMetadata(Instruction &I, unsigned Line, StringRef File);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_INSTMARKER_INSTMARKER_H