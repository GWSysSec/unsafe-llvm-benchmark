#include "llvm/Transforms/SVFAnalysis/IntToPtrDDA.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"

// SVF Includes
#include "SVFIR/SVFVariables.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "WPA/Andersen.h"

#define DEBUG_TYPE "unsafe-heap-alloc"

using namespace llvm;
using namespace SVF;

IntToPtrDDA::IntToPtrDDA(Module &M, SVFIR *PAG, Andersen *Ander,
                         LLVMModuleSet *LMS)
    : M(M), PAG(PAG), Ander(Ander), LMS(LMS) {}

AllocaInst *IntToPtrDDA::getOffsetFromAlloca(Value *Ptr, int64_t &Offset) {
  const DataLayout &DL = M.getDataLayout();
  Offset = 0;
  Value *Curr = Ptr;
  while (Curr) {
    if (auto *GEP = dyn_cast<GetElementPtrInst>(Curr)) {
      APInt GEPOff(64, 0);
      if (GEP->accumulateConstantOffset(DL, GEPOff)) {
        Offset += GEPOff.getSExtValue();
        Curr = GEP->getPointerOperand();
      } else {
        return nullptr;
      }
    } else if (auto *BC = dyn_cast<BitCastInst>(Curr)) {
      Curr = BC->getOperand(0);
    } else if (auto *AI = dyn_cast<AllocaInst>(Curr)) {
      return AI;
    } else {
      break;
    }
  }
  return nullptr;
}

