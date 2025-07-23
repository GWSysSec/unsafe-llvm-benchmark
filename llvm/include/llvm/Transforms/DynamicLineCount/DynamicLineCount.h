//===-- DynamicLineCount.h - Track unsafe source line coverage -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===--------------------------------------------------------------------------------===//
///
/// \file
/// This file declares the DynamicLineCount pass for tracking unsafe source
/// line coverage.
///
//===--------------------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_DYNAMICLINECOUNT_DYNAMICLINECOUNT_H
#define LLVM_TRANSFORMS_DYNAMICLINECOUNT_DYNAMICLINECOUNT_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Module;
}

namespace llvm {

extern const char *REGISTER_UNSAFE_LINE_FN;
extern const char *EXECUTE_UNSAFE_BLOCK_FN;
extern const char *DYNAMIC_LINE_PRINT_STATS_FN;

/// \brief Pass that tracks unsafe source line coverage using markers.
///
/// This pass uses marker instructions inserted by InstMarkerPass to identify
/// unsafe code blocks and the line metadata preserved by InstMarkerPass.
/// It registers unique source lines per unsafe block and tracks execution
/// coverage at runtime without relying on debug information in release builds.
class DynamicLineCountPass : public PassInfoMixin<DynamicLineCountPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_DYNAMICLINECOUNT_DYNAMICLINECOUNT_H