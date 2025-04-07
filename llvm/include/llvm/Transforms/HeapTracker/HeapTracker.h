//===------ HeapTracker.h - Tracking memory access to heap ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_HEAPTRACKER_HEAPTRACKER_H
#define LLVM_HEAPTRACKER_HEAPTRACKER_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class HeapTrackerPass : public PassInfoMixin<HeapTrackerPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm

#endif //  LLVM_HEAPTRACKER_HEAPTRACKER_H
