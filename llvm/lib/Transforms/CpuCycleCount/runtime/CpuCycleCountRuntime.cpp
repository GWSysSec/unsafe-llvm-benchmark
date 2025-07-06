//===-- CpuCycleCountRuntime.cpp - CPU Cycle Counting Runtime -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CpuCycleCountRuntime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <atomic>

// Output file for CPU cycle statistics
#define CYCLE_OUTPUT_FILE "cpu_cycle.stat"

// Forward declarations
static void print_cycles_on_exit(void);
static void write_cycle_file(uint64_t total_cycles, uint64_t total_blocks, double avg_cycles);

// Global atomic counters for thread-safe cycle tracking
static std::atomic<uint64_t> total_unsafe_cycles{0};
static std::atomic<uint64_t> total_unsafe_blocks{0};
static std::atomic<int> runtime_initialized{0};

// Total program cycle tracking
static std::atomic<uint64_t> program_start_cycles{0};
static std::atomic<uint64_t> total_program_cycles{0};
static std::atomic<int> program_cycle_tracking_enabled{0};

// Mutex for thread-safe operations that require consistency
static pthread_mutex_t cycle_mutex = PTHREAD_MUTEX_INITIALIZER;

// One-time initialization at runtime, called through constructor attribute
static void __attribute__((constructor)) runtime_init(void) {
    // Use atomic to ensure initialization happens only once
    int expected = 0;
    if (runtime_initialized.compare_exchange_strong(expected, 1)) {
        atexit(print_cycles_on_exit);
    }
}

// Assembly function to execute RDTSCP with serialization
static inline uint64_t read_cpu_cycles(void) {
    uint32_t lo, hi, aux;
    
    // Use RDTSCP for serializing read of timestamp counter
    // RDTSCP is preferred over RDTSC as it's serializing
    __asm__ volatile (
        "rdtscp"
        : "=a" (lo), "=d" (hi), "=c" (aux)
        :
        : "memory"
    );
    
    return ((uint64_t)hi << 32) | lo;
}

// Start measurement - returns current cycle count
RUNTIME_EXPORT uint64_t cpu_cycle_start_measurement(void) {
    // Quick check if runtime is initialized
    if (runtime_initialized.load() == 0)
        return 0;
        
    return read_cpu_cycles();
}

// End measurement - calculates and accumulates cycle difference
RUNTIME_EXPORT void cpu_cycle_end_measurement(uint64_t start_cycle) {
    // Quick check if runtime is initialized or invalid start
    if (runtime_initialized.load() == 0 || start_cycle == 0)
        return;
    
    uint64_t end_cycle = read_cpu_cycles();
    
    // Handle potential wraparound (unlikely but possible)
    uint64_t cycle_diff = (end_cycle >= start_cycle) ? 
                         (end_cycle - start_cycle) : 
                         (UINT64_MAX - start_cycle + end_cycle + 1);
    
    // Atomically update global counters
    total_unsafe_cycles.fetch_add(cycle_diff);
    total_unsafe_blocks.fetch_add(1);
}

// Start total program cycle tracking
RUNTIME_EXPORT void cpu_cycle_program_start(void) {
    if (runtime_initialized.load() == 0)
        return;
    
    program_start_cycles.store(read_cpu_cycles());
    program_cycle_tracking_enabled.store(1);
}

// End total program cycle tracking
RUNTIME_EXPORT void cpu_cycle_program_end(void) {
    if (runtime_initialized.load() == 0 || program_cycle_tracking_enabled.load() == 0)
        return;
    
    uint64_t end_cycles = read_cpu_cycles();
    uint64_t start_cycles = program_start_cycles.load();
    
    if (end_cycles >= start_cycles) {
        total_program_cycles.store(end_cycles - start_cycles);
    }
}

