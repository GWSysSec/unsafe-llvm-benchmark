#include "llvm/Transforms/RuntimeAlias/RuntimeAlias.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

static const char *CHECK_ALIAS_FN = "__svf_check_alias";

PreservedAnalyses RuntimeAliasPass::run(Module &M, ModuleAnalysisManager &AM) {
  // Get or insert the runtime check function: 
  // void __svf_check_alias(i8* p, i8* q, i32 id)
  LLVMContext &Ctx = M.getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  PointerType *PtrTy = PointerType::getUnqual(Ctx); // Opaque pointer
  Type *Int32Ty = Type::getInt32Ty(Ctx);

  FunctionCallee CheckFn = M.getOrInsertFunction(
      CHECK_ALIAS_FN, FunctionType::get(VoidTy, {PtrTy, PtrTy, Int32Ty}, false));

  bool Modified = false;
  uint32_t CheckID = 0;

  for (Function &F : M) {
    if (F.isDeclaration() || F.isIntrinsic())
      continue;

    // Collect pointer arguments
    SmallVector<Value *, 8> PtrArgs;
    for (Argument &Arg : F.args()) {
      if (Arg.getType()->isPointerTy()) {
        PtrArgs.push_back(&Arg);
      }
    }

    if (PtrArgs.size() < 2)
      continue;

    // Inject checks at the entry block
    IRBuilder<> Builder(&*F.getEntryBlock().getFirstInsertionPt());

    // Pairwise check of pointer arguments
    // For now, check all pairs (n*(n-1)/2)
    for (size_t i = 0; i < PtrArgs.size(); ++i) {
      for (size_t j = i + 1; j < PtrArgs.size(); ++j) {
        Value *P = PtrArgs[i];
        Value *Q = PtrArgs[j];
        
        // __svf_check_alias(p, q, id++)
        Builder.CreateCall(CheckFn, {P, Q, ConstantInt::get(Int32Ty, CheckID++)});
        Modified = true;
      }
    }
  }

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
