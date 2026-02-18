#include "llvm/Transforms/SVFAnalysis/UnsafeHeapAllocAnalysis.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/raw_ostream.h"

// SVF Includes
#include "SVFIR/SVFVariables.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "WPA/Andersen.h"

using namespace llvm;
using namespace SVF;

AnalysisKey UnsafeHeapAllocAnalysis::Key;

UnsafeHeapAllocAnalysis::Result UnsafeHeapAllocAnalysis::run(Module &M, ModuleAnalysisManager &AM) {
  Result Res;
  
  // 0. Check Environment Variable
  const char *EnvPackage = std::getenv("CARGO_PRIMARY_PACKAGE");
  if (!EnvPackage || std::strcmp(EnvPackage, "1") != 0) {
    return Res;
  }

  errs() << "[UnsafeHeapAllocAnalysis] Running Analysis on " << M.getName() << "\n";

  // 1. Build SVF Module
  LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
  llvmModuleSet->buildSVFModule(M);

  // 2. Build SVFIR (PAG)
  SVFIRBuilder builder;
  SVFIR* pag = builder.build();

  // 3. Run Andersen
  Andersen* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);

  // 4. Populate HeapAllocSizes
  // Iterate over all nodes in PAG to find Heap Objects
  for (auto it = pag->begin(); it != pag->end(); ++it) {
      NodeID id = it->first;
      if (pag->getBaseObject(id)) { // Check if it is a base object
          const BaseObjVar* node = pag->getBaseObject(id);
          if (node->isHeap()) {
              // Get size if possible. isHeap() usually means malloc/new.
              // SVF might have size info. 
              // BaseObjVar has getSize() but it might be symbolic or constant.
              // For now, we store 0 if unknown, but runtime alloc hook usually gets size from arguments.
              Res.HeapAllocSizes[id] = 0; 

              // Populate AllocationSites
              if (llvmModuleSet->hasLLVMValue(node)) {
                  const Value* V = llvmModuleSet->getLLVMValue(node);
                  Res.AllocationSites[V] = id;
              }
          }
      }
  }

  // 5. Populate UnsafePtrs (Instructions that point to Heap)
  for (Function &F : M) {
      if (F.isDeclaration()) continue;
      for (BasicBlock &BB : F) {
          for (Instruction &I : BB) {
              Value *Ptr = nullptr;
              if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
                  Ptr = LI->getPointerOperand();
              } else if (StoreInst *SI = dyn_cast<StoreInst>(&I)) {
                  Ptr = SI->getPointerOperand();
              } else if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(&I)) {
                   // GEPs calculate addresses, but don't access memory directly.
                   // Usually checks are done on Load/Store.
              } else if (CallBase *CB = dyn_cast<CallBase>(&I)) {
                   // Calls might access memory, but handling them is harder (alias of args).
                   // For V1, focus on Load/Store.
              }

              if (Ptr) {
                  if (llvmModuleSet->hasValueNode(Ptr)) {
                      NodeID pNodeId = llvmModuleSet->getValueNode(Ptr);
                      const PointsTo &pts = ander->getPts(pNodeId);
                      
                      std::vector<NodeID> heapTargets;
                      for (NodeID target : pts) {
                          const BaseObjVar* targetNode = pag->getBaseObject(target);
                          if (targetNode && targetNode->isHeap()) {
                              heapTargets.push_back(target);
                          }
                      }

                      if (!heapTargets.empty()) {
                          Res.UnsafePtrs[&I] = std::move(heapTargets);
                      }
                  }
              }
          }
      }
  }

  // 6. Cleanup
  AndersenWaveDiff::releaseAndersenWaveDiff();
  SVFIR::releaseSVFIR();
  LLVMModuleSet::releaseLLVMModuleSet();

  return Res;
}
