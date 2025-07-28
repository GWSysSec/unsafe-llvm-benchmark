// update_counter.c
// gcc -c -fPIC update_counter.c -o update_counter.o
// ar rcs libupdate_counter.a update_counter.o
//ar rcs libmap_function.a map_function.o

#include <stdio.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DIST_SIZE 1024  // Maximum size of unsafe blocks to track distribution

// Atomic counters for total stats
static atomic_long total_instruction_count = 0;
static atomic_long instruction_count = 0;
static atomic_long unsafe_load_count = 0;
static atomic_long unsafe_store_count = 0;
static atomic_long unsafe_add_count = 0;
static atomic_long unsafe_getelementptr_count = 0;
static atomic_long unsafe_sub_count = 0;
static atomic_long unsafe_alloca_count = 0;
static atomic_long unsafe_ptrtoint_instructions = 0;
static atomic_long unsafe_inttoptr_instructions = 0;
static atomic_long unsafe_bitcast_instructions = 0;
static atomic_long unsafe_other = 0;

// Atomic map: index = instruction count in block, value = frequency
static atomic_long unsafe_block_dist[MAX_DIST_SIZE];

// Mutex to serialize logging and directory creation
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// Called from LLVM pass to update counters
void update_counter(long total_inst, long count, long load_count, long store_count,
                    long add_count, long getelementptr_count, long sub_count,
                    long alloca_count, long ptrtoint_instructions,
                    long inttoptr_instructions, long bitcast_instructions,
                    long other) {
    atomic_fetch_add(&total_instruction_count, total_inst);
    atomic_fetch_add(&instruction_count, count);

    atomic_fetch_add(&unsafe_load_count, load_count);
    atomic_fetch_add(&unsafe_store_count, store_count);
    atomic_fetch_add(&unsafe_add_count, add_count);
    atomic_fetch_add(&unsafe_getelementptr_count, getelementptr_count);
    atomic_fetch_add(&unsafe_sub_count, sub_count);
    atomic_fetch_add(&unsafe_alloca_count, alloca_count);
    atomic_fetch_add(&unsafe_ptrtoint_instructions, ptrtoint_instructions);
    atomic_fetch_add(&unsafe_inttoptr_instructions, inttoptr_instructions);
    atomic_fetch_add(&unsafe_bitcast_instructions, bitcast_instructions);
    atomic_fetch_add(&unsafe_other, other);

    if (count >= 0 && count < MAX_DIST_SIZE) {
        atomic_fetch_add(&unsafe_block_dist[count], 1);
    }
}

// Called at end of main() to print all counts
void print_inline_count() {
    pthread_mutex_lock(&log_mutex);

    if (mkdir("res", 0755) != 0 && errno != EEXIST) {
        perror("Error creating 'res' directory");
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    // Write instruction statistics
    FILE *file = fopen("res/unsafe_instruction_count.log", "a");
    if (file) {
        fprintf(file, "Total Instructions Executed: %ld\n", atomic_load(&total_instruction_count));
        fprintf(file, "Total Unsafe Instructions Executed Inside Markers: %ld\n", atomic_load(&instruction_count));

        fprintf(file, "Load: %ld\n", atomic_load(&unsafe_load_count));
        fprintf(file, "Store: %ld\n", atomic_load(&unsafe_store_count));
        fprintf(file, "Add: %ld\n", atomic_load(&unsafe_add_count));
        fprintf(file, "GetElementPtr: %ld\n", atomic_load(&unsafe_getelementptr_count));
        fprintf(file, "Sub: %ld\n", atomic_load(&unsafe_sub_count));
        fprintf(file, "Alloca: %ld\n", atomic_load(&unsafe_alloca_count));
        fprintf(file, "PtrtoInt: %ld\n", atomic_load(&unsafe_ptrtoint_instructions));
        fprintf(file, "InttoPtr: %ld\n", atomic_load(&unsafe_inttoptr_instructions));
        fprintf(file, "Bitcast: %ld\n", atomic_load(&unsafe_bitcast_instructions));
        fprintf(file, "Other: %ld\n", atomic_load(&unsafe_other));
        fclose(file);
    } else {
        perror("Error opening unsafe_instruction_count.log");
    }

    // Write block distribution statistics
    FILE *dist_file = fopen("res/unsafe_block_distribution.log", "a");
    if (dist_file) {
        fprintf(dist_file, "Unsafe Block Size Distribution (UnsafeInstCount : Occurrences):\n");
        for (int i = 0; i < MAX_DIST_SIZE; ++i) {
            long v = atomic_load(&unsafe_block_dist[i]);
            if (v > 0) {
                fprintf(dist_file, "%d : %ld\n", i, v);
            }
        }
        fclose(dist_file);
    } else {
        perror("Error opening unsafe_block_distribution.log");
    }

    pthread_mutex_unlock(&log_mutex);
}
