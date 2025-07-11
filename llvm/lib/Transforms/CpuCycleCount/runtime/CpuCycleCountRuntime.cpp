#include "CpuCycleCountRuntime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <atomic>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>

#define CYCLE_OUTPUT_FILE "/tmp/cpu_cycle.stat"

static void print_cycles_on_exit(void);
static inline uint64_t read_cpu_cycles(void);

static std::atomic<uint64_t> total_unsafe_cycles{0};
static std::atomic<uint64_t> total_unsafe_blocks{0};
static uint64_t program_begin_cycles = 0;
static uint64_t program_end_cycles = 0;
static std::atomic<bool> initialized{false};
static std::atomic<bool> stats_written{false};

static void __attribute__((constructor)) runtime_init(void) {
    if (!initialized.exchange(true)) {
        program_begin_cycles = read_cpu_cycles();
        atexit(print_cycles_on_exit);
    }
}

static inline uint64_t read_cpu_cycles(void) {
    uint32_t lo, hi, aux;
    __asm__ volatile (
        "rdtscp"
        : "=a" (lo), "=d" (hi), "=c" (aux)
        :
        : "memory"
    );
    return ((uint64_t)hi << 32) | lo;
}

RUNTIME_EXPORT uint64_t cpu_cycle_start_measurement(void) {
    if (!initialized.load())
        return 0;
    return read_cpu_cycles();
}

RUNTIME_EXPORT void cpu_cycle_end_measurement(uint64_t start_cycle) {
    if (!initialized.load() || start_cycle == 0)
        return;
    
    uint64_t end_cycle = read_cpu_cycles();
    if (end_cycle > start_cycle) {
        total_unsafe_cycles.fetch_add(end_cycle - start_cycle, std::memory_order_relaxed);
        total_unsafe_blocks.fetch_add(1, std::memory_order_relaxed);
    }
}

RUNTIME_EXPORT void cpu_cycle_program_start(void) {
    if (!initialized.load())
        return;
    program_begin_cycles = read_cpu_cycles();
}

RUNTIME_EXPORT void cpu_cycle_program_end(void) {
    if (!initialized.load())
        return;
    program_end_cycles = read_cpu_cycles();
}

static void write_simple_stats(uint64_t program_cycles, uint64_t total_cycles, uint64_t total_blocks) {
    FILE* file = fopen(CYCLE_OUTPUT_FILE, "a");
    if (file) {
        fprintf(file, "%lu,%lu,%lu\n", 
                (unsigned long)program_cycles,
                (unsigned long)total_cycles,
                (unsigned long)total_blocks);
        fclose(file);
    }
}

static void print_cycles_on_exit(void) {
    if (!stats_written.exchange(true)) {
        uint64_t total_cycles = total_unsafe_cycles.load(std::memory_order_acquire);
        uint64_t total_blocks = total_unsafe_blocks.load(std::memory_order_acquire);
        
        uint64_t program_cycles;
        if (program_end_cycles > 0) {
            program_cycles = program_end_cycles - program_begin_cycles;
        } else {
            program_cycles = read_cpu_cycles() - program_begin_cycles;
        }
        
        write_simple_stats(program_cycles, total_cycles, total_blocks);

        if (total_blocks > 0){
            printf("[CpuCycleCount] %lu total cycles, %lu unsafe cycles, %lu unsafe blocks executed\n",
            (unsigned long) program_cycles, (unsigned long) total_cycles, (unsigned long)total_blocks); 
        }
    }
}

RUNTIME_EXPORT void print_cpu_cycle_stats(void) {
    print_cycles_on_exit();
}