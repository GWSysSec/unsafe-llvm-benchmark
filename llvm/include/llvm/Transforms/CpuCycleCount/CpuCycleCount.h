//===-- CpuCycleCount.h - Track unsafe instruction execution time -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------------------===//
///
/// \file
/// This file declares the CpuCycleCount pass for tracking CPU cycles spent in
/// unsafe code blocks.
///
//===---------------------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_CPUCYCLECOUNT_CPUCYCLECOUNT_H
#define LLVM_TRANSFORMS_CPUCYCLECOUNT_CPUCYCLECOUNT_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Module;

// Runtime function names
extern const char *PROGRAM_START_FN;
extern const char *START_MEASUREMENT_FN;
extern const char *END_MEASUREMENT_FN;
extern const char *PRINT_STATS_FN;

/// \brief Pass that tracks CPU cycles spent executing unsafe code blocks.
///
/// This pass instruments unsafe code blocks marked by InstMarkerPass to measure
/// CPU cycles. It inserts calls to runtime functions at the beginning and end
/// of unsafe blocks, adds memory fences for accurate timing, and registers a
/// destructor to print statistics at program exit.
class CpuCycleCountPass : public PassInfoMixin<CpuCycleCountPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_CPUCYCLECOUNT_CPUCYCLECOUNT_H