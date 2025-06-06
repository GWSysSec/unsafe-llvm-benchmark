//===-- CpuCycleCount.h - CPU cycle counting for unsafe code ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines a module pass that measures CPU cycles consumed by unsafe
// Rust code blocks. It builds upon InstMarker's marker_begin/marker_end assembly
// markers to identify unsafe code regions and uses RDTSCP intrinsics for precise
// cycle counting.
//
// The pass works by:
// 1. Scanning for marker_begin/marker_end inline assembly calls
// 2. Inserting RDTSCP measurements around these regions
// 3. Accumulating cycle counts in thread-safe global variables
// 4. Providing runtime statistics at program exit
//
// Metrics tracked:
// - Total CPU cycles consumed by unsafe code blocks
// - Number of unsafe blocks executed
// - Average cycles per unsafe block
// - Global cycle accumulation across all threads
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_CPUCYCLECOUNT_CPUCYCLECOUNT_H
#define LLVM_TRANSFORMS_CPUCYCLECOUNT_CPUCYCLECOUNT_H

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"

namespace llvm {

// Runtime function name constants for CpuCycleCount
// These must match the exported symbols in the runtime library
inline constexpr const char *CPU_CYCLE_START_FN = "cpu_cycle_start_measurement";
inline constexpr const char *CPU_CYCLE_END_FN = "cpu_cycle_end_measurement";
inline constexpr const char *CPU_CYCLE_STATS_FN = "print_cpu_cycle_stats";

/// CpuCycleCountPass - This pass instruments code to measure CPU cycles
/// consumed by unsafe Rust code blocks identified by InstMarker.
///
/// The pass operates at module level to:
/// 1. Insert cycle measurement calls around marker_begin/marker_end regions
/// 2. Manage global state for cycle accumulation
/// 3. Add statistics reporting at program exit
///
/// Dependencies:
/// - InstMarker: Provides marker_begin/marker_end assembly markers
/// - X86 RDTSCP intrinsic: For precise cycle counting
/// - Thread-safe runtime: For concurrent access protection
struct CpuCycleCountPass : PassInfoMixin<CpuCycleCountPass> {
  /// Main entry point - instruments unsafe code blocks for cycle measurement
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  
  /// This pass is required for unsafe code cycle analysis
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_CPUCYCLECOUNT_CPUCYCLECOUNT_H