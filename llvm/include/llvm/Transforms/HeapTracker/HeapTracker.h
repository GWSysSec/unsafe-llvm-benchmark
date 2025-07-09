#ifndef LLVM_TRANSFORMS_HEAPTRACKER_HEAPTRACKER_H
#define LLVM_TRANSFORMS_HEAPTRACKER_HEAPTRACKER_H

#include "llvm/IR/PassManager.h"

namespace llvm {

struct HeapTrackerPass : PassInfoMixin<HeapTrackerPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_HEAPTRACKER_HEAPTRACKER_H
