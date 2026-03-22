#include "llvm/Transforms/SVFAnalysis/UnsafeHeapInstrumentation.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "unsafe-heap-alloc"
#include <map>
#include <set>
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Transforms/InstMarker/InstMarker.h" 

using namespace llvm;

namespace {

// Helper: get a valid debug location.
// Some instructions lose their !dbg after optimization — scan nearby instructions.
DebugLoc getValidDebugLoc(Instruction *I) {
    if (I->getDebugLoc()) return I->getDebugLoc();
    
    for (auto It = I->getIterator(), End = I->getParent()->end(); It != End; ++It) {
        if (It->getDebugLoc()) return It->getDebugLoc();
    }
    for (auto It = I->getReverseIterator(), End = I->getParent()->rend(); It != End; ++It) {
        if (It->getDebugLoc()) return It->getDebugLoc();
    }
    
    if (auto *SP = I->getFunction()->getSubprogram()) {
        return DILocation::get(SP->getContext(), SP->getLine(), 0, SP);
    }
    return DebugLoc();
}

// Helper: Collect all SESE Region Markers in a function
void collectRegions(Function &F, std::vector<Instruction*> &Begins, std::vector<Instruction*> &Ends) {
    for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
            if (CallInst *CI = dyn_cast<CallInst>(&I)) {
                if (CI->isInlineAsm()) {
                    if (InlineAsm *IA = dyn_cast<InlineAsm>(CI->getCalledOperand())) {
                        if (IA->getAsmString() == UNSAFE_MARKER_BEGIN) {
                            Begins.push_back(&I);
                        } else if (IA->getAsmString() == UNSAFE_MARKER_END) {
                            Ends.push_back(&I);
                        }
                    }
                }
            }
        }
    }
}

// Helper: Check if an instruction is in a valid SESE region
bool isInstructionInUnsafeRegion(Instruction *I, const std::vector<Instruction*> &Begins, 
                                 const std::vector<Instruction*> &Ends, DominatorTree *DT, PostDominatorTree *PDT) {
    for (Instruction *Begin : Begins) {
        for (Instruction *End : Ends) {
            // For intra-block regions (which is what InstMarker creates), we can easily use `comesBefore`.
            if (Begin->getParent() == I->getParent() && End->getParent() == I->getParent()) {
                if (Begin->comesBefore(I) && I->comesBefore(End)) {
                    return true;
                }
            } else {
                // Cross block check (safeguard)
                if (DT->dominates(Begin, I) && PDT->dominates(End->getParent(), I->getParent())) {
                    return true;
                }
            }
        }
    }
    return false;
}

