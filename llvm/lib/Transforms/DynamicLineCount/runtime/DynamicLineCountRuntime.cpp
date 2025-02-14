//===-- DynamicLineCountRuntime.cpp - Simplified Unsafe Coverage ----------===//

#include "DynamicLineCountRuntime.h"
#include <atomic>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace {

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

std::mutex GlobalMutex;
std::unordered_map<std::string, FileStats> GlobalStats;
std::atomic<bool> Initialized{false};

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

void ensureInitialized() {
    if (!Initialized.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> Lock(GlobalMutex);
        if (!Initialized.load(std::memory_order_relaxed)) {
            std::atexit(print_coverage_stats);
            Initialized.store(true, std::memory_order_release);
        }
    }
}

} // anonymous namespace

extern "C" {

RUNTIME_EXPORT void update_unsafe_line_counter(int64_t LineNum, const char *File) {
    if (!File) return;
    ensureInitialized();
    ThreadBuffer.LocalStats[File].UnsafeLines.insert(LineNum);
}

RUNTIME_EXPORT void mark_unsafe_line_executed(int64_t LineNum, const char *File) {
    if (!File) return;
    ensureInitialized();
    
    auto &Stats = ThreadBuffer.LocalStats[File];
    if (Stats.UnsafeLines.count(LineNum)) {
        Stats.UnsafeExecuted.insert(LineNum);
        Stats.ExecCount[LineNum]++;
    }
}

RUNTIME_EXPORT void print_coverage_stats(void) {
    ThreadBuffer.flush();
    
    std::lock_guard<std::mutex> Lock(GlobalMutex);
    size_t TotalUnsafe = 0, TotalExecuted = 0;
    bool AllExecuted = true;

    std::cout << "\n=== Unsafe Line Coverage Report ===\n";
    
    for (const auto &[File, Stats] : GlobalStats) {
        size_t FileUnsafe = Stats.UnsafeLines.size();
        size_t FileExecuted = Stats.UnsafeExecuted.size();
        
        TotalUnsafe += FileUnsafe;
        TotalExecuted += FileExecuted;
        
        if (FileUnsafe > 0) {
            bool Complete = (FileExecuted == FileUnsafe);
            AllExecuted &= Complete;
            
            std::cout << "\nFile: " << File 
                     << "\n  Unsafe Lines: " << FileUnsafe
                     << "\n  Executed: " << FileExecuted
                     << "\n  Coverage: " << (FileExecuted * 100.0 / FileUnsafe) << "%"
                     << (Complete ? "\n  ✅ All unsafe lines executed!" : "")
                     << "\n";

            std::cout << "  Line execution counts:\n";
            for (const auto &[Line, Count] : Stats.ExecCount) {
                std::cout << "    Line " << Line << ": " << Count << " times\n";
            }
        }
    }

    std::cout << "\n=== Final Summary ===\n"
              << "Total Unsafe Lines: " << TotalUnsafe << "\n"
              << "Total Executed: " << TotalExecuted << "\n"
              << "Overall Coverage: " 
              << (TotalUnsafe > 0 ? (TotalExecuted * 100.0) / TotalUnsafe : 0.0)
              << "%\n"
              << (AllExecuted && TotalUnsafe > 0 ? "\n🎉 All unsafe lines executed!\n" : "")
              << "==============================\n";
}

} // extern "C"
