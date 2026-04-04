//===-- UnsafeAnalysisUtils.h - Shared utilities for unsafe analysis passes -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------------------===//
///
/// \file
/// Shared utilities used by all dynamic analysis passes (HeapTracker,
/// CpuCycleCount, ExternalCallTracker, UnsafeFunctionTracker,
/// UnsafeInstCounter, DynamicLineCount). Eliminates duplication of
/// isPrimaryPackage(), getInstrumentationDebugLoc(), and marker detection.
///
//===---------------------------------------------------------------------------------===//

// UNSAFE-RUST BEGIN
#ifndef LLVM_TRANSFORMS_DYNAMICANALYSIS_UNSAFEANALYSISUTILS_H
#define LLVM_TRANSFORMS_DYNAMICANALYSIS_UNSAFEANALYSISUTILS_H

#include "llvm/IR/DebugLoc.h"

namespace llvm {

class Instruction;
class CallBase;

/// \brief Check if this compilation unit is the primary Cargo package.
///
/// All dynamic analysis passes must only instrument the primary crate,
/// not dependencies. This reads the CARGO_PRIMARY_PACKAGE env var.
bool isPrimaryPackage();

/// \brief Get a valid debug location for compiler-inserted instrumentation.
///
/// ThinLTO post-link requires all call instructions in functions with debug
/// info to carry a !dbg location. This function searches for a nearby !dbg:
/// 1. The insertion point instruction's own !dbg
/// 2. Any instruction in the same basic block
/// 3. A synthetic line-0 location from the function's DISubprogram
///    (standard LLVM pattern, matches AddressSanitizer/MemorySanitizer)
/// 4. Empty DebugLoc if no debug info exists at all
DebugLoc getInstrumentationDebugLoc(Instruction *InsertBefore);

/// \brief Check if an instruction is an InstMarker marker_begin.
bool isMarkerBegin(const Instruction &I);

/// \brief Check if an instruction is an InstMarker marker_end.
bool isMarkerEnd(const Instruction &I);

/// \brief Check if an instruction is any InstMarker marker, setting flags.
///
/// Returns true if the instruction is a marker. On return, exactly one of
/// \p IsBegin or \p IsEnd is true.
bool isMarkerInstruction(const Instruction &I, bool &IsBegin, bool &IsEnd);

/// \brief Check if a CallBase is an inline asm marker matching a specific string.
///
/// Useful for passes (like CpuCycleCount) that work with CallBase* directly.
bool isMarkerAsm(const CallBase *Call, const char *MarkerStr);

class Function;

/// \brief Verify that unsafe markers in a function are well-formed.
///
/// Checks performed (all within LLVM_DEBUG — zero cost in release builds):
/// - Every marker_begin has a matching marker_end within the same BB
/// - No interleaving: markers nest properly (no begin-begin-end-end)
/// - No orphaned markers (begin without end or vice versa in a BB)
///
/// Returns true if all markers are valid, false if any issue is found.
/// Intended as a debug assertion callable from any downstream pass.
bool verifyUnsafeMarkers(Function &F);

} // namespace llvm

#endif // LLVM_TRANSFORMS_DYNAMICANALYSIS_UNSAFEANALYSISUTILS_H
// UNSAFE-RUST END
