#ifndef LLVM_TRANSFORMS_SVFANALYSIS_UNSAFEHEAPALLOCANALYSIS_H
#define LLVM_TRANSFORMS_SVFANALYSIS_UNSAFEHEAPALLOCANALYSIS_H

#include "llvm/IR/PassManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include <vector>
#include <cstdint>

namespace llvm {

class Module;
class Instruction;

// We use uint32_t for NodeID to avoid including SVF headers here
using NodeID = uint32_t;

struct UnsafeHeapAllocAnalysisResult {
  // Map of Heap Allocation Node ID -> Allocation Size (0 if unknown)
  DenseMap<NodeID, uint64_t> HeapAllocSizes;
  
  // Map of Unsafe Instruction -> List of Heap Allocation Node IDs it points to
  DenseMap<const Instruction*, std::vector<NodeID>> UnsafePtrs;

  // Map of Allocation Instruction -> Node ID (for instrumentation)
  DenseMap<const Instruction*, NodeID> AllocationSites;
};

class UnsafeHeapAllocAnalysis : public AnalysisInfoMixin<UnsafeHeapAllocAnalysis> {
  friend AnalysisInfoMixin<UnsafeHeapAllocAnalysis>;
  static AnalysisKey Key;

public:
  using Result = UnsafeHeapAllocAnalysisResult;
  Result run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_SVFANALYSIS_UNSAFEHEAPALLOCANALYSIS_H
