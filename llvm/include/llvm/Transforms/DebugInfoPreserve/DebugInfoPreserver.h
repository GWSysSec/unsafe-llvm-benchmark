//===- DEBUGINFOPRESERVER_H - Preserve Debug Metadata -----------*- C++ -*-===//
#ifndef LLVM_TRANSFORMS_UTILS_DEBUGINFOPRESERVER_H
#define LLVM_TRANSFORMS_UTILS_DEBUGINFOPRESERVER_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class DebugInfoPreserverPass : public PassInfoMixin<DebugInfoPreserverPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif