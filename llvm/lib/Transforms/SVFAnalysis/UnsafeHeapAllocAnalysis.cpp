#include "llvm/Transforms/SVFAnalysis/UnsafeHeapAllocAnalysis.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"

#define DEBUG_TYPE "unsafe-heap-alloc"

// SVF Includes
#include "SVFIR/SVFVariables.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "WPA/Andersen.h"

using namespace llvm;
using namespace SVF;

// command-line flag to dump points-to results as json for ci testing
static cl::opt<bool> DumpSVFPtsTo(
    "dump-svf-pts-to",
    cl::desc("dump svf points-to analysis results as json for ci verification"),
    cl::init(false));

AnalysisKey UnsafeHeapAllocAnalysis::Key;

/// helper: escape a string for json output
static std::string jsonEscape(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default: out += c;
    }
  }
  return out;
}

/// helper: get a string representation of an llvm instruction
static std::string instrToString(const Instruction *I) {
  std::string s;
  raw_string_ostream rso(s);
  rso << *I;
  return rso.str();
}

/// dump analysis results as json to svf_pts_to.json
static void dumpResultsAsJSON(const UnsafeHeapAllocAnalysis::Result &Res,
                               Module &M, LLVMModuleSet *llvmModuleSet,
                               SVFIR *pag) {
  std::error_code EC;
  raw_fd_ostream OS("svf_pts_to.json", EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "[UnsafeHeapAllocAnalysis] warning: cannot write svf_pts_to.json: "
           << EC.message() << "\n";
    return;
  }

  OS << "{\n";

  // 1. heap_objects
  OS << "  \"heap_objects\": [\n";
  bool firstObj = true;
  for (const auto &kv : Res.HeapAllocSizes) {
    if (!firstObj) OS << ",\n";
    firstObj = false;

    std::string allocFn = "unknown";
    std::string srcLoc = "";

    // try to find the corresponding allocation instruction
    for (const auto &site : Res.AllocationSites) {
      if (site.second == kv.first) {
        if (const CallBase *CB = dyn_cast<CallBase>(site.first)) {
          if (Function *F = CB->getCalledFunction()) {
            allocFn = F->getName().str();
          }
        }
        srcLoc = instrToString(site.first);
        break;
      }
    }

    OS << "    {\"node_id\": " << kv.first
       << ", \"alloc_fn\": \"" << jsonEscape(allocFn) << "\""
       << ", \"size\": " << kv.second
       << ", \"source\": \"" << jsonEscape(srcLoc) << "\""
       << "}";
  }
  OS << "\n  ],\n";

  // 2. unsafe_ptrs
  OS << "  \"unsafe_ptrs\": [\n";
  bool firstPtr = true;
  for (const auto &kv : Res.UnsafePtrs) {
    if (!firstPtr) OS << ",\n";
    firstPtr = false;

    std::string funcName = kv.first->getFunction()
                             ? kv.first->getFunction()->getName().str()
                             : "unknown";

    OS << "    {\"instruction\": \"" << jsonEscape(instrToString(kv.first)) << "\""
       << ", \"function\": \"" << jsonEscape(funcName) << "\""
       << ", \"targets\": [";

    for (size_t i = 0; i < kv.second.size(); ++i) {
      if (i > 0) OS << ", ";
      OS << kv.second[i];

      // classify target type
      const BaseObjVar *baseObj = pag->getBaseObject(kv.second[i]);
      (void)baseObj; // used only in json below
    }
    OS << "]"
       << ", \"num_heap_targets\": " << kv.second.size()
       << "}";
  }
  OS << "\n  ],\n";

  // 3. allocation_sites
  OS << "  \"allocation_sites\": [\n";
  bool firstSite = true;
  for (const auto &kv : Res.AllocationSites) {
    if (!firstSite) OS << ",\n";
    firstSite = false;

    std::string allocFn = "unknown";
    if (const CallBase *CB = dyn_cast<CallBase>(kv.first)) {
      if (Function *F = CB->getCalledFunction()) {
        allocFn = F->getName().str();
      }
    }

    OS << "    {\"instruction\": \"" << jsonEscape(instrToString(kv.first)) << "\""
       << ", \"node_id\": " << kv.second
       << ", \"alloc_fn\": \"" << jsonEscape(allocFn) << "\""
       << "}";
  }
  OS << "\n  ],\n";

  // 4. summary
  OS << "  \"summary\": {\n"
     << "    \"module\": \"" << jsonEscape(M.getName().str()) << "\",\n"
     << "    \"heap_objects_count\": " << Res.HeapAllocSizes.size() << ",\n"
     << "    \"unsafe_ptrs_count\": " << Res.UnsafePtrs.size() << ",\n"
     << "    \"allocation_sites_count\": " << Res.AllocationSites.size() << "\n"
     << "  }\n";

  OS << "}\n";

  errs() << "[UnsafeHeapAllocAnalysis] dumped points-to results to svf_pts_to.json\n";
}

UnsafeHeapAllocAnalysis::Result UnsafeHeapAllocAnalysis::run(Module &M, ModuleAnalysisManager &AM) {
  Result Res;
  
  // 0. Check Environment Variable
  const char *EnvPackage = std::getenv("CARGO_PRIMARY_PACKAGE");
  if (!EnvPackage || std::strcmp(EnvPackage, "1") != 0) {
    return Res;
  }

  LLVM_DEBUG(dbgs() << "[UnsafeHeapAllocAnalysis] Running Analysis on " << M.getName() << "\n");

  // 1. Build SVF Module
  LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
  llvmModuleSet->buildSVFModule(M);

  // 2. Build SVFIR (PAG)
  SVFIRBuilder builder;
  SVFIR* pag = builder.build();

  // 3. Run Andersen
  Andersen* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);

  // 4. Populate HeapAllocSizes
  // Iterate over all nodes in PAG to find Heap Objects.
  // Use base node ID to deduplicate: SVF creates GepObjVar field sub-objects
  // that share the same underlying allocation instruction as their base
  // HeapObjVar.  Storing both raw IDs causes AllocationSites (keyed by
  // Instruction*) to be overwritten, leaving the base ID unmatched in the
  // JSON dump.
  for (auto it = pag->begin(); it != pag->end(); ++it) {
      NodeID id = it->first;
      if (pag->getBaseObject(id)) { // Check if it is a base object
          const BaseObjVar* node = pag->getBaseObject(id);
          if (node->isHeap()) {
              // use base object id to avoid duplicate heap objects from gep sub-objects
              NodeID baseId = pag->getBaseObjVar(id);
              Res.HeapAllocSizes[baseId] = 0;

              // Populate AllocationSites
              if (llvmModuleSet->hasLLVMValue(node)) {
                  const Value* V = llvmModuleSet->getLLVMValue(node);
                  if (const Instruction* I = dyn_cast<Instruction>(V)) {
                      Res.AllocationSites[I] = baseId;
                  }
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

  // 6. Dump points-to results if requested (must happen before cleanup)
  if (DumpSVFPtsTo) {
    dumpResultsAsJSON(Res, M, llvmModuleSet, pag);
  }

  // 7. Cleanup
  AndersenWaveDiff::releaseAndersenWaveDiff();
  SVFIR::releaseSVFIR();
  LLVMModuleSet::releaseLLVMModuleSet();

  return Res;
}