// Helper: Apply Allocation Instrumentation
bool instrumentAllocations(BasicBlock &BB, const UnsafeHeapAllocAnalysis::Result &AnalysisRes, 
                           FunctionCallee ReportAlloc, Type* PtrTy, Type* SizeTy, Type* IdTy) {
    bool Modified = false;
    std::vector<std::pair<Instruction*, NodeID>> AllocInsts;
    
    // Collect allocations
    for (Instruction &I : BB) {
        if (AnalysisRes.AllocationSites.count(&I)) {
            AllocInsts.push_back({&I, AnalysisRes.AllocationSites.find(&I)->second});
        }
    }

    // Instrument allocations
    for (auto &Pair : AllocInsts) {
        Instruction *I = Pair.first;
        NodeID id = Pair.second;
        uint64_t size = 0;
        if (AnalysisRes.HeapAllocSizes.count(id)) {
            size = AnalysisRes.HeapAllocSizes.find(id)->second;
        }

        IRBuilder<> B(I->getParent());
        if (auto *Invoke = dyn_cast<InvokeInst>(I)) {
            B.SetInsertPoint(&*Invoke->getNormalDest()->getFirstInsertionPt());
        } else {
            B.SetInsertPoint(I->getParent(), std::next(I->getIterator()));
        }

        Value *Ptr = I;
        if (!Ptr->getType()->isPointerTy()) {
            if (Ptr->getType()->isStructTy() && Ptr->getType()->getStructNumElements() > 0 && Ptr->getType()->getStructElementType(0)->isPointerTy()) {
                Ptr = B.CreateExtractValue(I, {0});
            }
        }

        if (Ptr->getType()->isPointerTy()) { 
             Value *VoidPtr = B.CreateBitCast(Ptr, PtrTy);
             Value *SizeVal = ConstantInt::get(SizeTy, size);
             
             // Try to sniff size from args if 0
             if (size == 0) {
                 if (auto *CI = dyn_cast<CallBase>(I)) {
                     StringRef FnName;
                     if (Function *CalledFn = CI->getCalledFunction()) {
                         FnName = CalledFn->getName();
                     }
                     if (FnName == "calloc" && CI->arg_size() >= 2) {
                         if (CI->getArgOperand(0)->getType()->isIntegerTy() && CI->getArgOperand(1)->getType()->isIntegerTy()) {
                             Value *Arg0 = B.CreateZExtOrTrunc(CI->getArgOperand(0), SizeTy);
                             Value *Arg1 = B.CreateZExtOrTrunc(CI->getArgOperand(1), SizeTy);
                             SizeVal = B.CreateMul(Arg0, Arg1);
                         }
                     } else if (FnName == "realloc" && CI->arg_size() >= 2) {
                         if (CI->getArgOperand(1)->getType()->isIntegerTy()) {
                             SizeVal = B.CreateZExtOrTrunc(CI->getArgOperand(1), SizeTy);
                         }
                     } else if (CI->arg_size() > 0 && CI->getArgOperand(0)->getType()->isIntegerTy()) {
                         SizeVal = B.CreateZExtOrTrunc(CI->getArgOperand(0), SizeTy);
                     }
                 }
             }
             
             B.CreateCall(ReportAlloc, {VoidPtr, SizeVal, ConstantInt::get(IdTy, id)});
             Modified = true;
        } else {
             LLVM_DEBUG(dbgs() << "SVF: Warning: Allocation returning non-pointer type: " << *Ptr << "\n");
        }
    }
    return Modified;
}

// Helper: Instrument deallocations
bool instrumentDeallocations(BasicBlock &BB, FunctionCallee ReportDealloc, Type* PtrTy) {
    bool Modified = false;
    std::vector<CallBase*> DeallocInsts;
    
    // Collect deallocations to prevent iterator invalidation
    for (Instruction &I : BB) {
        if (CallBase *CB = dyn_cast<CallBase>(&I)) {
            if (Function *F = CB->getCalledFunction()) {
                StringRef FnName = F->getName();
                if (FnName == "free" || FnName == "rust_dealloc" || FnName == "_ZdlPv" || FnName == "__rust_dealloc") {
                    if (CB->arg_size() > 0 && CB->getArgOperand(0)->getType()->isPointerTy()) {
                        DeallocInsts.push_back(CB);
                    }
                }
            }
        }
    }

    // Instrument deallocations
    for (CallBase *CB : DeallocInsts) {
        Value *FreedPtr = CB->getArgOperand(0);
        IRBuilder<> B(CB); // Insert BEFORE the free call
        B.SetCurrentDebugLocation(getValidDebugLoc(CB));
        Value *VoidPtr = B.CreateBitCast(FreedPtr, PtrTy);
        B.CreateCall(ReportDealloc, {VoidPtr});
        Modified = true;
    }
    return Modified;
}

