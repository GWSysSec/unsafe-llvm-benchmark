#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdlib>

using namespace llvm;

namespace llvm {
const char *UNSAFE_MARKER_BEGIN = "nop # marker_begin";
const char *UNSAFE_MARKER_END = "nop # marker_end";
}

bool InstMarkerPass::isPrimaryPackage() {
  const char *p = std::getenv("CARGO_PRIMARY_PACKAGE");
  return p && std::strcmp(p, "1") == 0;
}

PreservedAnalyses InstMarkerPass::run(Function &F, FunctionAnalysisManager &AM) {
  if (F.isDeclaration() || !isPrimaryPackage())
    return PreservedAnalyses::all();

  std::map<BasicBlock*, std::vector<Instruction*>> UnsafeInstsByBlock;
  int TotalUnsafeInst = 0;

  for (Instruction &I : instructions(F)) {
    if (I.getMetadata("unsafe_inst")) {
      UnsafeInstsByBlock[I.getParent()].push_back(&I);
      TotalUnsafeInst++;
    }
  }

  if (TotalUnsafeInst == 0)
    return PreservedAnalyses::all();

  for (auto &BlockEntry : UnsafeInstsByBlock) {
    std::vector<Instruction*> &BlockUnsafeInsts = BlockEntry.second;
    std::sort(BlockUnsafeInsts.begin(), BlockUnsafeInsts.end(),
      [](Instruction *A, Instruction *B) {
        return A->comesBefore(B);
      });
  }

  Module *M = F.getParent();
  LLVMContext &Ctx = M->getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  InlineAsm *AsmMarkerBegin = InlineAsm::get(FunctionType::get(VoidTy, false),
                                             UNSAFE_MARKER_BEGIN, 
                                             "~{memory}", true);
  InlineAsm *AsmMarkerEnd = InlineAsm::get(FunctionType::get(VoidTy, false),
                                           UNSAFE_MARKER_END, 
                                           "~{memory}", true);
  bool Modified = false;

  for (auto &BlockEntry : UnsafeInstsByBlock) {
    BasicBlock *BB = BlockEntry.first;
    std::vector<Instruction*> &BlockUnsafeInsts = BlockEntry.second;
    
    if (BlockUnsafeInsts.empty())
      continue;
      
    Instruction *FirstUnsafe = BlockUnsafeInsts.front();
    Instruction *LastUnsafe = BlockUnsafeInsts.back();
    
    IRBuilder<> Builder(FirstUnsafe);
    Builder.CreateCall(AsmMarkerBegin);
    Modified = true;
    
    if (Instruction *NextInst = LastUnsafe->getNextNode()) {
      IRBuilder<> EndBuilder(NextInst);
      EndBuilder.CreateCall(AsmMarkerEnd);
    } else {
      IRBuilder<> EndBuilder(BB);
      EndBuilder.SetInsertPoint(BB->getTerminator());
      EndBuilder.CreateCall(AsmMarkerEnd);
    }
  }
  
  if (UnsafeInstsByBlock.size() > 0 && TotalUnsafeInst > 0) {
    errs() << "[InstMarker] " << F.getName() 
           << " - " << TotalUnsafeInst << " unsafe instrs, " 
           << UnsafeInstsByBlock.size() << " blocks\n";
  }
  
  if (Modified) {
    PreservedAnalyses PA;
    PA.preserveSet<AllAnalysesOn<Function>>();
    return PA;
  }
  
  return PreservedAnalyses::all();
}