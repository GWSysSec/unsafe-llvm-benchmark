//===-- DynamicLineCountRuntime.cpp - Lightweight Coverage Runtime --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DynamicLineCountRuntime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <atomic>

// Simple data structures to avoid STL dependencies
#define MAX_FILES 500
#define MAX_LINES_PER_FILE 65536
#define COVERAGE_OUTPUT_FILE "/tmp/coverage_stat.stat"

// Forward declaration
static void print_coverage_on_exit(void);
static void write_coverage_file(int total_unsafe, int total_executed, float coverage);

// File tracking structure
typedef struct {
    const char* filename;           // Source file path
    unsigned int unsafe_lines[MAX_LINES_PER_FILE/32]; // Bitmap of unsafe lines
    unsigned int exec_map[MAX_LINES_PER_FILE/32];     // Bitmap of executed lines
    std::atomic<int> unsafe_count;  // Total unsafe lines in file
    std::atomic<int> exec_count;    // Total executed unsafe lines
} FileStats;

// Global state 
static FileStats files[MAX_FILES];
static std::atomic<int> file_count{0};
static std::atomic<int> total_blocks{0};
static std::atomic<int> total_instructions{0};
static std::atomic<int> runtime_initialized{0};

// Mutex for thread-safe file operations
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

// Simple find_file function - doesn't modify state, so no lock needed
static int find_file(const char* filename) {
    if (!filename) return -1;
    
    int current_count = file_count.load();
    for (int i = 0; i < current_count; i++) {
        if (files[i].filename && strcmp(files[i].filename, filename) == 0) {
            return i;
        }
    }
    return -1;
}

// Create file entry - needs mutex protection
static int create_file(const char* filename) {
    if (!filename || file_count.load() >= MAX_FILES) {
        return -1;
    }
    
    // Lock for thread safety during file creation
    pthread_mutex_lock(&file_mutex);
    
    // Double-check if file was added while waiting for lock
    int idx = find_file(filename);
    if (idx >= 0) {
        pthread_mutex_unlock(&file_mutex);
        return idx;
    }
    
    // Create new file entry
    idx = file_count.fetch_add(1);
    if (idx < MAX_FILES) {
        char* name_copy = strdup(filename);
        if (!name_copy) {
            file_count.fetch_sub(1); // Rollback increment
            pthread_mutex_unlock(&file_mutex);
            return -1;
        }
        
        files[idx].filename = name_copy;
        files[idx].unsafe_count.store(0);
        files[idx].exec_count.store(0);
        memset(files[idx].unsafe_lines, 0, sizeof(files[idx].unsafe_lines));
        memset(files[idx].exec_map, 0, sizeof(files[idx].exec_map));
    } else {
        idx = -1; // Overflow, undo increment
        file_count.fetch_sub(1);
    }
    
    pthread_mutex_unlock(&file_mutex);
    return idx;
}

// One-time initialization at runtime, called through constructor attribute
static void __attribute__((constructor)) runtime_init(void) {
    // Use atomic to ensure initialization happens only once
    int expected = 0;
    if (runtime_initialized.compare_exchange_strong(expected, 1)) {
        atexit(print_coverage_on_exit);
    }
}

// Thread-safe line registration function
RUNTIME_EXPORT void update_unsafe_line_counter(int64_t LineNum, const char* File) {
    if (!File || LineNum <= 0 || LineNum >= MAX_LINES_PER_FILE) 
        return;
    
    // Find or create file entry
    int idx = find_file(File);
    if (idx < 0) {
        idx = create_file(File);
    }
    
    if (idx >= 0 && idx < MAX_FILES) {
        // Add line if not already tracked
        FileStats* fs = &files[idx];
        int bitmap_idx = LineNum / 32;
        int bit_offset = LineNum % 32;
        
        if (bitmap_idx < (MAX_LINES_PER_FILE/32)) {
            // Use mutex for bitmap update to avoid race conditions
            pthread_mutex_lock(&file_mutex);
            if (!(fs->unsafe_lines[bitmap_idx] & (1U << bit_offset))) {
                fs->unsafe_lines[bitmap_idx] |= (1U << bit_offset);
                fs->unsafe_count.fetch_add(1);
            }
            pthread_mutex_unlock(&file_mutex);
        }
    }
}

