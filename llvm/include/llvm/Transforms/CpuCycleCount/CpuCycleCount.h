#ifndef LLVM_TRANSFORMS_CPUCYCLECOUNT_CPUCYCLECOUNT_H
#define LLVM_TRANSFORMS_CPUCYCLECOUNT_CPUCYCLECOUNT_H

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Module.h"

namespace llvm {

inline constexpr const char *CPU_CYCLE_START_FN = "cpu_cycle_start_measurement";
inline constexpr const char *CPU_CYCLE_END_FN = "cpu_cycle_end_measurement";
inline constexpr const char *CPU_CYCLE_STATS_FN = "print_cpu_cycle_stats";

struct CpuCycleCountPass : PassInfoMixin<CpuCycleCountPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_CPUCYCLECOUNT_CPUCYCLECOUNT_H