void IntToPtrDDA::walkBackward(Value *V, SmallPtrSetImpl<Value *> &Visited,
                               PointsTo &Result) {
  if (!V || !Visited.insert(V).second)
    return;

  // check memoization cache
  auto CacheIt = Cache.find(V);
  if (CacheIt != Cache.end()) {
    Result |= CacheIt->second;
    return;
  }

  PointsTo Local;

  // pointer-typed value with Andersen info — use directly
  if (V->getType()->isPointerTy() && LMS->hasValueNode(V)) {
    NodeID NID = LMS->getValueNode(V);
    const PointsTo &Pts = Ander->getPts(NID);
    if (!Pts.empty()) {
      Local |= Pts;
      Result |= Local;
      Cache[V] = Local;
      return;
    }
  }

  // ptrtoint: found the pointer origin
  if (auto *P2I = dyn_cast<PtrToIntInst>(V)) {
    Value *PtrOp = P2I->getPointerOperand();
    if (LMS->hasValueNode(PtrOp)) {
      NodeID NID = LMS->getValueNode(PtrOp);
      Local |= Ander->getPts(NID);
    }
    Result |= Local;
    Cache[V] = Local;
    return;
  }

  // binary operator — recurse both operands
  if (auto *BO = dyn_cast<BinaryOperator>(V)) {
    walkBackward(BO->getOperand(0), Visited, Local);
    walkBackward(BO->getOperand(1), Visited, Local);
    Result |= Local;
    Cache[V] = Local;
    return;
  }

  // phi — recurse all incoming
  if (auto *PHI = dyn_cast<PHINode>(V)) {
    for (unsigned I = 0; I < PHI->getNumIncomingValues(); ++I)
      walkBackward(PHI->getIncomingValue(I), Visited, Local);
    Result |= Local;
    Cache[V] = Local;
    return;
  }

  // select — recurse both alternatives
  if (auto *SEL = dyn_cast<SelectInst>(V)) {
    walkBackward(SEL->getTrueValue(), Visited, Local);
    walkBackward(SEL->getFalseValue(), Visited, Local);
    Result |= Local;
    Cache[V] = Local;
    return;
  }

  // extractvalue — recurse aggregate operand
  if (auto *EV = dyn_cast<ExtractValueInst>(V)) {
    walkBackward(EV->getAggregateOperand(), Visited, Local);
    Result |= Local;
    Cache[V] = Local;
    return;
  }

  // zext/sext/trunc — recurse operand
  if (auto *CI = dyn_cast<CastInst>(V)) {
    walkBackward(CI->getOperand(0), Visited, Local);
    Result |= Local;
    Cache[V] = Local;
    return;
  }

  // call — query Andersen for return value
  if (auto *Call = dyn_cast<CallInst>(V)) {
    if (LMS->hasValueNode(Call)) {
      NodeID NID = LMS->getValueNode(Call);
      const PointsTo &Pts = Ander->getPts(NID);
      if (!Pts.empty())
        Local |= Pts;
    }
    Result |= Local;
    Cache[V] = Local;
    return;
  }

  // load — Andersen-guided store matching (3-phase)
  if (auto *LI = dyn_cast<LoadInst>(V)) {
    Value *LoadPtr = LI->getPointerOperand();
    if (!LMS->hasValueNode(LoadPtr)) {
      LLVM_DEBUG(dbgs() << "[DDA-load] no SVF node for load ptr: " << *LoadPtr
                        << "\n");
      Cache[V] = Local;
      return;
    }
    NodeID LoadPtrNode = LMS->getValueNode(LoadPtr);
    const PointsTo &LoadPts = Ander->getPts(LoadPtrNode);
    Function *LIFunc = LI->getFunction();

    // helper: check alias and recurse into stored value
    auto CheckStoreAlias = [&](StoreInst *SI) {
      Value *StorePtr = SI->getPointerOperand();
      bool Alias = false;
      if (StorePtr == LoadPtr) {
        Alias = true;
      } else {
        int64_t LoadOff = 0, StoreOff = 0;
        AllocaInst *LoadAI = getOffsetFromAlloca(LoadPtr, LoadOff);
        AllocaInst *StoreAI = getOffsetFromAlloca(StorePtr, StoreOff);
        if (LoadAI && StoreAI && LoadAI == StoreAI && LoadOff == StoreOff) {
          Alias = true;
        } else if (!LoadPts.empty() && LMS->hasValueNode(StorePtr)) {
          NodeID StorePtrNode = LMS->getValueNode(StorePtr);
          const PointsTo &StorePts = Ander->getPts(StorePtrNode);
          PointsTo Isect = LoadPts;
          Isect &= StorePts;
          if (!Isect.empty())
            Alias = true;
        }
      }
      if (Alias)
        walkBackward(SI->getValueOperand(), Visited, Local);
    };

    // Phase 1: same-function search
    if (LIFunc) {
      for (BasicBlock &BB : *LIFunc)
        for (Instruction &Inst : BB)
          if (auto *SI = dyn_cast<StoreInst>(&Inst))
            CheckStoreAlias(SI);
    }

    // Phase 2: module-wide search (Andersen-guided only)
    if (Local.empty() && !LoadPts.empty()) {
      for (Function &Func : M) {
        if (&Func == LIFunc)
          continue;
        if (Func.isDeclaration() || Func.getName().starts_with("__svf_"))
          continue;
        for (BasicBlock &BB : Func) {
          for (Instruction &Inst : BB) {
            auto *SI = dyn_cast<StoreInst>(&Inst);
            if (!SI)
              continue;
            Value *StorePtr = SI->getPointerOperand();
            if (LMS->hasValueNode(StorePtr)) {
              NodeID StorePtrNode = LMS->getValueNode(StorePtr);
              const PointsTo &StorePts = Ander->getPts(StorePtrNode);
              PointsTo Isect = LoadPts;
              Isect &= StorePts;
              if (!Isect.empty())
                walkBackward(SI->getValueOperand(), Visited, Local);
            }
          }
        }
      }
    }

    // Phase 3: inter-procedural alloca-offset matching
    if (Local.empty()) {
      int64_t LoadOff = 0;
      AllocaInst *LoadAI = getOffsetFromAlloca(LoadPtr, LoadOff);
      if (LoadAI) {
        const DataLayout &DL = M.getDataLayout();
        for (User *U : LoadAI->users()) {
          CallBase *CB = dyn_cast<CallBase>(U);
          if (!CB) {
            for (User *UU : U->users()) {
              if (auto *cb = dyn_cast<CallBase>(UU))
                CB = cb;
            }
          }
          if (!CB)
            continue;
          Function *Callee = CB->getCalledFunction();
          if (!Callee || Callee->isDeclaration())
            continue;

          for (unsigned I = 0; I < CB->arg_size(); ++I) {
            Value *Arg = CB->getArgOperand(I);
            int64_t ArgOff = 0;
            AllocaInst *ArgAI = getOffsetFromAlloca(Arg, ArgOff);
            if (ArgAI != LoadAI)
              continue;

            Argument *FormalArg = Callee->getArg(I);
            int64_t TargetOff = LoadOff - ArgOff;

            for (BasicBlock &BB : *Callee) {
              for (Instruction &Inst : BB) {
                auto *SI = dyn_cast<StoreInst>(&Inst);
                if (!SI)
                  continue;
                Value *Curr = SI->getPointerOperand();
                int64_t Off = 0;
                bool Valid = true;
                while (Curr) {
                  if (auto *GEP = dyn_cast<GetElementPtrInst>(Curr)) {
                    APInt GEPOff(64, 0);
                    if (GEP->accumulateConstantOffset(DL, GEPOff)) {
                      Off += GEPOff.getSExtValue();
                      Curr = GEP->getPointerOperand();
                    } else {
                      Valid = false;
                      break;
                    }
                  } else if (auto *BC = dyn_cast<BitCastInst>(Curr)) {
                    Curr = BC->getOperand(0);
                  } else {
                    break;
                  }
                }
                if (!Valid)
                  continue;
                if (Curr == FormalArg && Off == TargetOff) {
                  Value *StoreVal = SI->getValueOperand();
                  LLVM_DEBUG(dbgs() << "[DDA-load] phase3 MATCH in "
                                    << Callee->getName() << " off=" << Off
                                    << "\n");
                  // Option A: use Andersen directly for pointer-typed values
                  // to bypass visited/cache poisoning from earlier phases
                  if (StoreVal->getType()->isPointerTy() &&
                      LMS->hasValueNode(StoreVal)) {
                    NodeID NID = LMS->getValueNode(StoreVal);
                    const PointsTo &SvPts = Ander->getPts(NID);
                    if (!SvPts.empty()) {
                      Local |= SvPts;
                    } else {
                      walkBackward(StoreVal, Visited, Local);
                    }
                  } else {
                    walkBackward(StoreVal, Visited, Local);
                  }
                }
              }
            }
          }
        }
      }
    }

    Result |= Local;
    Cache[V] = Local;
    return;
  }

  // constants / other — dead end
  Cache[V] = Local;
}

