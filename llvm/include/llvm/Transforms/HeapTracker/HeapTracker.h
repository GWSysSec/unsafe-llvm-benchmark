//===-- HeapTracker.h - Track memory access to heap ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===-----------------------------------------------------------------------------===//
///
/// \file
/// This file declares the HeapTracker pass for tracking memory access to heap.
///
//===-----------------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_HEAPTRACKER_HEAPTRACKER_H
#define LLVM_TRANSFORMS_HEAPTRACKER_HEAPTRACKER_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Function;

extern const char *DYN_MEM_ACCESS_FN;
extern const char *DYN_UNSAFE_MEM_ACCESS_FN;

/// \brief Pass that tracks memory accesses to the heap.
///
/// This pass instruments memory instructions (loads and stores) to track
/// both general memory access and unsafe memory access within marked
/// unsafe code blocks. It inserts calls to runtime functions that can
/// analyze memory access patterns.
class HeapTrackerPass : public PassInfoMixin<HeapTrackerPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

  static bool isRequired() { return true; }
  static bool isPrimaryPackage();

};

} // namespace llvm

#endif // LLVM_TRANSFORMS_HEAPTRACKER_HEAPTRACKER_H