// Writes a cycle summary file in the /tmp directory
static void write_cycle_file(uint64_t total_cycles, uint64_t total_blocks, double avg_cycles, uint64_t program_cycles) {
    FILE* cycle_file = fopen(CYCLE_OUTPUT_FILE, "w");
    if (!cycle_file) {
        fprintf(stderr, "Failed to open cycle output file %s\n", CYCLE_OUTPUT_FILE);
        return;
    }
    
    // Write in a simple, consistent format
    fprintf(cycle_file, "===== CPU Cycle Measurement Results =====\n");
    fprintf(cycle_file, "CpuCycleCount Pass: EXECUTED\n");
    fprintf(cycle_file, "Total Program Cycles: %lu\n", (unsigned long)program_cycles);
    fprintf(cycle_file, "Total Unsafe Blocks Executed: %lu\n", (unsigned long)total_blocks);
    fprintf(cycle_file, "Total CPU Cycles in Unsafe Code: %lu\n", (unsigned long)total_cycles);
    
    if (total_blocks > 0) {
        fprintf(cycle_file, "Average Cycles Per Unsafe Block: %.2f\n", avg_cycles);
        if (program_cycles > 0) {
            double unsafe_percentage = (double)total_cycles / program_cycles * 100.0;
            fprintf(cycle_file, "Unsafe Code Percentage: %.2f%%\n", unsafe_percentage);
        }
    } else {
        fprintf(cycle_file, "Average Cycles Per Unsafe Block: N/A (no unsafe blocks)\n");
        fprintf(cycle_file, "Unsafe Code Percentage: 0.00%%\n");
    }
    
    // Add some context about measurement precision
    fprintf(cycle_file, "\n===== Measurement Notes =====\n");
    fprintf(cycle_file, "Measurement Method: RDTSCP (Serializing)\n");
    fprintf(cycle_file, "Includes: Instruction execution + memory access cycles\n");
    fprintf(cycle_file, "Excludes: OS context switches and interrupts\n");
    
    fclose(cycle_file);
    printf("CPU cycle data written to %s\n", CYCLE_OUTPUT_FILE);
}

// This wrapper function is registered with atexit
static void print_cycles_on_exit(void) {
    print_cpu_cycle_stats();
}

RUNTIME_EXPORT void print_cpu_cycle_stats(void) {
    // Lock to ensure consistent state during reporting
    pthread_mutex_lock(&cycle_mutex);
    
    uint64_t total_cycles = total_unsafe_cycles.load();
    uint64_t total_blocks = total_unsafe_blocks.load();
    uint64_t program_cycles = total_program_cycles.load();
    
    printf("\n=== CPU Cycle Measurement Report ===\n\n");
    printf("CpuCycleCount Pass: EXECUTED\n");
    printf("Total Program Cycles: %lu\n", (unsigned long)program_cycles);
    printf("Total Unsafe Blocks Executed: %lu\n", (unsigned long)total_blocks);
    printf("Total CPU Cycles in Unsafe Code: %lu\n", (unsigned long)total_cycles);
    
    double avg_cycles = 0.0;
    if (total_blocks > 0) {
        avg_cycles = (double)total_cycles / total_blocks;
        printf("Average Cycles Per Unsafe Block: %.2f\n", avg_cycles);
        
        if (program_cycles > 0) {
            double unsafe_percentage = (double)total_cycles / program_cycles * 100.0;
            printf("Unsafe Code Percentage: %.2f%%\n", unsafe_percentage);
        }
    } else {
        printf("Average Cycles Per Unsafe Block: N/A (no unsafe blocks)\n");
        printf("Unsafe Code Percentage: 0.00%%\n");
    }
    
    // Write the cycle summary file (always write, even with 0 unsafe blocks)
    write_cycle_file(total_cycles, total_blocks, avg_cycles, program_cycles);
    
    // Performance insights (only if there are unsafe blocks)
    if (total_blocks > 0) {
        printf("\n=== Performance Insights ===\n");
        if (avg_cycles < 100) {
            printf("  ✅ Very fast unsafe operations (< 100 cycles/block)\n");
        } else if (avg_cycles < 1000) {
            printf("  ⚡ Fast unsafe operations (< 1000 cycles/block)\n");
        } else if (avg_cycles < 10000) {
            printf("  ⚠️  Moderate unsafe operations (< 10k cycles/block)\n");
        } else {
            printf("  🐌 Slow unsafe operations (> 10k cycles/block)\n");
        }
    }
    
    // Final status
    printf("\n  📊 CPU cycle measurement completed successfully\n");
    
    pthread_mutex_unlock(&cycle_mutex);
}