// Helper: Instrument all SESE load/stores
bool instrumentUnsafeAccesses(BasicBlock &BB, const UnsafeHeapAllocAnalysis::Result &AnalysisRes,
                              const std::vector<Instruction*> &Begins, const std::vector<Instruction*> &Ends,
                              DominatorTree *DT, PostDominatorTree *PDT,
                              FunctionCallee CheckHeapAccess, FunctionCallee AnalyzeHeapObj,
                              Type *PtrTy, Type *BoolTy, Type *IdTy,
                              std::set<NodeID> &ModuleUnsafeTargets) {
    bool Modified = false;
    std::vector<Instruction*> UnsafeAccessInsts;
    std::map<Instruction*, std::vector<NodeID>> PredictedInsts;

    // Collect baseline accesses
    for (Instruction &I : BB) {
        if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
            if (isInstructionInUnsafeRegion(&I, Begins, Ends, DT, PDT)) {
                UnsafeAccessInsts.push_back(&I);
                
                if (AnalysisRes.UnsafePtrs.count(&I)) {
                    PredictedInsts[&I] = AnalysisRes.UnsafePtrs.find(&I)->second;
                }
            }
        }
    }

    // Instrument accesses
    for (Instruction *I : UnsafeAccessInsts) {
        IRBuilder<> B(I); // Insert BEFORE
        B.SetCurrentDebugLocation(getValidDebugLoc(I));
        Value *Ptr = nullptr;

        if (auto *LI = dyn_cast<LoadInst>(I)) Ptr = LI->getPointerOperand();
        else if (auto *SI = dyn_cast<StoreInst>(I)) Ptr = SI->getPointerOperand();
        
        if (Ptr) {
            Value *VoidPtr = B.CreateBitCast(Ptr, PtrTy);
            
            // IF SVF analyzed this instruction as aliasing a heap object, register the target objects
            if (PredictedInsts.count(I)) {
                for (NodeID targetId : PredictedInsts[I]) {
                    B.CreateCall(AnalyzeHeapObj, {VoidPtr, ConstantInt::get(IdTy, targetId)});
                    ModuleUnsafeTargets.insert(targetId);
                }
            }

            // Increment load/store access ONCE per instruction execution
            bool isLoad = isa<LoadInst>(I);
            Value *IsLoadVal = ConstantInt::get(BoolTy, isLoad);
            B.CreateCall(CheckHeapAccess, {VoidPtr, IsLoadVal});
            Modified = true;
        }
    }
    return Modified;
}

} // anonymous namespace

PreservedAnalyses UnsafeHeapInstrumentation::run(Module &M, ModuleAnalysisManager &AM) {
  // Get Analysis Results
  auto &AnalysisRes = AM.getResult<UnsafeHeapAllocAnalysis>(M);
  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  // Declare Runtime Hooks
  LLVMContext &Ctx = M.getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *PtrTy = PointerType::getUnqual(Ctx);
  Type *SizeTy = Type::getInt64Ty(Ctx); // usize
  Type *IdTy = Type::getInt64Ty(Ctx);   // u64
  Type *BoolTy = Type::getInt1Ty(Ctx);

  FunctionCallee ReportAlloc = M.getOrInsertFunction("__svf_report_alloc", VoidTy, PtrTy, SizeTy, IdTy);
  FunctionCallee ReportDealloc = M.getOrInsertFunction("__svf_report_dealloc", VoidTy, PtrTy);
  FunctionCallee CheckHeapAccess = M.getOrInsertFunction("__svf_check_heap_access", VoidTy, PtrTy, BoolTy);
  FunctionCallee AnalyzeHeapObj = M.getOrInsertFunction("__svf_analyze_heap_obj", VoidTy, PtrTy, IdTy);

  bool Modified = false;
  std::set<NodeID> ModuleUnsafeTargets;

  for (Function &F : M) {
    if (F.isDeclaration()) continue;
    // Skip SVF runtime hooks to prevent infinite recursion when instrumenting svf_runtime
    if (F.getName().starts_with("__svf_")) continue;

    DominatorTree *DT = &FAM.getResult<DominatorTreeAnalysis>(F);
    PostDominatorTree *PDT = &FAM.getResult<PostDominatorTreeAnalysis>(F);

    std::vector<Instruction*> UnsafeMarkerBegins;
    std::vector<Instruction*> UnsafeMarkerEnds;
    collectRegions(F, UnsafeMarkerBegins, UnsafeMarkerEnds);

    for (BasicBlock &BB : F) {
      Modified |= instrumentAllocations(BB, AnalysisRes, ReportAlloc, PtrTy, SizeTy, IdTy);
      Modified |= instrumentDeallocations(BB, ReportDealloc, PtrTy);
      Modified |= instrumentUnsafeAccesses(BB, AnalysisRes, UnsafeMarkerBegins, UnsafeMarkerEnds, DT, PDT,
                                           CheckHeapAccess, AnalyzeHeapObj, PtrTy, BoolTy, IdTy,
                                           ModuleUnsafeTargets);
    }
  }

  errs() << "[UnsafeHeapInstrumentation] Unique Static Unsafe Heap Targets in Regions: " 
         << ModuleUnsafeTargets.size() << "\n";

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
