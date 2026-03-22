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

  // map from patched inttoptr instructions to their resolved heap targets
  // used later for forward propagation to downstream unsafe pointers
  DenseMap<Value*, PointsTo> patchedIntToPtrTargets;
  
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

  // 3.5 llvm def-use chain dda for broken inttoptr pointers
  // svf's pointer solver cannot traverse BinaryOP edges (ptrtoint -> arithmetic -> inttoptr),
  // so we walk llvm ir def-use chains instead, using andersen for load->store matching.
  LLVM_DEBUG(dbgs() << "[UnsafeHeapAllocAnalysis] running def-use DDA for broken IntToPtr nodes\n");

  // phase 1: audit — find inttoptr instructions with empty andersen pts
  std::vector<std::pair<IntToPtrInst*, NodeID>> brokenIntToPtrs;
  for (Function &F : M) {
      if (F.isDeclaration() || F.getName().starts_with("__svf_")) continue;
      for (BasicBlock &BB : F) {
          for (Instruction &I : BB) {
              if (auto *ITP = dyn_cast<IntToPtrInst>(&I)) {
                  if (llvmModuleSet->hasValueNode(ITP)) {
                      NodeID node = llvmModuleSet->getValueNode(ITP);
                      if (ander->getPts(node).empty()) {
                          brokenIntToPtrs.push_back({ITP, node});
                      }
                  }
              }
          }
      }
  }

  LLVM_DEBUG(dbgs() << "[UnsafeHeapAllocAnalysis] found " << brokenIntToPtrs.size()
                    << " broken IntToPtr instructions\n");

  // global memoization cache for dda results — avoids redundant traversals
  DenseMap<Value*, PointsTo> ddaCache;

  // helper: extract constant offset from alloca for local struct matching
  const DataLayout &DL = M.getDataLayout();
  auto getOffsetFromAlloca = [&](Value *V_ptr, int64_t &offset) -> AllocaInst* {
      offset = 0;
      Value *curr = V_ptr;
      while (curr) {
          if (auto *GEP = dyn_cast<GetElementPtrInst>(curr)) {
              APInt gepOffset(64, 0);
              if (GEP->accumulateConstantOffset(DL, gepOffset)) {
                  offset += gepOffset.getSExtValue();
                  curr = GEP->getPointerOperand();
              } else {
                  return nullptr;
              }
          } else if (auto *BC = dyn_cast<BitCastInst>(curr)) {
              curr = BC->getOperand(0);
          } else if (auto *AI = dyn_cast<AllocaInst>(curr)) {
              return AI;
          } else {
              break;
          }
      }
      return nullptr;
  };

  // phase 2: backward def-use dda walk for each broken inttoptr
  // lambda: recursively walk backward through llvm def-use chains to find pointer origins
  std::function<void(Value*, SmallPtrSetImpl<Value*>&, PointsTo&)> walkBackward;
  walkBackward = [&](Value *V, SmallPtrSetImpl<Value*> &visited, PointsTo &result) {
      if (!V || !visited.insert(V).second) return;

      // check memoization cache first
      auto cacheIt = ddaCache.find(V);
      if (cacheIt != ddaCache.end()) {
          result |= cacheIt->second;
          return;
      }

      // track local result for caching
      PointsTo localResult;

      // if this value is pointer-typed and andersen has info, use it directly
      if (V->getType()->isPointerTy() && llvmModuleSet->hasValueNode(V)) {
          NodeID nid = llvmModuleSet->getValueNode(V);
          const PointsTo &pts = ander->getPts(nid);
          if (!pts.empty()) {
              localResult |= pts;
              result |= localResult;
              ddaCache[V] = localResult;
              return;
          }
      }

      // ptrtoint: found the pointer origin, query andersen
      if (auto *P2I = dyn_cast<PtrToIntInst>(V)) {
          Value *ptrOp = P2I->getPointerOperand();
          if (llvmModuleSet->hasValueNode(ptrOp)) {
              NodeID nid = llvmModuleSet->getValueNode(ptrOp);
              localResult |= ander->getPts(nid);
          }
          result |= localResult;
          ddaCache[V] = localResult;
          return;
      }

      // binary operator (add, sub, and, or, xor) — recurse into both operands
      if (auto *BO = dyn_cast<BinaryOperator>(V)) {
          walkBackward(BO->getOperand(0), visited, localResult);
          walkBackward(BO->getOperand(1), visited, localResult);
          result |= localResult;
          ddaCache[V] = localResult;
          return;
      }

      // phi node — recurse into all incoming values
      if (auto *PHI = dyn_cast<PHINode>(V)) {
          for (unsigned i = 0; i < PHI->getNumIncomingValues(); ++i) {
              walkBackward(PHI->getIncomingValue(i), visited, localResult);
          }
          result |= localResult;
          ddaCache[V] = localResult;
          return;
      }

      // select — recurse into both alternatives
      if (auto *SEL = dyn_cast<SelectInst>(V)) {
          walkBackward(SEL->getTrueValue(), visited, localResult);
          walkBackward(SEL->getFalseValue(), visited, localResult);
          result |= localResult;
          ddaCache[V] = localResult;
          return;
      }

      // extractvalue / insertvalue — recurse into aggregate operand
      if (auto *EV = dyn_cast<ExtractValueInst>(V)) {
          walkBackward(EV->getAggregateOperand(), visited, localResult);
          result |= localResult;
          ddaCache[V] = localResult;
          return;
      }

      // zext/sext/trunc — recurse into operand
      if (auto *CI = dyn_cast<CastInst>(V)) {
          walkBackward(CI->getOperand(0), visited, localResult);
          result |= localResult;
          ddaCache[V] = localResult;
          return;
      }

      // call instruction — check if it returns an integer that might be a pointer
      if (auto *Call = dyn_cast<CallInst>(V)) {
          if (llvmModuleSet->hasValueNode(Call)) {
              NodeID nid = llvmModuleSet->getValueNode(Call);
              const PointsTo &pts = ander->getPts(nid);
              if (!pts.empty()) {
                  localResult |= pts;
              }
          }
          result |= localResult;
          ddaCache[V] = localResult;
          return;
      }

      // load instruction — andersen-guided store matching
      // strategy: try same-function first (fast), then module-wide if needed
      if (auto *LI = dyn_cast<LoadInst>(V)) {
          Value *loadPtr = LI->getPointerOperand();
          if (!llvmModuleSet->hasValueNode(loadPtr)) {
              LLVM_DEBUG(dbgs() << "[DDA-load] no SVF node for load ptr: " << *loadPtr << "\n");
              ddaCache[V] = localResult;
              return;
          }
          NodeID loadPtrNode = llvmModuleSet->getValueNode(loadPtr);
          const PointsTo &loadPts = ander->getPts(loadPtrNode);
          Function *LIFunc = LI->getFunction();

          // helper lambda to check alias and recurse
          auto checkStoreAlias = [&](StoreInst *SI) {
              Value *storePtr = SI->getPointerOperand();
              bool alias = false;
              if (storePtr == loadPtr) {
                  alias = true;
              } else {
                  int64_t loadOff = 0, storeOff = 0;
                  AllocaInst *loadAI = getOffsetFromAlloca(loadPtr, loadOff);
                  AllocaInst *storeAI = getOffsetFromAlloca(storePtr, storeOff);
                  if (loadAI && storeAI && loadAI == storeAI && loadOff == storeOff) {
                      alias = true;
                  } else if (!loadPts.empty() && llvmModuleSet->hasValueNode(storePtr)) {
                      NodeID storePtrNode = llvmModuleSet->getValueNode(storePtr);
                      const PointsTo &storePts = ander->getPts(storePtrNode);
                      PointsTo isect = loadPts;
                      isect &= storePts;
                      if (!isect.empty()) alias = true;
                  }
              }
              if (alias) {
                  walkBackward(SI->getValueOperand(), visited, localResult);
              }
          };

          // phase 1: same-function search (fast path)
          if (LIFunc) {
              for (BasicBlock &BB : *LIFunc) {
                  for (Instruction &Inst : BB) {
                      if (auto *SI = dyn_cast<StoreInst>(&Inst)) {
                          checkStoreAlias(SI);
                      }
                  }
              }
          }

          // phase 2: if same-function failed and we have andersen pts info,
          // do module-wide search using ONLY andersen-guided matching (skip alloca matching)
          if (localResult.empty() && !loadPts.empty()) {
              for (Function &Func : M) {
                  if (&Func == LIFunc) continue;  // already searched
                  if (Func.isDeclaration() || Func.getName().starts_with("__svf_")) continue;
                  for (BasicBlock &BB : Func) {
                      for (Instruction &Inst : BB) {
                          auto *SI = dyn_cast<StoreInst>(&Inst);
                          if (!SI) continue;
                          Value *storePtr = SI->getPointerOperand();
                          // only use andersen-guided matching for cross-function
                          if (llvmModuleSet->hasValueNode(storePtr)) {
                              NodeID storePtrNode = llvmModuleSet->getValueNode(storePtr);
                              const PointsTo &storePts = ander->getPts(storePtrNode);
                              PointsTo isect = loadPts;
                              isect &= storePts;
                              if (!isect.empty()) {
                                  walkBackward(SI->getValueOperand(), visited, localResult);
                              }
                          }
                      }
                  }
              }
          }

          // phase 3: if both phases failed, try alloca-offset matching across functions.
          // When the load pointer is a GEP from an alloca passed to a callee,
          // Andersen's field-sensitive analysis may create separate field objects
          // for the GEPs in caller vs callee, causing empty intersection.
          // Fall back to matching by base alloca + constant offset.
          if (localResult.empty()) {
              int64_t loadOff = 0;
              AllocaInst *loadAI = getOffsetFromAlloca(loadPtr, loadOff);
              if (loadAI) {
                  // find all call sites passing this alloca as an argument
                  for (User *U : loadAI->users()) {
                      CallBase *CB = dyn_cast<CallBase>(U);
                      if (!CB) {
                          // might be a GEP/bitcast used by a call
                          for (User *UU : U->users()) {
                              if (auto *cb = dyn_cast<CallBase>(UU))
                                  CB = cb;
                          }
                      }
                      if (!CB) continue;
                      Function *Callee = CB->getCalledFunction();
                      if (!Callee || Callee->isDeclaration()) continue;

                      // find which argument corresponds to our alloca
                      for (unsigned i = 0; i < CB->arg_size(); ++i) {
                          Value *arg = CB->getArgOperand(i);
                          // strip GEPs/bitcasts to find if arg derives from loadAI
                          int64_t argOff = 0;
                          AllocaInst *argAI = getOffsetFromAlloca(arg, argOff);
                          if (argAI != loadAI) continue;

                          // the formal parameter in the callee
                          Argument *formalArg = Callee->getArg(i);
                          // target offset in callee = loadOff - argOff
                          int64_t targetOff = loadOff - argOff;

                          // search callee for stores at same offset from this arg
                          for (BasicBlock &BB : *Callee) {
                              for (Instruction &Inst : BB) {
                                  auto *SI = dyn_cast<StoreInst>(&Inst);
                                  if (!SI) continue;
                                  int64_t storeOff = 0;
                                  Value *storeBase = SI->getPointerOperand();
                                  // walk GEP chain from store ptr
                                  Value *curr = storeBase;
                                  int64_t off = 0;
                                  bool valid = true;
                                  while (curr) {
                                      if (auto *GEP = dyn_cast<GetElementPtrInst>(curr)) {
                                          APInt gepOff(64, 0);
                                          if (GEP->accumulateConstantOffset(DL, gepOff)) {
                                              off += gepOff.getSExtValue();
                                              curr = GEP->getPointerOperand();
                                          } else {
                                              valid = false;
                                              break;
                                          }
                                      } else if (auto *BC = dyn_cast<BitCastInst>(curr)) {
                                          curr = BC->getOperand(0);
                                      } else {
                                          break;
                                      }
                                  }
                                  if (!valid) continue;
                                  // check if base is the formal arg and offset matches
                                  if (curr == formalArg && off == targetOff) {
                                      Value *storeVal = SI->getValueOperand();
                                      LLVM_DEBUG(dbgs() << "[DDA-load] phase3 MATCH in "
                                                        << Callee->getName() << " off=" << off << "\n");
                                      // Option A: use Andersen directly for pointer-typed stored values
                                      // to bypass visited/ddaCache poisoning from earlier walk phases
                                      if (storeVal->getType()->isPointerTy() && llvmModuleSet->hasValueNode(storeVal)) {
                                          NodeID nid = llvmModuleSet->getValueNode(storeVal);
                                          const PointsTo &svPts = ander->getPts(nid);
                                          if (!svPts.empty()) {
                                              localResult |= svPts;
                                          } else {
                                              walkBackward(storeVal, visited, localResult);
                                          }
                                      } else {
                                          walkBackward(storeVal, visited, localResult);
                                      }
                                  }
                              }
                          }
                      }
                  }
              }
          }

          result |= localResult;
          ddaCache[V] = localResult;
          return;
      }

      // constants (integer literals) — not pointer-derived, skip
      ddaCache[V] = localResult;
  };

  // phase 3: run dda and merge results into andersen pts
  int patchedCount = 0;
  int failedCount = 0;
  for (auto &[ITP, nodeId] : brokenIntToPtrs) {
      SmallPtrSet<Value*, 32> visited;
      PointsTo ddaResult;
      walkBackward(ITP->getOperand(0), visited, ddaResult);

      if (!ddaResult.empty()) {
          ander->unionPts(nodeId, ddaResult);
          patchedIntToPtrTargets[ITP] = ddaResult;
          LLVM_DEBUG(dbgs() << "[DDA] patched: " << *ITP << " -> " << ddaResult.count()
                            << " targets in " << ITP->getFunction()->getName() << "\n");
          patchedCount++;
      } else {
          LLVM_DEBUG(dbgs() << "[DDA] FAILED: " << *ITP
                            << " in " << ITP->getFunction()->getName() << "\n");
          failedCount++;
      }
  }

  if (!brokenIntToPtrs.empty()) {
      LLVM_DEBUG(dbgs() << "[UnsafeHeapAllocAnalysis] DDA: patched " << patchedCount
                        << ", failed " << failedCount << " (cache entries: " << ddaCache.size() << ")\n");
  }


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
                          SmallPtrSet<Value*, 32> fwdVisited;
                          PointsTo ddaPts;
                          walkBackward(Ptr, fwdVisited, ddaPts);
                          if (!ddaPts.empty()) {
                              LLVM_DEBUG(dbgs() << "[DDA] forward prop: " << ddaPts.count() << " targets for " << *Ptr << "\n");
                              extractTargets(ddaPts);
                          }
                      }

                      // Option D: IntToPtr forwarding heuristic
                      // If still no heap targets, walk the pointer's GEP/bitcast/IntToPtr
                      // chain looking for a DDA-patched IntToPtr and inherit its targets.
                      if (heapTargets.empty() && !patchedIntToPtrTargets.empty()) {
                          SmallPtrSet<Value*, 16> seen;
                          std::function<bool(Value*)> walkChain;
                          walkChain = [&](Value *V) -> bool {
                              if (!V || !seen.insert(V).second) return false;
                              auto it = patchedIntToPtrTargets.find(V);
                              if (it != patchedIntToPtrTargets.end()) {
                                  extractTargets(it->second);
                                  LLVM_DEBUG(dbgs() << "[DDA] IntToPtr fwd: inherited "
                                                    << it->second.count() << " targets from patched "
                                                    << *V << " for " << *Ptr << "\n");
                                  return true;
                              }
                              if (auto *GEP = dyn_cast<GetElementPtrInst>(V))
                                  return walkChain(GEP->getPointerOperand());
                              if (auto *BC = dyn_cast<BitCastInst>(V))
                                  return walkChain(BC->getOperand(0));
                              if (auto *PHI = dyn_cast<PHINode>(V)) {
                                  bool found = false;
                                  for (unsigned i = 0; i < PHI->getNumIncomingValues(); ++i)
                                      found |= walkChain(PHI->getIncomingValue(i));
                                  return found;
                              }
                              if (auto *SEL = dyn_cast<SelectInst>(V)) {
                                  return walkChain(SEL->getTrueValue()) |
                                         walkChain(SEL->getFalseValue());
                              }
                              // for loads, check if any stored value derives from a patched IntToPtr
                              if (auto *LI = dyn_cast<LoadInst>(V)) {
                                  Value *loadPtr = LI->getPointerOperand();
                                  if (llvmModuleSet->hasValueNode(loadPtr)) {
                                      NodeID loadNode = llvmModuleSet->getValueNode(loadPtr);
                                      const PointsTo &loadPts = ander->getPts(loadNode);
                                      if (!loadPts.empty()) {
                                          Function *F = LI->getFunction();
                                          if (F) {
                                              for (BasicBlock &BB2 : *F) {
                                                  for (Instruction &Inst2 : BB2) {
                                                      if (auto *SI = dyn_cast<StoreInst>(&Inst2)) {
                                                          Value *storePtr = SI->getPointerOperand();
                                                          if (llvmModuleSet->hasValueNode(storePtr)) {
                                                              NodeID storeNode = llvmModuleSet->getValueNode(storePtr);
                                                              const PointsTo &storePts = ander->getPts(storeNode);
                                                              PointsTo isect = loadPts;
                                                              isect &= storePts;
                                                              if (!isect.empty()) {
                                                                  if (walkChain(SI->getValueOperand()))
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
                          };
                          walkChain(Ptr);
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
