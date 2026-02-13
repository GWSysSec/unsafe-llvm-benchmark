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

PreservedAnalyses UnsafeHeapAllocAnalysis::run(Module &M, ModuleAnalysisManager &AM) {
  errs() << "[UnsafeHeapAllocAnalysis] DEBUG: Pass started for " << M.getName() << "\n";

  // 0. Check Environment Variable to ensure we only analyze the primary crate
  // This avoids analyzing dependencies when running via cargo
  const char *EnvPackage = std::getenv("CARGO_PRIMARY_PACKAGE");
  if (!EnvPackage || std::strcmp(EnvPackage, "1") != 0) {
    return PreservedAnalyses::all();
  }

  errs() << "[UnsafeHeapAllocAnalysis] Running on " << M.getName() << "\n";

  // 1. Build SVF Module from the LLVM Module
  LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
  llvmModuleSet->buildSVFModule(M);

  // 2. Build SVFIR (Program Assignment Graph)
  SVFIRBuilder builder;
  SVFIR* pag = builder.build();

  // 3. Run Andersen's Pointer Analysis (WaveDiff implementation is the standard)
  Andersen* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
  
  // 4. Iterate over instructions to find unsafe pointers
  for (Function &F : M) {
      if (F.isDeclaration()) continue;
      for (BasicBlock &BB : F) {
          for (Instruction &I : BB) {
              // Check for "unsafe_inst" metadata
              if (!I.getMetadata("unsafe_inst")) continue;

              Value *Ptr = nullptr;
              if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
                  Ptr = LI->getPointerOperand();
              } else if (StoreInst *SI = dyn_cast<StoreInst>(&I)) {
                  Ptr = SI->getPointerOperand();
              }

              if (Ptr) {
                  // Get the SVF Node ID for the pointer
                  if (!llvmModuleSet->hasValueNode(Ptr)) continue;
                  
                  NodeID pNodeId = llvmModuleSet->getValueNode(Ptr);
                  const PointsTo &pts = ander->getPts(pNodeId);
                  
                  if (pts.empty()) continue;

                  errs() << "[UnsafeHeapAllocAnalysis] Unsafe pointer usage detected:\n";
                  errs() << "  Instruction: " << I << "\n";
                  
                  for (NodeID target : pts) {
                      const BaseObjVar* targetNode = pag->getBaseObject(target);
                      
                      std::string typeStr = "[Unknown]";
                      if(targetNode) {
                          if (targetNode->isHeap()) typeStr = "[Heap]";
                          else if (targetNode->isStack()) typeStr = "[Stack]";
                          else if (targetNode->isStaticObj() || targetNode->isGlobalObj()) typeStr = "[Static]";
                      }

                      errs() << "  Points to " << typeStr << " (SVF Node " << target << "): " << targetNode->toString() << "\n";
                      
                      std::string allocSiteStr = "";
                      if (llvmModuleSet->hasLLVMValue(targetNode)) {
                          const Value* V = llvmModuleSet->getLLVMValue(targetNode);
                          allocSiteStr = V->getName().str();
                          errs() << "    Allocation Site: " << *V << "\n";
                      }
                      
                      // JSON Output
                      errs() << "JSON_REPORT: {\"instruction\": \"" << I.getOpcodeName() << "\", " 
                             << "\"pointer_node\": " << pNodeId << ", "
                             << "\"target_node\": " << target << ", "
                             << "\"memory_type\": \"" << typeStr << "\", "
                             << "\"allocation_site\": \"" << allocSiteStr << "\"}\n";
                  }
              }
          }
      }
  }

  // 5. Cleanup SVF resources
  AndersenWaveDiff::releaseAndersenWaveDiff();
  SVFIR::releaseSVFIR();
  LLVMModuleSet::releaseLLVMModuleSet();

  return PreservedAnalyses::all();
}