void IntToPtrDDA::run() {
  LLVM_DEBUG(
      dbgs()
      << "[IntToPtrDDA] running def-use DDA for broken IntToPtr nodes\n");

  // Phase 1: audit — find IntToPtrs with empty Andersen points-to sets
  std::vector<std::pair<IntToPtrInst *, NodeID>> BrokenITPs;
  for (Function &F : M) {
    if (F.isDeclaration() || F.getName().starts_with("__svf_"))
      continue;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (auto *ITP = dyn_cast<IntToPtrInst>(&I)) {
          if (LMS->hasValueNode(ITP)) {
            NodeID Node = LMS->getValueNode(ITP);
            if (Ander->getPts(Node).empty())
              BrokenITPs.push_back({ITP, Node});
          }
        }
      }
    }
  }

  LLVM_DEBUG(dbgs() << "[IntToPtrDDA] found " << BrokenITPs.size()
                    << " broken IntToPtr instructions\n");

  // Phase 2: backward walk + Phase 3: patch Andersen
  PatchedCount = 0;
  FailedCount = 0;
  for (auto &[ITP, NodeId] : BrokenITPs) {
    SmallPtrSet<Value *, 32> Visited;
    PointsTo DDAResult;
    walkBackward(ITP->getOperand(0), Visited, DDAResult);

    if (!DDAResult.empty()) {
      Ander->unionPts(NodeId, DDAResult);
      PatchedTargets[ITP] = DDAResult;
      LLVM_DEBUG(dbgs() << "[DDA] patched: " << *ITP << " -> "
                        << DDAResult.count() << " targets in "
                        << ITP->getFunction()->getName() << "\n");
      PatchedCount++;
    } else {
      LLVM_DEBUG(dbgs() << "[DDA] FAILED: " << *ITP << " in "
                        << ITP->getFunction()->getName() << "\n");
      FailedCount++;
    }
  }

  if (!BrokenITPs.empty()) {
    LLVM_DEBUG(dbgs() << "[IntToPtrDDA] patched " << PatchedCount << ", failed "
                      << FailedCount << " (cache entries: " << Cache.size()
                      << ")\n");
  }
}

