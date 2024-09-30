#ifndef LLVM_TRANSFORMS_LINECOUNT_H
#define LLVM_TRANSFORMS_LINECOUNT_H

#include "llvm/IR/PassManager.h"
#include "llvm/Analysis/InstCount.h"

namespace llvm {

class LineCount : public PassInfoMixin<LineCount> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_LINECOUNTNEW_LINECOUNT_H