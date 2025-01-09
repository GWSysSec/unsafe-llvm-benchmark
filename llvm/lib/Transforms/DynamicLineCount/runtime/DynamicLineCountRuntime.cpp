//===-- DynamicLineCountRuntime.cpp - Runtime Implementation --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DynamicLineCountRuntime.h"
#include <atomic>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <memory>
#include <thread>

extern "C" {
RUNTIME_EXPORT void update_line_counter(int64_t LineNum, const char *File);
RUNTIME_EXPORT void update_unsafe_line_counter(int64_t LineNum, const char *File);
RUNTIME_EXPORT void mark_line_executed(int64_t LineNum, const char *File);
RUNTIME_EXPORT void mark_unsafe_line_executed(int64_t LineNum, const char *File);
RUNTIME_EXPORT void print_coverage_stats(void);
}

struct LineIdentifier {
  std::string File;
  int64_t Line;

  bool operator==(const LineIdentifier &Other) const {
    return Line == Other.Line && File == Other.File;
  }
};

namespace std {
template <>
struct hash<LineIdentifier> {
  size_t operator()(const LineIdentifier &Id) const {
    return std::hash<std::string>()(Id.File) ^ std::hash<int64_t>()(Id.Line);
  }
};
} // end namespace std

namespace {
struct FileStats {
  std::unordered_set<int64_t> TotalLines;
  std::unordered_set<int64_t> UnsafeLines;
  std::unordered_set<int64_t> ExecutedLines;
  std::unordered_set<int64_t> UnsafeExecuted;
  std::unordered_map<int64_t, size_t> ExecCount;

  void merge(const FileStats &Other) {
    TotalLines.insert(Other.TotalLines.begin(), Other.TotalLines.end());
    UnsafeLines.insert(Other.UnsafeLines.begin(), Other.UnsafeLines.end());
    ExecutedLines.insert(Other.ExecutedLines.begin(), Other.ExecutedLines.end());
    UnsafeExecuted.insert(Other.UnsafeExecuted.begin(), Other.UnsafeExecuted.end());
    
    for (const auto &[Line, Count] : Other.ExecCount)
      ExecCount[Line] += Count;
  }
};

using StatsMap = std::unordered_map<std::string, FileStats>;

class ThreadLocalBuffer {
public:
  StatsMap LocalStats;
  size_t UpdateCount{0};
  static constexpr size_t FlushThreshold = RUNTIME_BUFFER_SIZE;

  ~ThreadLocalBuffer() {
    flushToGlobal(true);
  }

  void incrementAndMaybeFlush() {
    if (++UpdateCount >= FlushThreshold)
      flushToGlobal(false);
  }

  void flushToGlobal(bool FinalFlush);
};

std::unique_ptr<StatsMap> GlobalStats;
std::mutex GlobalMutex;
std::atomic<bool> Initialized{false};

thread_local std::unique_ptr<ThreadLocalBuffer> ThreadBuffer;

void ensureInitialized() {
  if (!Initialized.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> Lock(GlobalMutex);
    if (!Initialized.load(std::memory_order_relaxed)) {
      GlobalStats = std::make_unique<StatsMap>();
      Initialized.store(true, std::memory_order_release);
      std::atexit([]() {
        if (ThreadBuffer)
          ThreadBuffer->flushToGlobal(true);
        print_coverage_stats();
      });
    }
  }
}

void ThreadLocalBuffer::flushToGlobal(bool FinalFlush) {
  if (LocalStats.empty())
    return;

  std::lock_guard<std::mutex> Lock(GlobalMutex);
  for (const auto &[Filename, Stats] : LocalStats)
    (*GlobalStats)[Filename].merge(Stats);

  if (FinalFlush) {
    LocalStats.clear();
  } else {
    for (auto &[Filename, Stats] : LocalStats)
      Stats = FileStats();
  }
  UpdateCount = 0;
}

ThreadLocalBuffer &getThreadBuffer() {
  if (!ThreadBuffer)
    ThreadBuffer = std::make_unique<ThreadLocalBuffer>();
  return *ThreadBuffer;
}

} // anonymous namespace

