//===-- DynamicLineCount.h - Tracking unsafe instructions ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines a function pass that tracks and instruments unsafe Rust
// instructions, building on the InstMarker analysis to ensure runtime compatibility.
// It focuses specifically on per-line instrumentation for dynamic analysis.
//
// The pass uses a two-phase approach to track line coverage:
// 1. Compile time: All unsafe lines are registered with update_unsafe_line_counter
//    This provides the denominator for the coverage calculation (total unsafe lines)
// 2. Runtime: When unsafe code is executed, mark_unsafe_line_executed is called
//    This provides the numerator for the coverage calculation (executed unsafe lines)
//
// At program exit, coverage is calculated as (executed unsafe lines / total unsafe lines)
// per file and for the entire program. This approach allows accurate coverage reporting
// even if not all unsafe code paths are executed during a test run.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_DYNAMICLINECOUNT_DYNAMICLINECOUNT_H
#define LLVM_TRANSFORMS_DYNAMICLINECOUNT_DYNAMICLINECOUNT_H

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Function.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"

namespace llvm {

// Runtime function name constants specific to DynamicLineCount
// These must match the exported symbols in the runtime library
inline constexpr const char *UPDATE_UNSAFE_LINE_FN = "update_unsafe_line_counter";
inline constexpr const char *MARK_UNSAFE_LINE_FN = "mark_unsafe_line_executed";
// Block count function directly referenced in cpp file

/// DynamicLineCountPass - This pass instruments code with calls to the runtime 
/// library to track execution of unsafe Rust code at the line level.
///
/// It depends on InstMarker pass for identification of unsafe instructions.
/// The pass adds two types of instrumentation:
/// 1. Module constructor calls to register all unsafe lines
/// 2. Runtime execution tracking at each unsafe instruction site
struct DynamicLineCountPass : PassInfoMixin<DynamicLineCountPass> {
  /// Main entry point - instruments unsafe code for runtime tracking
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  
  /// This pass is required for unsafe code tracking
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_DYNAMICLINECOUNT_DYNAMICLINECOUNT_H
