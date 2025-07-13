#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DerivedTypes.h"
#include <cstring>
#include <cstdlib>

using namespace llvm;

namespace llvm {
const char *UNSAFE_MARKER_BEGIN = "nop # marker_begin";
const char *UNSAFE_MARKER_END = "nop # marker_end";
}

bool InstMarkerPass::isPrimaryPackage() {
  const char *p = getenv("CARGO_PRIMARY_PACKAGE");
  return p && strcmp(p, "1") == 0;
}

PreservedAnalyses InstMarkerPass::run(Function &F, FunctionAnalysisManager &AM) {
  if (!isPrimaryPackage())
    return PreservedAnalyses::all();

  bool Modified = false;

  auto *VoidTy = Type::getVoidTy(F.getContext());
  InlineAsm *AsmMarkerBegin = InlineAsm::get(FunctionType::get(VoidTy, false),
                                             UNSAFE_MARKER_BEGIN, "", true);
  InlineAsm *AsmMarkerEnd = InlineAsm::get(FunctionType::get(VoidTy, false),
                                           UNSAFE_MARKER_END, "", true);

  for (BasicBlock &BB : F) {
    Instruction *FirstUnsafeInst = nullptr;
    Instruction *LastUnsafeInst = nullptr;

    for (Instruction &I : BB) {
      if (I.getMetadata("unsafe_inst")) {
        if (!FirstUnsafeInst) {
          FirstUnsafeInst = &I;
        }
        LastUnsafeInst = &I;
      }
    }

    if (FirstUnsafeInst && LastUnsafeInst) {
      IRBuilder<> Builder(FirstUnsafeInst);
      Builder.CreateCall(AsmMarkerBegin);
      Modified = true;

      if (Instruction *NextInst = LastUnsafeInst->getNextNode()) {
        IRBuilder<> EndBuilder(NextInst);
        EndBuilder.CreateCall(AsmMarkerEnd);
      } else {
        IRBuilder<> EndBuilder(&BB);
        EndBuilder.SetInsertPoint(BB.getTerminator());
        EndBuilder.CreateCall(AsmMarkerEnd);
      }
    }
  }

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
