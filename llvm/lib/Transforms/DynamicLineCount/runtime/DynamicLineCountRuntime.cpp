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

// Simple data structures to avoid STL dependencies
#define MAX_FILES 1000
#define MAX_LINES_PER_FILE 10000

// File tracking structure
typedef struct {
    const char* filename;
    int unsafe_lines[MAX_LINES_PER_FILE];
    int unsafe_count;
    int exec_map[MAX_LINES_PER_FILE];
    int exec_count;
} FileStats;

// Global state
static FileStats files[MAX_FILES];
static int file_count = 0;
static int total_blocks = 0;
static int total_instructions = 0;
static int initialized = 0;

// Find or create file entry
static int find_file(const char* filename) {
    for (int i = 0; i < file_count; i++) {
        if (files[i].filename && strcmp(files[i].filename, filename) == 0) {
            return i;
        }
    }
    return -1;
}

static int create_file(const char* filename) {
    if (file_count >= MAX_FILES) {
        fprintf(stderr, "Warning: Too many files tracked, limit reached\n");
        return -1;
    }
    
    int idx = file_count++;
    files[idx].filename = strdup(filename);
    files[idx].unsafe_count = 0;
    files[idx].exec_count = 0;
    memset(files[idx].exec_map, 0, sizeof(files[idx].exec_map));
    return idx;
}

// Initialize at runtime
static void ensure_initialized(void) {
    if (!initialized) {
        // Register atexit handler
        atexit(print_coverage_stats);
        initialized = 1;
    }
}

// Export functions for runtime coverage tracking
RUNTIME_EXPORT void update_unsafe_line_counter(int64_t LineNum, const char* File) {
    if (!File) return;
    ensure_initialized();
    
    // Find or create file entry
    int idx = find_file(File);
    if (idx < 0) {
        idx = create_file(File);
    }
    if (idx < 0) return;
    
    // Add line if not already tracked
    FileStats* fs = &files[idx];
    for (int i = 0; i < fs->unsafe_count; i++) {
        if (fs->unsafe_lines[i] == LineNum) {
            return; // Already tracked
        }
    }
    
    // Add new unsafe line
    if (fs->unsafe_count < MAX_LINES_PER_FILE) {
        fs->unsafe_lines[fs->unsafe_count++] = LineNum;
    }
}

RUNTIME_EXPORT void mark_unsafe_line_executed(int64_t LineNum, const char* File) {
    if (!File) return;
    ensure_initialized();
    
    // Find or create file entry
    int idx = find_file(File);
    if (idx < 0) {
        idx = create_file(File);
    }
    if (idx < 0) return;
    
    // Mark line as executed
    FileStats* fs = &files[idx];
    if (LineNum < MAX_LINES_PER_FILE && fs->exec_map[LineNum] == 0) {
        fs->exec_map[LineNum] = 1;
        fs->exec_count++;
    }
}

RUNTIME_EXPORT void total_unsafe_block_count(int64_t BlockSize) {
    total_blocks++;
    total_instructions += BlockSize;
    ensure_initialized();
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
    size_t max_name_len = 10; // Minimum for "File" header
    
    for (int i = 0; i < file_count; i++) {
        if (files[i].filename) {
            size_t len = strlen(files[i].filename);
            if (len > max_name_len) max_name_len = len;
            
            // Count executed unsafe lines
            int executed = 0;
            for (int j = 0; j < files[i].unsafe_count; j++) {
                int line = files[i].unsafe_lines[j];
                if (line < MAX_LINES_PER_FILE && files[i].exec_map[line]) {
                    executed++;
                }
            }
            
            total_unsafe += files[i].unsafe_count;
            total_executed += executed;
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
            // Count executed unsafe lines
            int executed = 0;
            for (int j = 0; j < files[i].unsafe_count; j++) {
                int line = files[i].unsafe_lines[j];
                if (line < MAX_LINES_PER_FILE && files[i].exec_map[line]) {
                    executed++;
                }
            }
            
            int missing = files[i].unsafe_count - executed;
            float coverage = files[i].unsafe_count > 0 ? 
                (float)executed * 100 / files[i].unsafe_count : 0;
                
            printf("%-*s | %10d | %10d | %10d | %9.2f%%\n", 
                   (int)max_name_len, files[i].filename, 
                   files[i].unsafe_count, executed, missing, coverage);
            
            // Show missing lines (up to 5)
            if (missing > 0) {
                printf("   Missing lines: ");
                int count = 0;
                for (int j = 0; j < files[i].unsafe_count && count < 5; j++) {
                    int line = files[i].unsafe_lines[j];
                    if (line < MAX_LINES_PER_FILE && !files[i].exec_map[line]) {
                        printf("%d", line);
                        count++;
                        if (count < 5 && count < missing) {
                            printf(", ");
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
    
    // Free memory
    for (int i = 0; i < file_count; i++) {
        free((void*)files[i].filename);
    }
}