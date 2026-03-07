#include "llvm/Transforms/SVFAnalysis/UnsafeHeapInstrumentation.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "unsafe-heap-alloc"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Transforms/InstMarker/InstMarker.h" 

using namespace llvm;

PreservedAnalyses UnsafeHeapInstrumentation::run(Module &M, ModuleAnalysisManager &AM) {
  // Check CARGO_PRIMARY_PACKAGE
  const char *EnvPackage = std::getenv("CARGO_PRIMARY_PACKAGE");
  if (!EnvPackage || std::strcmp(EnvPackage, "1") != 0) {
    return PreservedAnalyses::all();
  }

  // Get Analysis Results
  auto &AnalysisRes = AM.getResult<UnsafeHeapAllocAnalysis>(M);
  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  // Declare Runtime Hooks
  LLVMContext &Ctx = M.getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *PtrTy = PointerType::getUnqual(Ctx);
  Type *SizeTy = Type::getInt64Ty(Ctx); // usize
  Type *IdTy = Type::getInt64Ty(Ctx);   // u64

  FunctionCallee ReportAlloc = M.getOrInsertFunction("__svf_report_alloc", VoidTy, PtrTy, SizeTy, IdTy);
  FunctionCallee CheckHeap = M.getOrInsertFunction("__svf_check_heap", VoidTy, PtrTy, IdTy);
  // unsafe heap access hook: same signature as HeapTrackerPass's dyn_unsafe_mem_access(ptr, is_load)
  Type *BoolTy = Type::getInt1Ty(Ctx);
  FunctionCallee UnsafeHeapAccess = M.getOrInsertFunction("__svf_unsafe_heap_access", VoidTy, PtrTy, BoolTy);
  // Dealloc hook if needed, but not using for now in instrumentation loop (Dealloc usually handled by FreeInst check? But Rust uses Drop glue)
  // For now we only instrument explicitly identified Heap Allocations for tracking.

  bool Modified = false;

  for (Function &F : M) {
    if (F.isDeclaration()) continue;

    DominatorTree *DT = &FAM.getResult<DominatorTreeAnalysis>(F);
    PostDominatorTree *PDT = &FAM.getResult<PostDominatorTreeAnalysis>(F);

    // 1. Find Markers
    std::vector<CallInst*> BeginMarkers;
    std::vector<CallInst*> EndMarkers;

    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (CallInst *CI = dyn_cast<CallInst>(&I)) {
          if (CI->isInlineAsm()) {
            if (InlineAsm *IA = dyn_cast<InlineAsm>(CI->getCalledOperand())) {
              if (IA->getAsmString() == UNSAFE_MARKER_BEGIN) {
                BeginMarkers.push_back(CI);
              } else if (IA->getAsmString() == UNSAFE_MARKER_END) {
                EndMarkers.push_back(CI);
              }
            }
          }
        }
      }
    }

    // 3. Compute Valid Regions (SESE)
    struct Region { CallInst *Begin; CallInst *End; };
    std::vector<Region> ValidRegions;

    for (CallInst *Begin : BeginMarkers) {
        bool Matched = false;
        for (CallInst *End : EndMarkers) {
             // Check Strict Dominance and PostDominance
             if (DT->dominates(Begin, End) && PDT->dominates(End, Begin)) {
                 ValidRegions.push_back({Begin, End});
                 LLVM_DEBUG(dbgs() << "SVF: Valid SESE Region found in " << F.getName() << "\n");
                 Matched = true;
                 break; 
             }
        }
        if (!Matched) {
             LLVM_DEBUG(dbgs() << "SVF: Invalid SESE Region or Unmatched Marker in " << F.getName() << "\n");
        }
    }

    // 3. Instrument
    for (BasicBlock &BB : F) {
      // Collect instructions to instrument to avoid iterator invalidation
      std::vector<std::pair<Instruction*, NodeID>> AllocInsts;
      std::vector<std::pair<Instruction*, NodeID>> AccessInsts;

      for (Instruction &I : BB) {
          // Identify Allocations
          if (AnalysisRes.AllocationSites.count(&I)) {
             AllocInsts.push_back({&I, AnalysisRes.AllocationSites[&I]});
          }

              // Identify Unsafe Accesses
          if (AnalysisRes.UnsafePtrs.count(&I)) {
             // Check membership in Valid Regions
             for (const auto &R : ValidRegions) {
                 if (DT->dominates(R.Begin->getParent(), I.getParent()) && PDT->dominates(R.End->getParent(), I.getParent())) {
                     // Inside Region
                     for (NodeID targetId : AnalysisRes.UnsafePtrs[&I]) {
                         AccessInsts.push_back({&I, targetId});
                     }
                     break;
                 }
             }
          }
      }

      // Apply Instrumentation
      for (auto &Pair : AllocInsts) {
          Instruction *I = Pair.first;
          NodeID id = Pair.second;
          uint64_t size = AnalysisRes.HeapAllocSizes[id];

          IRBuilder<> B(I->getParent(), std::next(I->getIterator())); // Insert AFTER
          Value *Ptr = I;
          if (Ptr->getType()->isPointerTy()) { // Ensure it is pointer
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

      for (auto &Pair : AccessInsts) {
          Instruction *I = Pair.first;
          NodeID targetId = Pair.second;

          IRBuilder<> B(I); // Insert BEFORE
          Value *Ptr = nullptr;
          if (auto *LI = dyn_cast<LoadInst>(I)) Ptr = LI->getPointerOperand();
          else if (auto *SI = dyn_cast<StoreInst>(I)) Ptr = SI->getPointerOperand();
          
          if (Ptr) {
              Value *VoidPtr = B.CreateBitCast(Ptr, PtrTy);
              B.CreateCall(CheckHeap, {VoidPtr, ConstantInt::get(IdTy, targetId)});
              // also inject unsafe heap access counter (same sig as dyn_unsafe_mem_access)
              bool isLoad = isa<LoadInst>(I);
              Value *IsLoadVal = ConstantInt::get(BoolTy, isLoad);
              B.CreateCall(UnsafeHeapAccess, {VoidPtr, IsLoadVal});
              Modified = true;
          }
      }
    }
  }

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
