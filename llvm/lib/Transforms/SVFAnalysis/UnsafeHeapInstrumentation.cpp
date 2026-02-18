#include "llvm/Transforms/SVFAnalysis/UnsafeHeapInstrumentation.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
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
                 errs() << "SVF: Valid SESE Region found in " << F.getName() << "\n";
                 Matched = true;
                 break; 
             }
        }
        if (!Matched) {
             errs() << "SVF: Invalid SESE Region or Unmatched Marker in " << F.getName() << "\n";
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
                 if (DT->dominates(R.Begin, &I) && PDT->dominates(R.End, &I)) {
                     // Inside Region
                     AccessInsts.push_back({&I, AnalysisRes.UnsafePtrs[&I][0]}); // Pick first target
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

          IRBuilder<> B(I->getNextNode()); // Insert AFTER
          Value *Ptr = I;
          if (Ptr->getType()->isPointerTy()) { // Ensure it is pointer
               Value *VoidPtr = B.CreateBitCast(Ptr, PtrTy);
               Value *SizeVal = ConstantInt::get(SizeTy, size);
               
               // Try to sniff size from args if 0
               if (size == 0) {
                   if (auto *CI = dyn_cast<CallBase>(I)) {
                       if (CI->arg_size() > 0 && CI->getArgOperand(0)->getType()->isIntegerTy()) {
                           SizeVal = B.CreateZExtOrTrunc(CI->getArgOperand(0), SizeTy);
                       }
                   }
               }
               
               B.CreateCall(ReportAlloc, {VoidPtr, SizeVal, ConstantInt::get(IdTy, id)});
               Modified = true;
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
              Modified = true;
          }
      }
    }
  }

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
