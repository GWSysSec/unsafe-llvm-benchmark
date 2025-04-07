//===-- DynamicLineCountRuntime.cpp - Unified Unsafe Coverage -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DynamicLineCountRuntime.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <cstdio>
#include <vector>
#include <algorithm> 

namespace {

// Thread-local guard to prevent recursion
thread_local int g_instr_guard = 0;

// Statistics structures
struct FileStats {
    std::unordered_set<int64_t> UnsafeLines;
    std::unordered_set<int64_t> UnsafeExecuted;
    std::unordered_map<int64_t, size_t> ExecCount;

    void merge(const FileStats &Other) {
        UnsafeLines.insert(Other.UnsafeLines.begin(), Other.UnsafeLines.end());
        UnsafeExecuted.insert(Other.UnsafeExecuted.begin(), Other.UnsafeExecuted.end());
        for (const auto &[Line, Count] : Other.ExecCount) {
            ExecCount[Line] += Count;
        }
    }
};

// Global state
std::mutex GlobalMutex;
std::unordered_map<std::string, FileStats> GlobalStats;
std::atomic<bool> Initialized{false};
std::atomic<int> TotalInstrCalls{0};
std::atomic<int> TotalUnsafeBlocks{0};
std::atomic<int64_t> TotalUnsafeInstructions{0};

// Thread-local buffer for performance
thread_local struct ThreadLocal {
    std::unordered_map<std::string, FileStats> LocalStats;
    
    void flush() {
        std::lock_guard<std::mutex> Lock(GlobalMutex);
        for (auto& [File, Stats] : LocalStats) {
            GlobalStats[File].merge(Stats);
        }
        LocalStats.clear();
    }
    
    ~ThreadLocal() { flush(); }
} ThreadBuffer;

// Initialization check
void ensureInitialized() {
    if (!Initialized.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> Lock(GlobalMutex);
        if (!Initialized.load(std::memory_order_relaxed)) {
            std::atexit(print_coverage_stats);
            Initialized.store(true, std::memory_order_release);
        }
    }
}

// Function that definitely won't be optimized away
__attribute__((noinline, used)) 
void enforce_side_effect(int64_t line) {
    g_instr_guard += line;
    // Compiler barrier
    asm volatile("" ::: "memory");
}

} // anonymous namespace

extern "C" {

// Update unsafe line counter
RUNTIME_EXPORT void update_unsafe_line_counter(int64_t LineNum, const char *File) {
    if (!File) return;
    
    TotalInstrCalls.fetch_add(1, std::memory_order_relaxed);
    enforce_side_effect(LineNum);
    ensureInitialized();
    ThreadBuffer.LocalStats[File].UnsafeLines.insert(LineNum);
}

// Mark unsafe line as executed
RUNTIME_EXPORT void mark_unsafe_line_executed(int64_t LineNum, const char *File) {
    if (!File) return;
    
    TotalInstrCalls.fetch_add(1, std::memory_order_relaxed);
    enforce_side_effect(LineNum);
    ensureInitialized();
    
    auto &Stats = ThreadBuffer.LocalStats[File];
    Stats.UnsafeLines.insert(LineNum); // Ensure the line is in UnsafeLines even if update wasn't called
    Stats.UnsafeExecuted.insert(LineNum);
    Stats.ExecCount[LineNum]++;
}

// Track total count of unsafe blocks (used by InstMarker)
RUNTIME_EXPORT void total_unsafe_block_count(int64_t BlockSize) {
    TotalUnsafeBlocks.fetch_add(1, std::memory_order_relaxed);
    TotalUnsafeInstructions.fetch_add(BlockSize, std::memory_order_relaxed);
    enforce_side_effect(BlockSize);
    ensureInitialized();
}

// Print coverage statistics at program exit
RUNTIME_EXPORT void print_coverage_stats(void) {
    ThreadBuffer.flush();
    
    std::lock_guard<std::mutex> Lock(GlobalMutex);

    if (GlobalStats.empty()) {
        printf("No unsafe lines were instrumented or executed.\n");
        return;
    }
    
    printf("\n=== Unsafe Code Coverage Report ===\n\n");
    
    // Calculate file-level statistics
    size_t TotalUnsafe = 0, TotalExecuted = 0;
    size_t MaxFileNameLen = 0;
    
    // Get max filename length for formatting
    for (const auto &[File, _] : GlobalStats) {
        MaxFileNameLen = std::max(MaxFileNameLen, File.length());
    }
    
    // First print files with unsafe code
    printf("%-*s | %10s | %10s | %10s\n", 
           static_cast<int>(MaxFileNameLen), "File", "Unsafe", "Executed", "Coverage");
    printf("%s\n", std::string(MaxFileNameLen + 36, '-').c_str());
    
    std::vector<std::pair<std::string, FileStats>> SortedStats(
        GlobalStats.begin(), GlobalStats.end());
    
    // Sort by coverage percentage (descending)
    std::sort(SortedStats.begin(), SortedStats.end(), 
        [](const auto &a, const auto &b) {
            double aCoverage = a.second.UnsafeLines.empty() ? 0.0 : 
                static_cast<double>(a.second.UnsafeExecuted.size()) / a.second.UnsafeLines.size();
            double bCoverage = b.second.UnsafeLines.empty() ? 0.0 : 
                static_cast<double>(b.second.UnsafeExecuted.size()) / b.second.UnsafeLines.size();
            return aCoverage > bCoverage;
        });
    
    for (const auto &[File, Stats] : SortedStats) {
        size_t FileUnsafe = Stats.UnsafeLines.size();
        size_t FileExecuted = Stats.UnsafeExecuted.size();
        
        TotalUnsafe += FileUnsafe;
        TotalExecuted += FileExecuted;
        
        if (FileUnsafe > 0) {
            double Coverage = static_cast<double>(FileExecuted) * 100.0 / FileUnsafe;
            printf("%-*s | %10zu | %10zu | %9.2f%%\n", 
                   static_cast<int>(MaxFileNameLen), File.c_str(), 
                   FileUnsafe, FileExecuted, Coverage);
        }
    }
    
    // Print summary
    printf("\n=== Final Summary ===\n");
    printf("Total Unsafe Lines: %zu\n", TotalUnsafe);
    printf("Total Executed: %zu\n", TotalExecuted);
    double OverallCoverage = TotalUnsafe > 0 ? 
        (static_cast<double>(TotalExecuted) * 100.0) / TotalUnsafe : 0.0;
    printf("Overall Coverage: %.2f%%\n", OverallCoverage);
    
    // Print unsafe block stats if available
    int UnsafeBlocks = TotalUnsafeBlocks.load(std::memory_order_relaxed);
    if (UnsafeBlocks > 0) {
        printf("Total Unsafe Blocks: %d\n", UnsafeBlocks);
        printf("Total Unsafe Instructions: %ld\n", 
               TotalUnsafeInstructions.load(std::memory_order_relaxed));
        printf("Avg Instructions Per Block: %.2f\n", 
               static_cast<double>(TotalUnsafeInstructions.load(std::memory_order_relaxed)) / UnsafeBlocks);
    }
    
    if (TotalUnsafe > 0 && TotalUnsafe == TotalExecuted) {
        printf("\n  ✅ All unsafe lines executed!");
    }
}

} // extern "C"
