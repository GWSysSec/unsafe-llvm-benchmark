#include "DynamicLineCountRuntime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <atomic>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

#define MAX_FILES 500
#define MAX_LINES_PER_FILE 65536
#define COVERAGE_OUTPUT_FILE "/tmp/unsafe_coverage.stat"

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

static FileStats files[MAX_FILES];
static std::atomic<int> file_count{0};
static std::atomic<int> total_blocks{0};
static std::atomic<int> total_instructions{0};
static std::atomic<bool> runtime_initialized{false};

static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

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

static void runtime_init(void) {
    runtime_initialized.store(true, std::memory_order_release);
    atexit(print_coverage_on_exit);
}

static void __attribute__((constructor)) ensure_init(void) {
    pthread_once(&init_once, runtime_init);
}

// Thread-safe line registration function
RUNTIME_EXPORT void update_unsafe_line_counter(int64_t LineNum, const char* File) {
    if (!File || LineNum <= 0 || LineNum >= MAX_LINES_PER_FILE) 
        return;
    
    int idx = find_file(File);
    if (idx < 0) {
        idx = create_file(File);
    }
    
    if (idx >= 0 && idx < MAX_FILES) {
        FileStats* fs = &files[idx];
        int bitmap_idx = LineNum / 32;
        int bit_offset = LineNum % 32;
        
        if (bitmap_idx < (MAX_LINES_PER_FILE/32)) {
            pthread_mutex_lock(&file_mutex);
            if (!(fs->unsafe_lines[bitmap_idx] & (1U << bit_offset))) {
                fs->unsafe_lines[bitmap_idx] |= (1U << bit_offset);
                fs->unsafe_count.fetch_add(1, std::memory_order_relaxed);
            }
            pthread_mutex_unlock(&file_mutex);
        }
    }
}

// Thread-safe line execution tracking function
RUNTIME_EXPORT void mark_unsafe_line_executed(int64_t LineNum, const char* File) {
    if (!File || LineNum <= 0 || LineNum >= MAX_LINES_PER_FILE) 
        return;
    
    if (!runtime_initialized.load(std::memory_order_acquire)) 
        return;
    
    int idx = find_file(File);
    if (idx < 0 || idx >= MAX_FILES)
        return;
    
    FileStats* fs = &files[idx];
    int bitmap_idx = LineNum / 32;
    int bit_offset = LineNum % 32;
    
    if (bitmap_idx < (MAX_LINES_PER_FILE/32)) {
        unsigned int unsafe_bits = fs->unsafe_lines[bitmap_idx];
        unsigned int exec_bits = fs->exec_map[bitmap_idx];
        
        if ((unsafe_bits & (1U << bit_offset)) && !(exec_bits & (1U << bit_offset))) {
            pthread_mutex_lock(&file_mutex);
            
            if ((fs->unsafe_lines[bitmap_idx] & (1U << bit_offset)) && 
                !(fs->exec_map[bitmap_idx] & (1U << bit_offset))) {
                
                fs->exec_map[bitmap_idx] |= (1U << bit_offset);
                fs->exec_count.fetch_add(1, std::memory_order_relaxed);
            }
            
            pthread_mutex_unlock(&file_mutex);
        }
    }
}

// Thread-safe block counting function
RUNTIME_EXPORT void total_unsafe_block_count(int64_t BlockSize) {
    total_blocks.fetch_add(1, std::memory_order_relaxed);
    total_instructions.fetch_add(BlockSize, std::memory_order_relaxed);
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
    pthread_mutex_lock(&file_mutex);
    
    int current_file_count = file_count.load(std::memory_order_acquire);
    if (current_file_count == 0) {
        pthread_mutex_unlock(&file_mutex);
        return;
    }
    
    int total_unsafe = 0;
    int total_executed = 0;
    
    for (int i = 0; i < current_file_count; i++) {
        if (files[i].filename) {
            int file_unsafe = files[i].unsafe_count.load(std::memory_order_acquire);
            int file_executed = files[i].exec_count.load(std::memory_order_acquire);
            
            total_unsafe += file_unsafe;
            total_executed += file_executed;
        }
    }
    
    float overall_coverage = total_unsafe > 0 ? 
        (float)total_executed * 100 / total_unsafe : 0;
    
    write_coverage_file(total_unsafe, total_executed, overall_coverage);
    
    if (total_unsafe > 0) {
        printf("[DynamicLineCount] %d/%d lines, %.2f%% coverage\n", 
               total_executed, total_unsafe, overall_coverage);
    }
    
    pthread_mutex_unlock(&file_mutex);
}