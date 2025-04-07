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
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_INSTMARKER_INSTMARKER_H
#define LLVM_TRANSFORMS_INSTMARKER_INSTMARKER_H

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Function.h"
#include <string>

namespace llvm {

struct InstMarkerPass : PassInfoMixin<InstMarkerPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  
  // This pass is required for unsafe code tracking
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_INSTMARKER_INSTMARKER_H
