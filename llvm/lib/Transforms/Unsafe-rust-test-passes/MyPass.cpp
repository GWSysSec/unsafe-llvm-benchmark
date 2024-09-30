#include "llvm/Transforms/Unsafe-rust-test-passes/MyPass.h"

using namespace llvm;

PreservedAnalyses MyPass::run(Function &F,
                                      FunctionAnalysisManager &AM) {
  errs() << F.getName() << "\n";
  return PreservedAnalyses::all();
}