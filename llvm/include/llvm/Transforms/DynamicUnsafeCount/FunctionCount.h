//===--------------FunctionCount.h----------------===//
// This the header file for the FunctionCount.cpp pass which will measure the function behavior executed during runtime.

//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UNSAFERUST_MEASURE_FUNCTION_COUNT_H
#define LLVM_TRANSFORMS_UNSAFERUST_MEASURE_FUNCTION_COUNT_H
#include "llvm/IR/PassManager.h"
namespace llvm {
class FunctionCount : public PassInfoMixin<FunctionCount> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};
} // namespace llvm
#endif // LLVM_TRANSFORMS_UNSAFERUST_MEASURE