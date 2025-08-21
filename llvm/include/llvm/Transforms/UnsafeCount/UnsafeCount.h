//===-- UnsafeCount.h - Count unsafe instructions -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file declares the UnsafeCount pass for counting unsafe instructions.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UNSAFECOUNT_UNSAFECOUNT_H
#define LLVM_TRANSFORMS_UNSAFECOUNT_UNSAFECOUNT_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Function;
class Module;
class FunctionCallee;
}

namespace llvm {

extern const char *UPDATE_UNSAFE_COUNTER_FN;
extern const char *UPDATE_INSTRUCTION_COUNT_FN;
extern const char *RECORD_FUNCTION_EXEC_FN;
extern const char *PRINT_UNSAFE_STATS_FN;

/// \brief Pass that counts unsafe instructions and function calls.
///
/// This pass instruments unsafe code blocks marked by InstMarkerPass to count
/// unsafe instructions by type, total instruction count, and function calls.
class UnsafeCountPass : public PassInfoMixin<UnsafeCountPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  
  static bool isRequired() { return true; }

private:
  struct RuntimeContext {
    FunctionCallee UpdateCounterFn;
    FunctionCallee UpdateInstCountFn; 
    FunctionCallee RecordFuncFn;
    FunctionCallee PrintStatsFn;
  };
  
  RuntimeContext setupRuntimeFunctions(Module &M);
  bool instrumentUnsafeBlocks(Function &F, const RuntimeContext &Ctx);
  bool isFunctionUnsafe(const Function &F);
  bool shouldInstrumentFunction(const Function &F);
  bool isMarkerInstruction(const Instruction &I);
  bool hasUnsafeMetadata(const Instruction &I);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_UNSAFECOUNT_UNSAFECOUNT_H