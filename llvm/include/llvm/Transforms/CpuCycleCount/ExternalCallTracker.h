//===-- ExternalCallTracker.h - Track external function call time -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===-------------------------------------------------------------------------===//
///
/// \file
/// This file declares the ExternalCallTracker pass for tracking time spent in
/// external library function calls.
///
//===-------------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_CPUCYCLECOUNT_EXTERNALCALLTRACKER_H
#define LLVM_TRANSFORMS_CPUCYCLECOUNT_EXTERNALCALLTRACKER_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Module;

// Runtime function names for external call tracking
extern const char *EXTERNAL_CALL_START_FN;
extern const char *EXTERNAL_CALL_END_FN;

/// \brief Pass that tracks time spent in external function calls.
///
/// This pass instruments calls to external (non-instrumented) functions to
/// measure the time spent outside the instrumented code. It inserts calls to
/// runtime functions at the beginning and end of each external function call,
/// and adds memory fences to ensure accurate timing measurements.
///
/// External calls made from within unsafe blocks are handled by the runtime,
/// which tracks whether the call originated from safe or unsafe code.
class ExternalCallTrackerPass : public PassInfoMixin<ExternalCallTrackerPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_CPUCYCLECOUNT_EXTERNALCALLTRACKER_H
