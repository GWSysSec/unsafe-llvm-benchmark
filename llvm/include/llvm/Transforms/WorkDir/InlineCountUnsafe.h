//===--------------InlineCountUnsafe.h----------------===//
// This the header file for the InlineCountUnsafe.cpp pass which will count the unsafe instructions executed during runtime.

//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UNSAFERUST_INLINE_COUNT_MARKER_H
#define LLVM_TRANSFORMS_UNSAFERUST_INLINE_COUNT_MARKER_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class InlineCountUnsafe : public PassInfoMixin<InlineCountUnsafe> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} 

#endif 