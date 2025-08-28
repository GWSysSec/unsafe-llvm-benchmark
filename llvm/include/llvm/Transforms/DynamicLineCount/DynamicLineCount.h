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
/// Phase 1: Registration - collect unique unsafe lines, generate constructor
/// Phase 2: Execution - insert tracking calls at unsafe instructions
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_DYNAMICLINECOUNT_DYNAMICLINECOUNT_H
#define LLVM_TRANSFORMS_DYNAMICLINECOUNT_DYNAMICLINECOUNT_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Function;
}

namespace llvm {

/// \brief FunctionPass that tracks unsafe source line coverage using markers.
///
/// This pass uses marker instructions to identify unsafe code regions and tracks
/// only instructions with !unsafe_inst metadata. Uses a two-phase approach:
/// Phase 1: Collects unique unsafe lines and generates registration constructor
/// Phase 2: Inserts execution tracking calls at each unsafe instruction
/// Relies on marker pairs - no longer needs isPrimaryPackage logic.
class DynamicLineCountPass : public PassInfoMixin<DynamicLineCountPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_DYNAMICLINECOUNT_DYNAMICLINECOUNT_H