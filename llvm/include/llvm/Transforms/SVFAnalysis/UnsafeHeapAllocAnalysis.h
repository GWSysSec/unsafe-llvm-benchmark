#ifndef LLVM_TRANSFORMS_SVFANALYSIS_UNSAFEHEAPALLOCANALYSIS_H
#define LLVM_TRANSFORMS_SVFANALYSIS_UNSAFEHEAPALLOCANALYSIS_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Module;

class UnsafeHeapAllocAnalysis : public PassInfoMixin<UnsafeHeapAllocAnalysis> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_SVFANALYSIS_UNSAFEHEAPALLOCANALYSIS_H
