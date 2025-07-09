#include "CpuCycleCountRuntime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <atomic>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <time.h>

#define CYCLE_OUTPUT_FILE "/tmp/cpu_cycle.stat"

static void print_cycles_on_exit(void);

static std::atomic<uint64_t> total_unsafe_cycles{0};
static std::atomic<uint64_t> total_unsafe_blocks{0};
static std::atomic<uint64_t> program_start_cycles{0};
static std::atomic<uint64_t> total_program_cycles{0};
static std::atomic<bool> program_cycle_tracking_enabled{false};
static std::atomic<bool> runtime_initialized{false};

static pthread_mutex_t cycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static void runtime_init(void) {
    runtime_initialized.store(true, std::memory_order_release);
    atexit(print_cycles_on_exit);
}

static void __attribute__((constructor)) ensure_init(void) {
    pthread_once(&init_once, runtime_init);
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
    if (!runtime_initialized.load(std::memory_order_acquire))
        return 0;
    return read_cpu_cycles();
}

RUNTIME_EXPORT void cpu_cycle_end_measurement(uint64_t start_cycle) {
    if (!runtime_initialized.load(std::memory_order_acquire) || start_cycle == 0)
        return;
    
    uint64_t end_cycle = read_cpu_cycles();
    uint64_t cycle_diff = (end_cycle >= start_cycle) ? 
                         (end_cycle - start_cycle) : 
                         (UINT64_MAX - start_cycle + end_cycle + 1);
    
    total_unsafe_cycles.fetch_add(cycle_diff, std::memory_order_relaxed);
    total_unsafe_blocks.fetch_add(1, std::memory_order_relaxed);
}

RUNTIME_EXPORT void cpu_cycle_program_start(void) {
    if (!runtime_initialized.load(std::memory_order_acquire))
        return;
    
    program_start_cycles.store(read_cpu_cycles(), std::memory_order_relaxed);
    program_cycle_tracking_enabled.store(true, std::memory_order_release);
}

RUNTIME_EXPORT void cpu_cycle_program_end(void) {
    if (!runtime_initialized.load(std::memory_order_acquire) || 
        !program_cycle_tracking_enabled.load(std::memory_order_acquire))
        return;
    
    uint64_t end_cycles = read_cpu_cycles();
    uint64_t start_cycles = program_start_cycles.load(std::memory_order_relaxed);
    
    if (end_cycles >= start_cycles) {
        total_program_cycles.store(end_cycles - start_cycles, std::memory_order_relaxed);
    }
}

static void write_aggregate_data(uint64_t total_cycles, uint64_t total_blocks, uint64_t program_cycles) {
    int fd = open(CYCLE_OUTPUT_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1) return;
    
    if (flock(fd, LOCK_EX) == -1) {
        close(fd);
        return;
    }
    
    double unsafe_percentage = (program_cycles > 0) ? 
                              (double)total_cycles / program_cycles * 100.0 : 0.0;
    
    char buffer[256];
    int len = snprintf(buffer, sizeof(buffer), 
                      "%lu,%lu,%lu,%.6f\n", 
                      (unsigned long)program_cycles,
                      (unsigned long)total_cycles,
                      (unsigned long)total_blocks,
                      unsafe_percentage);
    
    write(fd, buffer, len);
    flock(fd, LOCK_UN);
    close(fd);
}

static void print_cycles_on_exit(void) {
    print_cpu_cycle_stats();
}

RUNTIME_EXPORT void print_cpu_cycle_stats(void) {
    pthread_mutex_lock(&cycle_mutex);
    
    uint64_t total_cycles = total_unsafe_cycles.load(std::memory_order_acquire);
    uint64_t total_blocks = total_unsafe_blocks.load(std::memory_order_acquire);
    uint64_t program_cycles = total_program_cycles.load(std::memory_order_acquire);
    
    write_aggregate_data(total_cycles, total_blocks, program_cycles);
    
    if (total_blocks > 0) {
        double unsafe_percentage = (program_cycles > 0) ? 
                                  (double)total_cycles / program_cycles * 100.0 : 0.0;
        printf("[CpuCycleCount] %lu blocks, %.2f%% unsafe\n", 
               (unsigned long)total_blocks, unsafe_percentage);
    }
    
    pthread_mutex_unlock(&cycle_mutex);
}