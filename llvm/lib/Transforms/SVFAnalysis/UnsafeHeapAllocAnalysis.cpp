#include "llvm/Transforms/SVFAnalysis/UnsafeHeapAllocAnalysis.h"
#include "llvm/Transforms/SVFAnalysis/IntToPtrDDA.h"
#include <algorithm>
#include <map>

#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"

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

/// helper: get source location json fragment from instruction debug info
static std::string getSourceLocJSON(const Instruction *I) {
  if (!I) return "{\"file\": \"\", \"line\": 0, \"col\": 0}";
  const DebugLoc &DL = I->getDebugLoc();
  if (!DL) return "{\"file\": \"\", \"line\": 0, \"col\": 0}";
  std::string file = DL->getFilename().str();
  unsigned line = DL->getLine();
  unsigned col = DL->getColumn();
  return "{\"file\": \"" + jsonEscape(file) + "\", \"line\": " +
         std::to_string(line) + ", \"col\": " + std::to_string(col) + "}";
}

/// dump analysis results as json
static void dumpResultsAsJSON(const UnsafeHeapAllocAnalysis::Result &Res,
                               Module &M, LLVMModuleSet *llvmModuleSet,
                               SVFIR *pag) {
  std::error_code EC;
  std::string moduleName = M.getName().str();
  std::replace(moduleName.begin(), moduleName.end(), '/', '_');
  std::replace(moduleName.begin(), moduleName.end(), '\\', '_');
  static int dumpCount = 0;
  std::string filename = "svf_pts_to_" + moduleName + "_" + std::to_string(dumpCount++) + ".json";
  raw_fd_ostream OS(filename, EC, sys::fs::OF_Text);


  if (EC) {
    errs() << "[UnsafeHeapAllocAnalysis] warning: cannot write " << filename << ": "
           << EC.message() << "\n";
    return;
  }

  OS << "{\n";

  // 1. abstract_heap_objects
  OS << "  \"abstract_heap_objects\": [\n";
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

    // get source location from the allocation instruction
    std::string srcLocJSON = "{\"file\": \"\", \"line\": 0, \"col\": 0}";
    for (const auto &site : Res.AllocationSites) {
      if (site.second == kv.first) {
        srcLocJSON = getSourceLocJSON(site.first);
        break;
      }
    }

    OS << "    {\"node_id\": " << kv.first
       << ", \"alloc_fn\": \"" << jsonEscape(allocFn) << "\""
       << ", \"size\": " << kv.second
       << ", \"source\": \"" << jsonEscape(srcLoc) << "\""
       << ", \"source_loc\": " << srcLocJSON
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
       << ", \"source_loc\": " << getSourceLocJSON(kv.first)
       << ", \"targets\": [";

    for (size_t i = 0; i < kv.second.size(); ++i) {
      if (i > 0) OS << ", ";
      OS << kv.second[i];
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
       << ", \"source_loc\": " << getSourceLocJSON(kv.first)
       << "}";
  }
  OS << "\n  ],\n";

  // 4. aliased_allocation_sites
  OS << "  \"aliased_allocation_sites\": [\n";
  std::map<NodeID, std::vector<const Instruction*>> AliasMap;
  for (const auto &kv : Res.UnsafePtrs) {
    for (NodeID target : kv.second) {
      AliasMap[target].push_back(kv.first);
    }
  }

  bool firstAliased = true;
  for (const auto &kv : Res.AllocationSites) {
    NodeID id = kv.second;
    if (AliasMap.count(id)) {
      if (!firstAliased) OS << ",\n";
      firstAliased = false;

      std::string allocFn = "unknown";
      if (const CallBase *CB = dyn_cast<CallBase>(kv.first)) {
        if (Function *F = CB->getCalledFunction()) {
          allocFn = F->getName().str();
        }
      }

      OS << "    {\"instruction\": \"" << jsonEscape(instrToString(kv.first)) << "\""
         << ", \"node_id\": " << id
         << ", \"alloc_fn\": \"" << jsonEscape(allocFn) << "\""
         << ", \"source_loc\": " << getSourceLocJSON(kv.first)
         << ", \"aliased_by_ptrs\": [\n";

      bool firstAliasPtr = true;
      for (const Instruction *Ptr : AliasMap[id]) {
        if (!firstAliasPtr) OS << ",\n";
        firstAliasPtr = false;
        
        std::string funcName = Ptr->getFunction() ? Ptr->getFunction()->getName().str() : "unknown";
        OS << "      {\"instruction\": \"" << jsonEscape(instrToString(Ptr)) << "\""
           << ", \"function\": \"" << jsonEscape(funcName) << "\""
           << ", \"source_loc\": " << getSourceLocJSON(Ptr) << "}";
      }
      OS << "\n    ]}";
    }
  }
  OS << "\n  ],\n";

  // 4. summary
  OS << "  \"summary\": {\n"
     << "    \"module\": \"" << jsonEscape(M.getName().str()) << "\",\n"
     << "    \"abstract_heap_objects_count\": " << Res.HeapAllocSizes.size() << ",\n"
     << "    \"unsafe_ptrs_count\": " << Res.UnsafePtrs.size() << ",\n"
     << "    \"allocation_sites_count\": " << Res.AllocationSites.size() << "\n"
     << "  }\n";

  OS << "}\n";

  errs() << "[UnsafeHeapAllocAnalysis] dumped points-to results to " << filename << "\n";
}

