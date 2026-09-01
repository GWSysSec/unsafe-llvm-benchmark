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
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace llvm {

class Instruction;
class CallBase;
class CallInst;
class DominatorTree;
class PostDominatorTree;

/// \brief Check whether this compilation unit should be instrumented.
///
/// By default the dynamic analysis passes instrument only the primary crate
/// and not its dependencies; that decision reads the CARGO_PRIMARY_PACKAGE
/// environment variable, which cargo sets on the primary package alone.
///
/// Setting UNSAFE_INSTRUMENT_ALL_PACKAGES=1 makes this return true for every
/// crate, so the whole dependency graph is instrumented. That is the opt-in
/// used by the whole-program measurement arm. Without the variable the
/// behaviour is unchanged.
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

/// \brief Check if an instruction is a stdlib call marker from the MIR pass.
///
/// These markers have the form: "# __unsafe_stdlib_call:{api_path}"
/// If true, \p ApiPath is set to the API path portion of the asm string.
bool isStdlibCallMarker(const Instruction &I, StringRef &ApiPath);

class Function;

/// \brief A validated SESE region bounded by begin/end markers.
///
/// A valid SESE region satisfies: Begin dominates End AND End post-dominates
/// Begin. This ensures the region has exactly one entry and one exit regardless
/// of CFG shape after optimization.
struct SESERegion {
  CallInst *Begin;
  CallInst *End;
};

/// \brief Collect all unsafe marker begin/end callsites in a function.
void collectMarkers(Function &F,
                    std::vector<CallInst *> &BeginMarkers,
                    std::vector<CallInst *> &EndMarkers);

/// \brief Validate SESE regions by matching begin/end markers using dominance.
///
/// A valid SESE region requires: begin dominates end AND end post-dominates
/// begin. Unmatched markers are logged via LLVM_DEBUG.
void validateSESERegions(const std::vector<CallInst *> &BeginMarkers,
                         const std::vector<CallInst *> &EndMarkers,
                         DominatorTree &DT, PostDominatorTree &PDT,
                         std::vector<SESERegion> &ValidRegions);

/// \brief Check if an instruction lies inside any valid SESE region.
///
/// Uses dominance: Begin's BB dominates I's BB AND End's BB post-dominates
/// I's BB.
bool isInSESERegion(const Instruction &I,
                    const std::vector<SESERegion> &ValidRegions,
                    DominatorTree &DT, PostDominatorTree &PDT);

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
