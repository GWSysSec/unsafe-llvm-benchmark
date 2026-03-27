#include "llvm/Transforms/SVFAnalysis/RuntimeAlias.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

static const char *CHECK_ALIAS_FN = "__svf_check_alias";

PreservedAnalyses RuntimeAliasPass::run(Module &M, ModuleAnalysisManager &AM) {
  // Get FunctionAnalysisManager to run DominatorTreeAnalysis
  FunctionAnalysisManager &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  LLVMContext &Ctx = M.getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  PointerType *PtrTy = PointerType::getUnqual(Ctx); 
  Type *Int32Ty = Type::getInt32Ty(Ctx);

  FunctionCallee CheckFn = M.getOrInsertFunction(
      CHECK_ALIAS_FN, FunctionType::get(VoidTy, {PtrTy, PtrTy, Int32Ty}, false));

  bool Modified = false;
  uint32_t CheckID = 0;

  for (Function &F : M) {
    if (F.isDeclaration() || F.isIntrinsic())
      continue;

    // Candidates: [Instruction*, Value*]
    // Value* is the pointer operand.
    SmallVector<std::pair<Instruction*, Value*>, 16> Candidates;

    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        // Filter 1: Must have "unsafe_inst" metadata
        // This metadata is expected to be inserted by the custom rustc or front-end.
        if (!I.getMetadata("unsafe_inst"))
          continue;

        Value *PtrOp = nullptr;
        if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
          PtrOp = LI->getPointerOperand();
        } else if (StoreInst *SI = dyn_cast<StoreInst>(&I)) {
          PtrOp = SI->getPointerOperand();
        } else {
          continue;
        }

        // Filter 2: Ignore Stack Allocations (match SVF Heap-Only focus)
        // getUnderlyingObject digs through GEPs/bitcasts to find the base.
        // If the base is an AllocaInst, it is strictly stack memory.
        if (isa<AllocaInst>(getUnderlyingObject(PtrOp))) {
            continue;
        }

        Candidates.push_back({&I, PtrOp});
      }
    }

    if (Candidates.size() < 2)
      continue;

    // Get DominatorTree for this function
    DominatorTree &DT = FAM.getResult<DominatorTreeAnalysis>(F);

    // Pairwise Check
    for (size_t i = 0; i < Candidates.size(); ++i) {
      for (size_t j = i + 1; j < Candidates.size(); ++j) {
        Instruction *InstA = Candidates[i].first;
        Value *PtrA = Candidates[i].second;

        Instruction *InstB = Candidates[j].first;
        Value *PtrB = Candidates[j].second;

        Instruction *InsertPt = nullptr;

        // Logic: To check Alias(PtrA, PtrB), both pointers must be available.
        // Using Dominance to determine a safe insertion point.
        // If InstA dominates InstB, execution passes A then B. 
        // We can check at B (PtrA is alive/available due to dominance).
        // Conversely for B dominating A.
        // If neither dominates, they are likely in parallel branches or disjoint paths,
        // so we skip checking them (conservative approach for runtime instrumentation).
        
        if (DT.dominates(InstA, InstB)) {
             InsertPt = InstB;
        } else if (DT.dominates(InstB, InstA)) {
             InsertPt = InstA;
        }
        
        if (InsertPt) {
            IRBuilder<> Builder(InsertPt);
            // __svf_check_alias(p, q, id++)
            Builder.CreateCall(CheckFn, {PtrA, PtrB, ConstantInt::get(Int32Ty, CheckID++)});
            Modified = true;
        }
      }
    }
  }

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
