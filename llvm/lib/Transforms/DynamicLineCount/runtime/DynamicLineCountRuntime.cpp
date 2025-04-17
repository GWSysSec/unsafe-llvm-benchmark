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

// Simple data structures to avoid STL dependencies
#define MAX_FILES 500
#define MAX_LINES_PER_FILE 65536

// Forward declaration
static void print_coverage_on_exit(void);

// File tracking structure
typedef struct {
    const char* filename;           // Source file path
    unsigned int unsafe_lines[MAX_LINES_PER_FILE/32]; // Bitmap of unsafe lines
    unsigned int exec_map[MAX_LINES_PER_FILE/32];     // Bitmap of executed lines
    int unsafe_count;               // Total unsafe lines in file
    int exec_count;                 // Total executed unsafe lines
} FileStats;

// Global state 
static FileStats files[MAX_FILES];
static int file_count = 0;
static int total_blocks = 0;
static int total_instructions = 0;
static int runtime_initialized = 0;

// Simple find_file function
static int find_file(const char* filename) {
    if (!filename) return -1;
    
    for (int i = 0; i < file_count; i++) {
        if (files[i].filename && strcmp(files[i].filename, filename) == 0) {
            return i;
        }
    }
    return -1;
}

// Create file entry
static int create_file(const char* filename) {
    if (!filename || file_count >= MAX_FILES) {
        return -1;
    }
    
    int idx = file_count++;
    char* name_copy = strdup(filename);
    if (!name_copy) {
        return -1;
    }
    
    files[idx].filename = name_copy;
    files[idx].unsafe_count = 0;
    files[idx].exec_count = 0;
    memset(files[idx].unsafe_lines, 0, sizeof(files[idx].unsafe_lines));
    memset(files[idx].exec_map, 0, sizeof(files[idx].exec_map));
    return idx;
}

// No longer need ensure_initialized since we use constructor attribute

// One-time initialization at runtime, called through constructor attribute
static void __attribute__((constructor)) runtime_init(void) {
    static int registered = 0;
    if (!registered) {
        registered = 1;
        runtime_initialized = 1;
        atexit(print_coverage_on_exit);
    }
}

// Export functions for runtime coverage tracking
RUNTIME_EXPORT void update_unsafe_line_counter(int64_t LineNum, const char* File) {
    // No need to call ensure_initialized() in each function
    // The constructor attribute ensures initialization happens before first use
    
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
            if (!(fs->unsafe_lines[bitmap_idx] & (1U << bit_offset))) {
                fs->unsafe_lines[bitmap_idx] |= (1U << bit_offset);
                fs->unsafe_count++;
            }
        }
    }
}

RUNTIME_EXPORT void mark_unsafe_line_executed(int64_t LineNum, const char* File) {
    // No need to call ensure_initialized() in each function
    
    if (!File || LineNum <= 0 || LineNum >= MAX_LINES_PER_FILE) 
        return;
    
    // Find or create file entry
    int idx = find_file(File);
    if (idx < 0) {
        idx = create_file(File);
    }
    
    if (idx >= 0 && idx < MAX_FILES) {
        // Mark line as executed
        FileStats* fs = &files[idx];
        int bitmap_idx = LineNum / 32;
        int bit_offset = LineNum % 32;
        if (bitmap_idx < (MAX_LINES_PER_FILE/32)) {
            // Check if line is unsafe but not yet executed
            if ((fs->unsafe_lines[bitmap_idx] & (1U << bit_offset)) && 
                !(fs->exec_map[bitmap_idx] & (1U << bit_offset))) {
                // Mark as executed
                fs->exec_map[bitmap_idx] |= (1U << bit_offset);
                fs->exec_count++;
            }
        }
    }
}

RUNTIME_EXPORT void total_unsafe_block_count(int64_t BlockSize) {
    // No need to call ensure_initialized() in each function
    
    // Simple counters
    total_blocks++;
    total_instructions += BlockSize;
}

// This wrapper function is registered with atexit
static void print_coverage_on_exit(void) {
    print_coverage_stats();
}

RUNTIME_EXPORT void print_coverage_stats(void) {
    if (file_count == 0) {
        printf("No unsafe code was instrumented or executed.\n");
        return;
    }
    
    printf("\n=== Unsafe Code Coverage Report ===\n\n");
    
    // Calculate totals and max filename length
    int total_unsafe = 0;
    int total_executed = 0;
    size_t max_name_len = 20; // Minimum for "File" header
    
    for (int i = 0; i < file_count; i++) {
        if (files[i].filename) {
            size_t len = strlen(files[i].filename);
            if (len > max_name_len) max_name_len = len;
            
            total_unsafe += files[i].unsafe_count;
            total_executed += files[i].exec_count;
        }
    }
    
    // Print table header
    printf("%-*s | %10s | %10s | %10s | %10s\n", 
           (int)max_name_len, "File", "Unsafe", "Executed", "Missing", "Coverage");
    
    // Print separator
    for (size_t i = 0; i < max_name_len + 48; i++) {
        printf("-");
    }
    printf("\n");
    
    // Print file data
    for (int i = 0; i < file_count; i++) {
        if (files[i].filename && files[i].unsafe_count > 0) {
            int missing = files[i].unsafe_count - files[i].exec_count;
            float coverage = files[i].unsafe_count > 0 ? 
                (float)files[i].exec_count * 100 / files[i].unsafe_count : 0;
                
            printf("%-*s | %10d | %10d | %10d | %9.2f%%\n", 
                   (int)max_name_len, files[i].filename, 
                   files[i].unsafe_count, files[i].exec_count, missing, coverage);
            
            // Show missing lines (up to 5)
            if (missing > 0) {
                printf("   Missing lines: ");
                int count = 0;
                int missing_printed = 0;
                
                for (int line = 1; line < MAX_LINES_PER_FILE && missing_printed < 5; line++) {
                    int bitmap_idx = line / 32;
                    int bit_offset = line % 32;
                    if (bitmap_idx < (MAX_LINES_PER_FILE/32)) {
                        if ((files[i].unsafe_lines[bitmap_idx] & (1U << bit_offset)) && 
                            !(files[i].exec_map[bitmap_idx] & (1U << bit_offset))) {
                            printf("%d", line);
                            missing_printed++;
                            count++;
                            if (missing_printed < 5 && count < missing) {
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
    
    // Print summary
    printf("\n=== Final Summary ===\n");
    printf("Total Unsafe Lines: %d\n", total_unsafe);
    printf("Total Executed: %d\n", total_executed);
    int total_missing = total_unsafe - total_executed;
    printf("Total Missing: %d\n", total_missing);
    
    float overall_coverage = total_unsafe > 0 ? 
        (float)total_executed * 100 / total_unsafe : 0;
    printf("Overall Coverage: %.2f%%\n", overall_coverage);
    
    // Print block stats
    if (total_blocks > 0) {
        printf("\n=== Block Statistics ===\n");
        printf("Total Unsafe Blocks: %d\n", total_blocks);
        printf("Total Unsafe Instructions: %d\n", total_instructions);
        printf("Avg Instructions Per Block: %.2f\n", 
               (float)total_instructions / total_blocks);
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
}