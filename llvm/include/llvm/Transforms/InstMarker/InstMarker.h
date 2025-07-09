#ifndef LLVM_TRANSFORMS_INSTMARKER_INSTMARKER_H
#define LLVM_TRANSFORMS_INSTMARKER_INSTMARKER_H

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Function.h"

namespace llvm {

extern const char *UNSAFE_MARKER_BEGIN;
extern const char *UNSAFE_MARKER_END;

struct InstMarkerPass : PassInfoMixin<InstMarkerPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
private:
  static bool isPrimaryPackage();
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_INSTMARKER_INSTMARKER_H
