#ifndef LLVM_TRANSFORMS_SVFANALYSIS_INTTOPTRDDA_H
#define LLVM_TRANSFORMS_SVFANALYSIS_INTTOPTRDDA_H

/// IntToPtrDDA — Demand-Driven backward def-use walker for IntToPtr recovery.
///
/// SVF's Andersen pointer analysis cannot track pointer provenance through
/// integer arithmetic (ptrtoint -> add/sub/and -> inttoptr).  This class
/// bridges that gap by walking LLVM IR def-use chains backward from "broken"
/// IntToPtr instructions (those with empty Andersen points-to sets) and
/// resolving their pointer origins.
///
/// Usage (from UnsafeHeapAllocAnalysis):
///   IntToPtrDDA dda(M, pag, ander, llvmModuleSet);
///   dda.run();                           // audit + backward walk + patch
///   // Query resolved targets for a specific SESE pointer:
///   PointsTo pts = dda.resolveTargets(Ptr);

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Instructions.h"

// Forward-declare SVF types to avoid pulling SVF headers into every includer.
// The .cpp includes the real headers.
namespace SVF {
class SVFIR;
class Andersen;
class LLVMModuleSet;
class PointsTo;
} // namespace SVF

namespace llvm {

class Module;

class IntToPtrDDA {
public:
  IntToPtrDDA(Module &M, SVF::SVFIR *PAG, SVF::Andersen *Ander,
              SVF::LLVMModuleSet *LMS);

  /// Audit broken IntToPtrs, walk backward, and patch Andersen points-to sets.
  void run();

  /// Resolve heap targets for a pointer that may derive from a patched
  /// IntToPtr.  First tries a fresh backward walk; if that yields nothing,
  /// falls back to Option D forward chain-walking through GEP/PHI/Select/Load
  /// looking for patched IntToPtr ancestors.
  /// Returns the resolved PointsTo set (may be empty).
  SVF::PointsTo resolveTargets(Value *Ptr);

  /// Statistics from the last run().
  unsigned getPatchedCount() const { return PatchedCount; }
  unsigned getFailedCount() const { return FailedCount; }

private:
  Module &M;
  SVF::SVFIR *PAG;
  SVF::Andersen *Ander;
  SVF::LLVMModuleSet *LMS;

  /// Global memoization cache — avoids redundant backward traversals.
  DenseMap<Value *, SVF::PointsTo> Cache;

  /// Map from patched IntToPtr instructions to their resolved heap targets.
  /// Populated by run(), consumed by resolveTargets() for Option D forwarding.
  DenseMap<Value *, SVF::PointsTo> PatchedTargets;

  unsigned PatchedCount = 0;
  unsigned FailedCount = 0;

  /// Recursive backward walk through LLVM def-use chains.
  void walkBackward(Value *V, SmallPtrSetImpl<Value *> &Visited,
                    SVF::PointsTo &Result);

  /// Extract constant byte offset from an alloca through a GEP/bitcast chain.
  /// Returns the alloca if found, nullptr otherwise.
  AllocaInst *getOffsetFromAlloca(Value *Ptr, int64_t &Offset);

  /// Option D: walk GEP/bitcast/PHI/Select/Load chains forward looking for a
  /// value in PatchedTargets.  Accumulates found targets into HeapTargets.
  bool walkChain(Value *V, SmallPtrSetImpl<Value *> &Seen,
                 SVF::PointsTo &HeapTargets);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_SVFANALYSIS_INTTOPTRDDA_H
