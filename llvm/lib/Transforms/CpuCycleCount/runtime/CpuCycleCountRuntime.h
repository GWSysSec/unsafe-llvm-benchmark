//===-- CpuCycleCountRuntime.h - CPU Cycle Counting Runtime ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef CPUCYCLECOUNTRUNTIME_H
#define CPUCYCLECOUNTRUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Runtime export macro for consistent symbol visibility
#ifndef RUNTIME_EXPORT
#define RUNTIME_EXPORT __attribute__((visibility("default")))
#endif

// Core measurement functions
RUNTIME_EXPORT uint64_t cpu_cycle_start_measurement(void);
RUNTIME_EXPORT void cpu_cycle_end_measurement(uint64_t start_cycle);

// Statistics and reporting functions
RUNTIME_EXPORT void print_cpu_cycle_stats(void);

// Total program cycle tracking (independent of unsafe detection)
RUNTIME_EXPORT void cpu_cycle_program_start(void);
RUNTIME_EXPORT void cpu_cycle_program_end(void);

#ifdef __cplusplus
}
#endif

#endif // CPUCYCLECOUNTRUNTIME_H