//===-- UnsafeCount.h - Count unsafe instructions -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file declares the UnsafeCount pass for counting unsafe instructions.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UNSAFECOUNT_UNSAFECOUNT_H
#define LLVM_TRANSFORMS_UNSAFECOUNT_UNSAFECOUNT_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Function;
class Module;
class FunctionCallee;
}

namespace llvm {

/// \brief Count unsafe instructions and function execution.
///
/// Instruments functions to track unsafe instruction counts by type,
/// total instruction counts, and function call statistics.
struct UnsafeCountPass : public PassInfoMixin<UnsafeCountPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

  static bool isRequired() { return true; }
  static bool isPrimaryPackage();

};

} // namespace llvm

#endif // LLVM_TRANSFORMS_UNSAFECOUNT_UNSAFECOUNT_H