bool IntToPtrDDA::walkChain(Value *V, SmallPtrSetImpl<Value *> &Seen,
                            PointsTo &HeapTargets) {
  if (!V || !Seen.insert(V).second)
    return false;

  auto It = PatchedTargets.find(V);
  if (It != PatchedTargets.end()) {
    HeapTargets |= It->second;
    LLVM_DEBUG(dbgs() << "[DDA] IntToPtr fwd: inherited " << It->second.count()
                      << " targets from patched " << *V << "\n");
    return true;
  }

  if (auto *GEP = dyn_cast<GetElementPtrInst>(V))
    return walkChain(GEP->getPointerOperand(), Seen, HeapTargets);
  if (auto *BC = dyn_cast<BitCastInst>(V))
    return walkChain(BC->getOperand(0), Seen, HeapTargets);

  if (auto *PHI = dyn_cast<PHINode>(V)) {
    bool Found = false;
    for (unsigned I = 0; I < PHI->getNumIncomingValues(); ++I) {
      if (walkChain(PHI->getIncomingValue(I), Seen, HeapTargets))
        Found = true;
    }
    return Found;
  }
  if (auto *SEL = dyn_cast<SelectInst>(V)) {
    bool A = walkChain(SEL->getTrueValue(), Seen, HeapTargets);
    bool B = walkChain(SEL->getFalseValue(), Seen, HeapTargets);
    return A || B;
  }

  // For loads, check if any stored value derives from a patched IntToPtr
  if (auto *LI = dyn_cast<LoadInst>(V)) {
    Value *LoadPtr = LI->getPointerOperand();
    if (LMS->hasValueNode(LoadPtr)) {
      NodeID LoadNode = LMS->getValueNode(LoadPtr);
      const PointsTo &LoadPts = Ander->getPts(LoadNode);
      if (!LoadPts.empty()) {
        Function *F = LI->getFunction();
        if (F) {
          for (BasicBlock &BB : *F) {
            for (Instruction &Inst : BB) {
              if (auto *SI = dyn_cast<StoreInst>(&Inst)) {
                Value *StorePtr = SI->getPointerOperand();
                if (LMS->hasValueNode(StorePtr)) {
                  NodeID StoreNode = LMS->getValueNode(StorePtr);
                  const PointsTo &StorePts = Ander->getPts(StoreNode);
                  PointsTo Isect = LoadPts;
                  Isect &= StorePts;
                  if (!Isect.empty()) {
                    if (walkChain(SI->getValueOperand(), Seen, HeapTargets))
                      return true;
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  return false;
}

PointsTo IntToPtrDDA::resolveTargets(Value *Ptr) {
  PointsTo Result;

  // First try a fresh backward walk
  SmallPtrSet<Value *, 32> Visited;
  walkBackward(Ptr, Visited, Result);

  // If backward walk found nothing, try Option D forward chain-walking
  if (Result.empty() && !PatchedTargets.empty()) {
    SmallPtrSet<Value *, 16> Seen;
    walkChain(Ptr, Seen, Result);
  }

  return Result;
}
