#ifndef CPUCYCLECOUNTRUNTIME_H
#define CPUCYCLECOUNTRUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RUNTIME_EXPORT
#define RUNTIME_EXPORT __attribute__((visibility("default")))
#endif

RUNTIME_EXPORT uint64_t cpu_cycle_start_measurement(void);
RUNTIME_EXPORT void cpu_cycle_end_measurement(uint64_t start_cycle);

RUNTIME_EXPORT void print_cpu_cycle_stats(void);

RUNTIME_EXPORT void cpu_cycle_program_start(void);
RUNTIME_EXPORT void cpu_cycle_program_end(void);

#ifdef __cplusplus
}
#endif

#endif // CPUCYCLECOUNTRUNTIME_H