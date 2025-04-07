//===-- DynamicLineCountRuntime.h - Runtime for Line Counter Pass ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_DYNAMICLINECOUNT_RUNTIME_H
#define LLVM_TRANSFORMS_DYNAMICLINECOUNT_RUNTIME_H

#include <cstdint>

// Use static library macro definitions
#define RUNTIME_EXPORT __attribute__((used, noinline))

extern "C" {

// Core functions for unsafe line tracking
RUNTIME_EXPORT void update_unsafe_line_counter(int64_t LineNum, const char *File);
RUNTIME_EXPORT void mark_unsafe_line_executed(int64_t LineNum, const char *File);
RUNTIME_EXPORT void print_coverage_stats(void);

// Optional function for total block counts (can be used by InstMarker)
RUNTIME_EXPORT void total_unsafe_block_count(int64_t BlockSize);

} // extern "C"

#endif // LLVM_TRANSFORMS_DYNAMICLINECOUNT_RUNTIME_H
