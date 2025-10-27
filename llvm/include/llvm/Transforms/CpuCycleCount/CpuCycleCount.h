//===-- CpuCycleCount.h - Track unsafe instruction execution time -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------------------===//
///
/// \file
/// This file declares the CpuCycleCount pass for tracking unsafe instruction
/// execution time.
///
//===---------------------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_CPUCYCLECOUNT_CPUCYCLECOUNT_H
#define LLVM_TRANSFORMS_CPUCYCLECOUNT_CPUCYCLECOUNT_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Module;

// Runtime function names
extern const char *PROGRAM_START_FN;
extern const char *THREAD_START_FN;
extern const char *START_MEASUREMENT_FN;
extern const char *END_MEASUREMENT_FN;
extern const char *PRINT_STATS_FN;
extern const char *EXTERNAL_CALL_START_FN;
extern const char *EXTERNAL_CALL_END_FN;

/// \brief Pass that tracks CPU cycle count for unsafe instruction execution.
///
/// This pass instruments unsafe code blocks marked by InstMarkerPass to measure
/// CPU cycles and also tracks time spent in external library calls. It inserts
/// calls to runtime functions at the beginning and end of unsafe blocks and
/// around external function calls, and registers a destructor to print
/// statistics at program exit.
class CpuCycleCountPass : public PassInfoMixin<CpuCycleCountPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_CPUCYCLECOUNT_CPUCYCLECOUNT_H