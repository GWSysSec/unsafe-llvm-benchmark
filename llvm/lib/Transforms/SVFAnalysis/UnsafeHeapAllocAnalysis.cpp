#include "llvm/Transforms/SVFAnalysis/UnsafeHeapAllocAnalysis.h"
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

  // 3.5 HashMap RawTable Auditor & Custom DDA (New Phase)
  errs() << "[UnsafeHeapAllocAnalysis] Running Auditor & Custom DDA\\n";
  
  // Phase 1: Auditor
  std::vector<IntToPtrInst*> brokenIntToPtrs;
  for (Function &F : M) {
      if (F.isDeclaration() || F.getName().starts_with("__svf_")) continue;
      for (BasicBlock &BB : F) {
          for (Instruction &I : BB) {
              if (auto *ITP = dyn_cast<IntToPtrInst>(&I)) {
                  if (llvmModuleSet->hasValueNode(ITP)) {
                      NodeID node = llvmModuleSet->getValueNode(ITP);
                      if (ander->getPts(node).empty()) {
                          brokenIntToPtrs.push_back(ITP);
                      }
                  } else {
                      errs() << "[UnsafeHeapAllocAnalysis] ITP has no value node: " << ITP->getFunction()->getName() << " :: " << *ITP << "\\n";
                  }
              }
          }
      }
  }

  errs() << "[UnsafeHeapAllocAnalysis] Found " << brokenIntToPtrs.size() << " broken IntToPtr instructions.\\n";

  // Pre-Compute all StoreInst for the Memory Wall scan
  std::vector<StoreInst*> allStores;
  for (Function &F : M) {
      if (!F.isDeclaration() && !F.getName().starts_with("__svf_")) {
          for (BasicBlock &BB : F) {
              for (Instruction &I : BB) {
                  if (auto *SI = dyn_cast<StoreInst>(&I)) {
                      allStores.push_back(SI);
                  }
              }
          }
      }
  }

  // ddaWalk lambda for reuse
  auto runDDA = [&](Value *StartV, PointsTo &resolvedPts) {
      DenseSet<Value*> visited;
      std::function<void(Value*, unsigned)> walk;
      walk = [&](Value *V, unsigned depth) {
          if (depth > 100) { errs() << "    [DDA] Depth limit reached\\n"; return; }
          if (!visited.insert(V).second) return;
          
          if (V->getType()->isPointerTy() && llvmModuleSet->hasValueNode(V)) {
              NodeID ptrNode = llvmModuleSet->getValueNode(V);
              const PointsTo &pts = ander->getPts(ptrNode);
              if (!pts.empty()) {
                  resolvedPts |= pts;
                  errs() << "    [DDA] Pointer Hit (resolved): " << *V << " with " << pts.count() << " targets.\\n";
                  return;
              }
          }

          if (auto *PTI = dyn_cast<PtrToIntInst>(V)) {
              Value *Ptr = PTI->getPointerOperand();
              if (llvmModuleSet->hasValueNode(Ptr)) {
                  NodeID ptrNode = llvmModuleSet->getValueNode(Ptr);
                  const PointsTo &pts = ander->getPts(ptrNode);
                  resolvedPts |= pts;
                  if (!pts.empty()) {
                      errs() << "    [DDA] Origin found: " << *PTI << " with " << pts.count() << " targets.\\n";
                  }
              }
          } else if (auto *CE = dyn_cast<ConstantExpr>(V)) {
              if (CE->getOpcode() == Instruction::PtrToInt) {
                  Value *Ptr = CE->getOperand(0);
                  if (llvmModuleSet->hasValueNode(Ptr)) {
                      NodeID ptrNode = llvmModuleSet->getValueNode(Ptr);
                      const PointsTo &pts = ander->getPts(ptrNode);
                      resolvedPts |= pts;
                      if (!pts.empty()) {
                          errs() << "    [DDA] ConstantExpr Origin found: " << *CE << " with " << pts.count() << " targets.\\n";
                      }
                  }
              } else {
                  for (unsigned i = 0; i < CE->getNumOperands(); ++i) {
                      walk(CE->getOperand(i), depth + 1);
                  }
              }
          } else if (auto *LI = dyn_cast<LoadInst>(V)) {
              Value *LoadPtr = LI->getPointerOperand();
              if (llvmModuleSet->hasValueNode(LoadPtr)) {
                  NodeID loadNode = llvmModuleSet->getValueNode(LoadPtr);
                  const PointsTo &loadPts = ander->getPts(loadNode);
                  
                  if (!loadPts.empty()) {
                      for (StoreInst *SI : allStores) {
                          Value *StorePtr = SI->getPointerOperand();
                          if (llvmModuleSet->hasValueNode(StorePtr)) {
                              NodeID storeNode = llvmModuleSet->getValueNode(StorePtr);
                              const PointsTo &storePts = ander->getPts(storeNode);
                              if (storePts.intersects(loadPts)) {
                                  walk(SI->getValueOperand(), depth + 1);
                              }
                          }
                      }
                  }
              }
          } else if (auto *Arg = dyn_cast<Argument>(V)) {
              Function *F = Arg->getParent();
              for (User *U : F->users()) {
                  if (CallBase *CB = dyn_cast<CallBase>(U)) {
                      if (CB->getCalledFunction() == F) {
                          walk(CB->getArgOperand(Arg->getArgNo()), depth + 1);
                      }
                  }
              }
          } else if (auto *I = dyn_cast<Instruction>(V)) {
              for (Use &U : I->operands()) {
                  Type *Ty = U.get()->getType();
                  if (Ty->isIntegerTy() || Ty->isVectorTy() || Ty->isStructTy() || Ty->isPointerTy()) {
                      walk(U.get(), depth + 1);
                  }
              }
          }
      };
      walk(StartV, 0);
  };

  // Phase 2: DDA Trigger
  int patchedCount = 0;
  for (IntToPtrInst *ITP : brokenIntToPtrs) {
      PointsTo resolvedPts;
      
      errs() << "  [DDA] Starting trace for: " << *ITP << "\\n";
      runDDA(ITP->getOperand(0), resolvedPts);

      // Phase 3: The Merge
      if (!resolvedPts.empty()) {
          NodeID targetNode = llvmModuleSet->getValueNode(ITP);
          ander->unionPts(targetNode, resolvedPts);
          errs() << "[UnsafeHeapAllocAnalysis] Patched points-to for IntToPtr: " << *ITP << " with " 
                 << resolvedPts.count() << " targets\\n";
          patchedCount++;
      } else {
          errs() << "[UnsafeHeapAllocAnalysis] FAILED to patch IntToPtr: " << *ITP << "\\n";
      }
  }
  
  errs() << "[UnsafeHeapAllocAnalysis] DDA phase complete. Patched " << patchedCount << " / " << brokenIntToPtrs.size() << " pointers.\\n";


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

                      if (heapTargets.empty()) {
                          PointsTo ddaPts;
                          runDDA(Ptr, ddaPts);
                          if (!ddaPts.empty()) {
                              errs() << "[UnsafeHeapAllocAnalysis] SESE Pointer Forward Propagation: On-Demand DDA recovered " << ddaPts.count() << " targets for " << *Ptr << "\\n";
                              extractTargets(ddaPts);
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
