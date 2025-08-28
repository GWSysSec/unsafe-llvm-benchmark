//===-- DynamicLineCount.h - Track unsafe source line coverage -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file declares the DynamicLineCount pass for tracking unsafe source
/// line coverage using a two-phase approach:
/// Phase 1: Compile-time - Collect all unsafe lines across the module
/// Phase 2: Runtime - Track which lines actually execute
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_DYNAMICLINECOUNT_DYNAMICLINECOUNT_H
#define LLVM_TRANSFORMS_DYNAMICLINECOUNT_DYNAMICLINECOUNT_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Module;

/// \brief ModulePass that tracks unsafe source line coverage.
///
/// This pass operates in two phases:
/// 1. Compile-time: Collects ALL unsafe lines across the entire module
///    and creates a module constructor to register them at program startup
/// 2. Runtime: Instruments unsafe instructions to track execution
///
/// Using a ModulePass ensures we can:
/// - See all functions at once to collect complete line information
/// - Create a module constructor that runs before main()
/// - Guarantee all lines are registered before any execution tracking
class DynamicLineCountPass : public PassInfoMixin<DynamicLineCountPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_DYNAMICLINECOUNT_DYNAMICLINECOUNT_H