// Thread-safe line execution tracking function
RUNTIME_EXPORT void mark_unsafe_line_executed(int64_t LineNum, const char* File) {
    if (!File || LineNum <= 0 || LineNum >= MAX_LINES_PER_FILE) 
        return;
    
    // Quick check using atomic read before doing more work
    if (runtime_initialized.load() == 0) 
        return;
    
    // Find file entry without creating a new one
    int idx = find_file(File);
    if (idx < 0 || idx >= MAX_FILES)
        return;
    
    // Mark line as executed
    FileStats* fs = &files[idx];
    int bitmap_idx = LineNum / 32;
    int bit_offset = LineNum % 32;
    
    if (bitmap_idx < (MAX_LINES_PER_FILE/32)) {
        // First check if the line is unsafe and not executed without locking
        // Read-only check first to avoid unnecessary locking
        unsigned int unsafe_bits = fs->unsafe_lines[bitmap_idx];
        unsigned int exec_bits = fs->exec_map[bitmap_idx];
        
        if ((unsafe_bits & (1U << bit_offset)) && !(exec_bits & (1U << bit_offset))) {
            // Now lock and do the update
            pthread_mutex_lock(&file_mutex);
            
            // Re-check after acquiring lock
            if ((fs->unsafe_lines[bitmap_idx] & (1U << bit_offset)) && 
                !(fs->exec_map[bitmap_idx] & (1U << bit_offset))) {
                
                fs->exec_map[bitmap_idx] |= (1U << bit_offset);
                fs->exec_count.fetch_add(1);
            }
            
            pthread_mutex_unlock(&file_mutex);
        }
    }
}

// Thread-safe block counting function
RUNTIME_EXPORT void total_unsafe_block_count(int64_t BlockSize) {
    // Simple atomic counters for thread safety
    total_blocks.fetch_add(1);
    total_instructions.fetch_add(BlockSize);
}

// Writes a coverage summary file in the /tmp directory
static void write_coverage_file(int total_unsafe, int total_executed, float coverage) {
    FILE* coverage_file = fopen(COVERAGE_OUTPUT_FILE, "w");
    if (!coverage_file) {
        fprintf(stderr, "Failed to open coverage output file %s\n", COVERAGE_OUTPUT_FILE);
        return;
    }
    
    // Write in a simple, consistent format like HeapTracker
    fprintf(coverage_file, "===== Unsafe Code Coverage Statistics =====\n");
    fprintf(coverage_file, "Total Unsafe Lines: %d\n", total_unsafe);
    fprintf(coverage_file, "Executed Unsafe Lines: %d\n", total_executed);
    fprintf(coverage_file, "Missing Unsafe Lines: %d\n", total_unsafe - total_executed);
    fprintf(coverage_file, "Coverage Percentage: %.2f%%\n", coverage);
    
    // Include block stats if available
    int blocks = total_blocks.load();
    if (blocks > 0) {
        int instructions = total_instructions.load();
        fprintf(coverage_file, "Total Unsafe Blocks: %d\n", blocks);
        fprintf(coverage_file, "Total Unsafe Instructions: %d\n", instructions);
        fprintf(coverage_file, "Avg Instructions Per Block: %.2f\n", (float)instructions / blocks);
    }
    
    fclose(coverage_file);
    printf("Coverage data written to %s\n", COVERAGE_OUTPUT_FILE);
}

// This wrapper function is registered with atexit
static void print_coverage_on_exit(void) {
    print_coverage_stats();
}

