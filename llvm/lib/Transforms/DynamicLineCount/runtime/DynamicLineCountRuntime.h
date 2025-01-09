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

#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef LLVM_DYNAMICLINECOUNT_BUILD_SHARED
    #define RUNTIME_EXPORT __declspec(dllexport)
  #else
    #define RUNTIME_EXPORT __declspec(dllimport)
  #endif
#else
  #ifdef LLVM_DYNAMICLINECOUNT_BUILD_SHARED
    #define RUNTIME_EXPORT __attribute__((visibility("default")))
  #else
    #define RUNTIME_EXPORT
  #endif
#endif

extern "C" {

RUNTIME_EXPORT void update_line_counter(int64_t LineNum, const char *File);
RUNTIME_EXPORT void update_unsafe_line_counter(int64_t LineNum, const char *File);
RUNTIME_EXPORT void mark_line_executed(int64_t LineNum, const char *File);
RUNTIME_EXPORT void mark_unsafe_line_executed(int64_t LineNum, const char *File);
RUNTIME_EXPORT void print_coverage_stats(void);

} // extern "C"

// TEST USE ONLY
#ifndef RUNTIME_BUFFER_SIZE
#define RUNTIME_BUFFER_SIZE 1000
#endif

#endif // LLVM_TRANSFORMS_DYNAMICLINECOUNT_RUNTIME_H
