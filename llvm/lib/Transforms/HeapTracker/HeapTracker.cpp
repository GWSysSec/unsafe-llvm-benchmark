#include "llvm/Transforms/HeapTracker/HeapTracker.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallVector.h"

static const char *UNSAFE_MARKER_BEGIN = "nop # marker_begin";
static const char *UNSAFE_MARKER_END = "nop # marker_end";
static const char *DYN_MEM_ACCESS_FN = "dyn_mem_access";
static const char *DYN_UNSAFE_MEM_ACCESS_FN = "dyn_unsafe_mem_access";

using namespace llvm;

static void instrumentMemInst(Function &F, FunctionCallee dynMemAccessFn) {
  for (BasicBlock &BB : F) {
    SmallVector<Instruction*, 8> memInsts;
    for (Instruction &I : BB) {
      if (isa<LoadInst>(&I) || isa<StoreInst>(&I)) {
        memInsts.push_back(&I);
      }
    }

    for (Instruction *memInst : memInsts) {
      Value *destAddr = isa<LoadInst>(memInst) ?
         cast<LoadInst>(memInst)->getPointerOperand() :
         cast<StoreInst>(memInst)->getPointerOperand();
      CallInst::Create(dynMemAccessFn, destAddr, "", memInst);
    }
  }
}

static void instrumentUnsafeMemInst(Function &F, FunctionCallee dynUnsafeMemAccessFn) {
  for (BasicBlock &BB : F) {
    bool unsafeBlockStarted = false;
    SmallVector<Instruction *, 8> unsafeMemInsts;

    for (Instruction &I : BB) {
      if (unsafeBlockStarted && (isa<LoadInst>(&I) || isa<StoreInst>(&I))) {
        unsafeMemInsts.push_back(&I);
        continue;
      }

      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (auto *IA = dyn_cast<InlineAsm>(CI->getCalledOperand()->stripPointerCasts())) {
          StringRef AsmStr = IA->getAsmString();
          if (AsmStr.contains("marker_begin")) {
            unsafeBlockStarted = true;
          } else if (AsmStr.contains("marker_end")) {
            unsafeBlockStarted = false;
          }
        }
      }
    }

    for (Instruction *memInst : unsafeMemInsts) {
      bool isLoad = isa<LoadInst>(memInst);
      Value *destAddr = isLoad ? cast<LoadInst>(memInst)->getPointerOperand() :
                                 cast<StoreInst>(memInst)->getPointerOperand();
      Value *isLoadVal = ConstantInt::get(Type::getInt1Ty(F.getContext()), isLoad);
      CallInst::Create(dynUnsafeMemAccessFn, {destAddr, isLoadVal}, "", memInst);
    }
  }
}

PreservedAnalyses HeapTrackerPass::run(Function &F, FunctionAnalysisManager &AM) {
  LLVMContext &C = F.getContext();
  Module *M = F.getParent();
  Type *voidTy = Type::getVoidTy(C);
  Type *rawPtrTy = PointerType::getUnqual(Type::getInt8Ty(C));
  Type *booleanTy = Type::getInt1Ty(C);
  
  FunctionType *dynMemAccessFnTy = FunctionType::get(voidTy, rawPtrTy, false);
  FunctionCallee dynMemAccessFn = M->getOrInsertFunction(
    DYN_MEM_ACCESS_FN, dynMemAccessFnTy);
  
  FunctionType *dynUnsafeMemAccessFnTy = FunctionType::get(
    voidTy, {rawPtrTy, booleanTy}, false);
  FunctionCallee dynUnsafeMemAccessFn = M->getOrInsertFunction(
    DYN_UNSAFE_MEM_ACCESS_FN, dynUnsafeMemAccessFnTy);

  instrumentMemInst(F, dynMemAccessFn);
  instrumentUnsafeMemInst(F, dynUnsafeMemAccessFn);
  
  return PreservedAnalyses::all();
}