RUNTIME_EXPORT void print_coverage_stats(void) {
    // Lock to ensure consistent state during reporting
    pthread_mutex_lock(&file_mutex);
    
    int current_file_count = file_count.load();
    if (current_file_count == 0) {
        printf("No unsafe code was instrumented or executed.\n");
        pthread_mutex_unlock(&file_mutex);
        return;
    }
    
    printf("\n=== Unsafe Code Coverage Report ===\n\n");
    
    // Calculate totals and max filename length in a single pass
    int total_unsafe = 0;
    int total_executed = 0;
    size_t max_name_len = 20; // Minimum for "File" header
    
    for (int i = 0; i < current_file_count; i++) {
        if (files[i].filename) {
            size_t len = strlen(files[i].filename);
            if (len > max_name_len) max_name_len = len;
            
            int file_unsafe = files[i].unsafe_count.load();
            int file_executed = files[i].exec_count.load();
            
            total_unsafe += file_unsafe;
            total_executed += file_executed;
        }
    }
    
    // Calculate overall coverage
    float overall_coverage = total_unsafe > 0 ? 
        (float)total_executed * 100 / total_unsafe : 0;
    
    // Write the coverage summary file
    write_coverage_file(total_unsafe, total_executed, overall_coverage);
    
    // Print table header
    printf("%-*s | %10s | %10s | %10s | %10s\n", 
           (int)max_name_len, "File", "Unsafe", "Executed", "Missing", "Coverage");
    
    // Print separator
    char separator[256];
    size_t sep_len = max_name_len + 48;
    if (sep_len > sizeof(separator) - 1) 
        sep_len = sizeof(separator) - 1;
    
    memset(separator, '-', sep_len);
    separator[sep_len] = '\0';
    printf("%s\n", separator);
    
    // Print file data
    for (int i = 0; i < current_file_count; i++) {
        if (files[i].filename) {
            int file_unsafe = files[i].unsafe_count.load();
            
            if (file_unsafe > 0) {
                int file_executed = files[i].exec_count.load();
                int missing = file_unsafe - file_executed;
                float coverage = file_unsafe > 0 ? 
                    (float)file_executed * 100 / file_unsafe : 0;
                    
                printf("%-*s | %10d | %10d | %10d | %9.2f%%\n", 
                       (int)max_name_len, files[i].filename, 
                       file_unsafe, file_executed, missing, coverage);
                
                // Show missing lines (up to 5)
                if (missing > 0) {
                    printf("   Missing lines: ");
                    int missing_printed = 0;
                    
                    // More efficient bitmap scanning
                    for (int bitmap_idx = 0; bitmap_idx < (MAX_LINES_PER_FILE/32) && missing_printed < 5; bitmap_idx++) {
                        unsigned int missing_bits = files[i].unsafe_lines[bitmap_idx] & ~files[i].exec_map[bitmap_idx];
                        
                        // Skip if no missing bits in this word
                        if (missing_bits == 0)
                            continue;
                            
                        // Process bits in the word
                        for (int bit = 0; bit < 32 && missing_printed < 5; bit++) {
                            if (missing_bits & (1U << bit)) {
                                int line = bitmap_idx * 32 + bit;
                                printf("%d", line);
                                missing_printed++;
                                
                                if (missing_printed < 5 && missing_printed < missing) {
                                    printf(", ");
                                }
                            }
                        }
                    }
                    
                    if (missing > 5) {
                        printf(", ... (%d more)", missing - 5);
                    }
                    printf("\n");
                }
            }
        }
    }
    
    // Print summary
    printf("\n=== Final Summary ===\n");
    printf("Total Unsafe Lines: %d\n", total_unsafe);
    printf("Total Executed: %d\n", total_executed);
    int total_missing = total_unsafe - total_executed;
    printf("Total Missing: %d\n", total_missing);
    
    printf("Overall Coverage: %.2f%%\n", overall_coverage);
    
    // Print block stats
    int blocks = total_blocks.load();
    if (blocks > 0) {
        int instructions = total_instructions.load();
        printf("\n=== Block Statistics ===\n");
        printf("Total Unsafe Blocks: %d\n", blocks);
        printf("Total Unsafe Instructions: %d\n", instructions);
        printf("Avg Instructions Per Block: %.2f\n", 
               (float)instructions / blocks);
    }
    
    // Final status
    if (total_unsafe > 0) {
        if (total_unsafe == total_executed) {
            printf("\n  ✅ All unsafe lines executed!\n");
        } else {
            printf("\n  ❌ Missing %d unsafe lines (%.2f%% coverage)\n", 
                   total_missing, overall_coverage);
        }
    }
    
    pthread_mutex_unlock(&file_mutex);
}