extern "C" {

RUNTIME_EXPORT void update_line_counter(int64_t LineNum, const char *File) {
    ensureInitialized();
    if (!File)
        return;

    auto &Buffer = getThreadBuffer();
    Buffer.LocalStats[std::string(File)].TotalLines.insert(LineNum);
    Buffer.incrementAndMaybeFlush();
}

RUNTIME_EXPORT void update_unsafe_line_counter(int64_t LineNum, const char *File) {
    ensureInitialized();
    if (!File)
        return;

    auto &Buffer = getThreadBuffer();
    auto &Stats = Buffer.LocalStats[std::string(File)];
    Stats.TotalLines.insert(LineNum);
    Stats.UnsafeLines.insert(LineNum);
    Buffer.incrementAndMaybeFlush();
}

RUNTIME_EXPORT void mark_line_executed(int64_t LineNum, const char *File) {
    ensureInitialized();
    if (!File)
        return;

    auto &Buffer = getThreadBuffer();
    auto &Stats = Buffer.LocalStats[std::string(File)];
    
    if (Stats.TotalLines.count(LineNum) > 0) {
        Stats.ExecutedLines.insert(LineNum);
        Stats.ExecCount[LineNum]++;
    }
    Buffer.incrementAndMaybeFlush();
}

RUNTIME_EXPORT void mark_unsafe_line_executed(int64_t LineNum, const char *File) {
    ensureInitialized();
    if (!File)
        return;

    auto &Buffer = getThreadBuffer();
    auto &Stats = Buffer.LocalStats[std::string(File)];
    
    if (Stats.UnsafeLines.count(LineNum) > 0) {
        Stats.ExecutedLines.insert(LineNum);
        Stats.UnsafeExecuted.insert(LineNum);
        Stats.ExecCount[LineNum]++;
    }
    Buffer.incrementAndMaybeFlush();
}

RUNTIME_EXPORT void print_coverage_stats(void) {
    ensureInitialized();
    
    if (ThreadBuffer)
        ThreadBuffer->flushToGlobal(true);

  std::lock_guard<std::mutex> Lock(GlobalMutex);
  
  std::unordered_set<LineIdentifier> GlobalLines;
  std::unordered_set<LineIdentifier> GlobalUnsafe;
  std::unordered_set<LineIdentifier> GlobalExecuted;
  std::unordered_set<LineIdentifier> GlobalUnsafeExecuted;

  for (const auto &[Filename, Stats] : *GlobalStats) {
    for (auto Line : Stats.TotalLines)
      GlobalLines.insert(LineIdentifier{Filename, Line});
    for (auto Line : Stats.UnsafeLines)
      GlobalUnsafe.insert(LineIdentifier{Filename, Line});
    for (auto Line : Stats.ExecutedLines)
      GlobalExecuted.insert(LineIdentifier{Filename, Line});
    for (auto Line : Stats.UnsafeExecuted)
      GlobalUnsafeExecuted.insert(LineIdentifier{Filename, Line});
  }

  size_t TotalSourceLines = GlobalLines.size();
  size_t TotalUnsafeLines = GlobalUnsafe.size();
  size_t TotalExecutedLines = GlobalExecuted.size();
  size_t TotalUnsafeExecuted = GlobalUnsafeExecuted.size();

  std::cout << "\n=== Source Line Coverage Report ===\n"
            << "Total Source Lines: " << TotalSourceLines << "\n"
            << "Total Executed Lines: " << TotalExecutedLines << "\n"
            << "Overall Coverage: " 
            << (TotalSourceLines > 0 ? (TotalExecutedLines * 100.0) / TotalSourceLines : 0.0)
            << "%\n\n"
            << "Total Unsafe Lines: " << TotalUnsafeLines << "\n"
            << "Executed Unsafe Lines: " << TotalUnsafeExecuted << "\n"
            << "Unsafe Code Coverage: " 
            << (TotalUnsafeLines > 0 ? (TotalUnsafeExecuted * 100.0) / TotalUnsafeLines : 0.0)
            << "%\n"
            << "==============================\n\n"
            << "Per-File Coverage:\n";

  for (const auto &[Filename, Stats] : *GlobalStats) {
    size_t FileTotal = Stats.TotalLines.size();
    if (FileTotal == 0)
      continue;

    size_t FileUnsafe = Stats.UnsafeLines.size();
    size_t FileExecuted = Stats.ExecutedLines.size();
    size_t FileUnsafeExecuted = Stats.UnsafeExecuted.size();

    std::cout << "\nFile: " << Filename << "\n"
              << "  Total Lines: " << FileTotal << "\n"
              << "  Executed Lines: " << FileExecuted << "\n"
              << "  Coverage: "
              << (FileExecuted * 100.0) / FileTotal << "%\n";

    if (FileUnsafe > 0) {
      std::cout << "  Unsafe Lines: " << FileUnsafe << "\n"
                << "  Executed Unsafe Lines: " << FileUnsafeExecuted << "\n"
                << "  Unsafe Coverage: "
                << (FileUnsafeExecuted * 100.0) / FileUnsafe << "%\n";
    }
  }
  std::cout << std::endl;
}

} // extern "C"
