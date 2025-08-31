//===-- UnsafeFunctionTracker.h - Track unsafe functions -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Module pass that assigns function IDs and tracks unsafe functions.
/// This pass must run before UnsafeInstCounter.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UNSAFECOUNT_UNSAFEFUNCTIONTRACKER_H
#define LLVM_TRANSFORMS_UNSAFECOUNT_UNSAFEFUNCTIONTRACKER_H

#include "llvm/IR/PassManager.h"
#include <cstdint>

namespace llvm {

class Module;
class Function;

/// \brief Module pass to track and assign IDs to functions.
///
/// This pass:
/// - Assigns unique IDs to all functions
/// - Creates a global metadata table with function information
/// - Instruments function entries to record calls
/// - Sets up runtime initialization
struct UnsafeFunctionTrackerPass : public PassInfoMixin<UnsafeFunctionTrackerPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  
  static bool isRequired() { return true; }
  static bool isPrimaryPackage();
  
  /// \brief Metadata stored for each function
  struct FunctionMetadata {
    uint32_t id;
    uint8_t hasUnsafeInst;
    uint8_t hasUnsafeRegions;
    uint16_t _padding;
  };
  
  /// \brief Name of the metadata node storing function IDs
  static constexpr const char *FUNCTION_ID_METADATA = "unsafe_count.func_id";
};

} // namespace llvm

#endif
