//===--------------InlineMarker.h ---------------------===//

// This the header file for the InlineMarker.cpp pass which will add an inline marker before and after unsafe instruction block.

//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UNSAFERUST_INLINE_MARKER_H
#define LLVM_TRANSFORMS_UNSAFERUST_INLINE_MARKER_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class InlineMarker : public PassInfoMixin<InlineMarker> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} 

#endif 