// helper: collect all sese region markers in a function
static void collectRegions(Function &F, std::vector<Instruction*> &Begins, std::vector<Instruction*> &Ends) {
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

// helper: check if an instruction is in a valid sese region
static bool isInstructionInUnsafeRegion(Instruction *I, const std::vector<Instruction*> &Begins, 
                                 const std::vector<Instruction*> &Ends, DominatorTree *DT, PostDominatorTree *PDT) {
    for (Instruction *Begin : Begins) {
        for (Instruction *End : Ends) {
            // for intra-block regions (which is what instmarker creates), use comesBefore
            if (Begin->getParent() == I->getParent() && End->getParent() == I->getParent()) {
                if (Begin->comesBefore(I) && I->comesBefore(End)) {
                    return true;
                }
            } else {
                // cross block check (safeguard)
                if (DT->dominates(Begin, I) && PDT->dominates(End->getParent(), I->getParent())) {
                    return true;
                }
            }
        }
    }
    return false;
}

UnsafeHeapAllocAnalysis::Result UnsafeHeapAllocAnalysis::run(Module &M, ModuleAnalysisManager &AM) {
  Result Res;
  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  LLVM_DEBUG(dbgs() << "[UnsafeHeapAllocAnalysis] Running Analysis on " << M.getName() << "\n");

  // Early exit: skip SVF entirely if the module has no unsafe region markers.
  // This avoids running the expensive SVF IR builder on crates like
  // compiler_builtins / memchr that have no unsafe regions to analyze,
  // and sidesteps a cast<Instruction> assertion in LLVM triggered by
  // IR patterns in those crates.
  {
    bool hasUnsafeMarkers = false;
    for (Function &F : M) {
      if (F.isDeclaration()) continue;
      std::vector<Instruction*> Begins, Ends;
      collectRegions(F, Begins, Ends);
      if (!Begins.empty()) { hasUnsafeMarkers = true; break; }
    }
    if (!hasUnsafeMarkers) {
      LLVM_DEBUG(dbgs() << "[UnsafeHeapAllocAnalysis] no unsafe markers in "
                        << M.getName() << ", skipping\n");
      return Res;
    }
  }

  // 1. Build SVF Module
  LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
  llvmModuleSet->buildSVFModule(M);

  // 1.5. Dynamically register Rust allocators since they may not be recognized by extapi.c
  for (Function &F : M) {
      StringRef Name = F.getName();
      // exchange_malloc is heavily mangled, e.g. `_ZN5alloc5alloc15exchange_malloc17...`
      if (Name.contains("exchange_malloc") || Name.contains("__rust_alloc")) {
          std::vector<std::string> annotations = {"ALLOC_HEAP_RET", "AllocSize:Arg0"};
          llvmModuleSet->setExtFuncAnnotations(&F, annotations);
      }
  }

  // 2. Build SVFIR (PAG)
  SVFIRBuilder builder;
  SVFIR* pag = builder.build();

  // 3. Run Andersen
  Andersen* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);

  // 3.5 IntToPtr DDA — bridge broken ptrtoint→inttoptr chains
  IntToPtrDDA dda(M, pag, ander, llvmModuleSet);
  dda.run();


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

  // 4b. Handle ALLOC_HEAP_ARG0 allocations (posix_memalign, etc.)
  // These create dummy heap nodes not linked to the CallInst, so the loop above
  // adds them to HeapAllocSizes but NOT AllocationSites.  Trace PAG edges:
  //   call arg0 (vnArg) <-- Store -- dummy val <-- Addr -- heap obj
  for (Function &F : M) {
      if (F.isDeclaration()) continue;
      for (BasicBlock &BB : F) {
          for (Instruction &I : BB) {
              if (!LLVMUtil::isHeapAllocExtCallViaArg(&I)) continue;
              auto *CI = dyn_cast<CallBase>(&I);
              if (!CI || Res.AllocationSites.count(CI)) continue;

              Function *Callee = CI->getCalledFunction();
              if (!Callee) continue;
              u32_t argPos = LLVMUtil::getHeapAllocHoldingArgPosition(Callee);
              if (argPos >= CI->arg_size()) continue;

              Value *Arg = CI->getArgOperand(argPos);
              if (!llvmModuleSet->hasValueNode(Arg)) continue;
              NodeID vnArg = llvmModuleSet->getValueNode(Arg);
              SVFVar *argVar = pag->getGNode(vnArg);

              // Walk: vnArg <-- Store -- dummyVal <-- Addr -- heapObj
              if (!argVar->hasIncomingEdges(SVFStmt::Store)) continue;
              for (SVFStmt *storeEdge : argVar->getIncomingEdges(SVFStmt::Store)) {
                  SVFVar *dummyVal = cast<AssignStmt>(storeEdge)->getRHSVar();
                  if (!dummyVal->hasIncomingEdges(SVFStmt::Addr)) continue;
                  for (SVFStmt *addrEdge : dummyVal->getIncomingEdges(SVFStmt::Addr)) {
                      NodeID objId = cast<AssignStmt>(addrEdge)->getRHSVarID();
                      NodeID baseId = pag->getBaseObjVar(objId);
                      if (Res.HeapAllocSizes.count(baseId)) {
                          Res.AllocationSites[CI] = baseId;
                          LLVM_DEBUG(dbgs() << "[UnsafeHeapAllocAnalysis] ALLOC_HEAP_ARG0: "
                                            << Callee->getName() << " -> heap node " << baseId << "\n");
                      }
                  }
              }
          }
      }
  }

  // 5. Populate UnsafePtrs (Instructions that point to Heap)
  //    Only query SVF for pointers within SESE unsafe regions (marker_begin/marker_end)
  for (Function &F : M) {
      if (F.isDeclaration()) continue;
      // skip svf runtime hooks to prevent recursion
      if (F.getName().starts_with("__svf_")) continue;

      DominatorTree *DT = &FAM.getResult<DominatorTreeAnalysis>(F);
      PostDominatorTree *PDT = &FAM.getResult<PostDominatorTreeAnalysis>(F);

      std::vector<Instruction*> UnsafeMarkerBegins;
      std::vector<Instruction*> UnsafeMarkerEnds;
      collectRegions(F, UnsafeMarkerBegins, UnsafeMarkerEnds);

      // skip functions with no unsafe regions
      if (UnsafeMarkerBegins.empty()) continue;

      for (BasicBlock &BB : F) {
          for (Instruction &I : BB) {
              if (!isa<LoadInst>(I) && !isa<StoreInst>(I)) continue;
              if (!isInstructionInUnsafeRegion(&I, UnsafeMarkerBegins, UnsafeMarkerEnds, DT, PDT)) continue;

              Value *Ptr = nullptr;
              if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
                  Ptr = LI->getPointerOperand();
              } else if (StoreInst *SI = dyn_cast<StoreInst>(&I)) {
                  Ptr = SI->getPointerOperand();
              }

              if (Ptr) {
                  if (llvmModuleSet->hasValueNode(Ptr)) {
                      NodeID pNodeId = llvmModuleSet->getValueNode(Ptr);
                      const PointsTo &pts = ander->getPts(pNodeId);
                      
                      std::vector<NodeID> heapTargets;
                      DenseSet<NodeID> seenBaseIds;

                      auto extractTargets = [&](const PointsTo &pointsTo) {
                          for (NodeID target : pointsTo) {
                              const BaseObjVar* targetNode = pag->getBaseObject(target);
                              if (targetNode && targetNode->isHeap()) {
                                  NodeID baseId = pag->getBaseObjVar(target);
                                  if (seenBaseIds.insert(baseId).second) {
                                      heapTargets.push_back(baseId);
                                  }
                              }
                          }
                      };
                      
                      extractTargets(pts);

                      // If Andersen found no heap targets, try DDA resolution
                      // (backward walk + Option D forward chain-walking)
                      if (heapTargets.empty()) {
                          PointsTo ddaPts = dda.resolveTargets(Ptr);
                          if (!ddaPts.empty())
                              extractTargets(ddaPts);
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
