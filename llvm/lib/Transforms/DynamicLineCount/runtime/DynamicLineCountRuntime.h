//===-- DynamicLineCountRuntime.h - Unsafe Line Coverage Runtime -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_DYNAMICLINECOUNT_RUNTIME_H
#define LLVM_TRANSFORMS_DYNAMICLINECOUNT_RUNTIME_H

#include <stdint.h>

// Platform independent export attribute
#if defined(_WIN32) || defined(_WIN64)
  #define RUNTIME_EXPORT __declspec(dllexport)
#else
  #define RUNTIME_EXPORT __attribute__((used, visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Core functions called by instrumented code
RUNTIME_EXPORT void update_unsafe_line_counter(int64_t LineNum, const char *File);
RUNTIME_EXPORT void mark_unsafe_line_executed(int64_t LineNum, const char *File);
RUNTIME_EXPORT void print_coverage_stats(void);
RUNTIME_EXPORT void total_unsafe_block_count(int64_t BlockSize);

#ifdef __cplusplus
}
#endif

#endif // LLVM_TRANSFORMS_DYNAMICLINECOUNT_RUNTIME_H
