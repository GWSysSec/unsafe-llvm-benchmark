#ifndef LLVM_TRANSFORMS_RUNTIMEALIAS_RUNTIMEALIAS_H
#define LLVM_TRANSFORMS_RUNTIMEALIAS_RUNTIMEALIAS_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class RuntimeAliasPass : public PassInfoMixin<RuntimeAliasPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_RUNTIMEALIAS_RUNTIMEALIAS_H
