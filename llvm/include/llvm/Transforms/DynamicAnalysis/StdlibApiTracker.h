//===-- StdlibApiTracker.h - Track stdlib API calls in unsafe code -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Function pass that instruments stdlib API calls inside unsafe SESE regions.
/// Reads InstMarker markers and !stdlib_call metadata emitted by rustc.
/// Must run after InstMarker and UnsafeFunctionTracker.
///
//===----------------------------------------------------------------------===//

// UNSAFE-RUST BEGIN
#ifndef LLVM_TRANSFORMS_DYNAMICANALYSIS_STDLIBAPI_TRACKER_H
#define LLVM_TRANSFORMS_DYNAMICANALYSIS_STDLIBAPI_TRACKER_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Function;

/// \brief Track stdlib API calls inside unsafe code regions.
///
/// This pass instruments CallBase instructions that:
/// 1. Lie inside a valid SESE region (between marker_begin/marker_end), AND
/// 2. Carry !stdlib_call metadata (attached by rustc for calls to std/core/alloc)
///
/// For each such call, it inserts a runtime call to
/// __unsafe_record_stdlib_call(api_str_ptr, api_str_len) which records
/// per-API invocation counts.
struct StdlibApiTrackerPass : public PassInfoMixin<StdlibApiTrackerPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_DYNAMICANALYSIS_STDLIBAPI_TRACKER_H
// UNSAFE-RUST END
