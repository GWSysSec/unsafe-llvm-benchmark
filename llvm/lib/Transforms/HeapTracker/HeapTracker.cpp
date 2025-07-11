#include "llvm/Transforms/HeapTracker/HeapTracker.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Constants.h"

static const char *DYN_MEM_ACCESS_FN        = "dyn_mem_access";
static const char *DYN_UNSAFE_MEM_ACCESS_FN = "dyn_unsafe_mem_access";

using namespace llvm;

/// @brief Add a call to dyn_mem_access() before each memory instruction.
/// @param F The target function.
/// @param dynMemAccessFn The to-be-inserted callee.
static void instrumentMemInst(Function &F, FunctionCallee dynMemAccessFn) {
  for (BasicBlock &BB : F) {
    SmallVector<Instruction*, 8> memInsts;
    for (Instruction &I : BB) {
      Instruction *Inst = &I;
      if (isa<LoadInst>(Inst) || isa<StoreInst>(Inst)) {
        memInsts.push_back(Inst);
      }
    }

    // Insert a call to dyn_mem_access() before each memory instruction.
    for (Instruction *memInst : memInsts) {
      Value *destAddr = isa<LoadInst>(memInst) ?
         cast<LoadInst>(memInst)->getPointerOperand() :
         cast<StoreInst>(memInst)->getPointerOperand();
      CallInst::Create(dynMemAccessFn, destAddr, "", memInst);
    }
  }
}

/// @brief Add a call to dyn_unsafe_mem_access() before each unsafe memory instruction.
/// @param F The target function.
/// @param dynUnsafeMemAccessFn The to-be-inserted callee.
static void instrumentUnsafeMemInst(Function &F, FunctionCallee dynUnsafeMemAccessFn) {
  for (BasicBlock &BB : F) {
    // Indicating whether the the pass has entered into an unsafe block.
    bool unsafeBlockStarted = false;
    SmallVector<Instruction *, 8> unsafeMemInsts;

    for (Instruction &I : BB) {
      Instruction *Inst = &I;
      // Collect memory instructions.
      if (unsafeBlockStarted && (isa<LoadInst>(I) || isa<StoreInst>(I))) {
        unsafeMemInsts.push_back(Inst);
        continue;
      }

      if (CallInst *CI = dyn_cast<CallInst>(&I)) {
        if (InlineAsm *IA = dyn_cast<InlineAsm>(CI->getCalledOperand())) {
          StringRef AsmStr = IA->getAsmString();
          if (AsmStr == UNSAFE_MARKER_BEGIN) {
            if (unsafeBlockStarted) {
              // Nested marker_begin - skip
              continue;
            }
            unsafeBlockStarted = true;
          } else if (AsmStr == UNSAFE_MARKER_END) {
            if (!unsafeBlockStarted) {
              // Unmatched marker_end - skip
              continue;
            }
            unsafeBlockStarted = false;
          }
        }
      }
    }

    // Insert a call to dyn_unsafe_mem_access() before each unsafe memory instruction.
    for (Instruction *memInst : unsafeMemInsts) {
      bool isLoad = isa<LoadInst>(memInst);
      Value *destAddr = isLoad ? cast<LoadInst>(memInst)->getPointerOperand() :
                                 cast<StoreInst>(memInst)->getPointerOperand();
      Value *isLoadVal = ConstantInt::get(Type::getInt1Ty(F.getContext()), isLoad);
      CallInst::Create(dynUnsafeMemAccessFn, {destAddr, isLoadVal}, "", memInst);
    }
  }
}

PreservedAnalyses HeapTrackerPass::run(Function &F,
                                                    FunctionAnalysisManager &AM) {
  // Define fn prototypes of dyn_mem_access() and dyn_unsafe_mem_access()
  // defined in the Rust runlib lib.
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

  // First, insert calls to dyn_mem_access
  instrumentMemInst(F, dynMemAccessFn);

  // Then, insert calls to dyn_unsafe_mem_access
  instrumentUnsafeMemInst(F, dynUnsafeMemAccessFn);
  
  return PreservedAnalyses::all();
}