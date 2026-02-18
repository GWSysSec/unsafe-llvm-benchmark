#ifndef LLVM_TRANSFORMS_SVFANALYSIS_UNSAFEHEAPINSTRUMENTATION_H
#define LLVM_TRANSFORMS_SVFANALYSIS_UNSAFEHEAPINSTRUMENTATION_H

#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/SVFAnalysis/UnsafeHeapAllocAnalysis.h"

namespace llvm {

class UnsafeHeapInstrumentation : public PassInfoMixin<UnsafeHeapInstrumentation> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_SVFANALYSIS_UNSAFEHEAPINSTRUMENTATION_H
