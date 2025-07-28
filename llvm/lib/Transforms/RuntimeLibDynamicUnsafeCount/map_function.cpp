// g++ -fPIC -shared -o map_function.so map_function.cpp
// g++ -c -fPIC map_function.cpp -o map_function.o
// ar rcs libmap_function.a map_function.o

#include <iostream>
#include <mutex>
#include <unordered_map>
#include <string>
#include <cxxabi.h>
#include <fstream>
#include <sys/stat.h>
#include <errno.h>

// Shared data structures - initialized at program startup to avoid initialization races
std::unordered_map<std::string, bool> FunctionMap;
std::unordered_map<std::string, size_t> FunctionCallCount;
std::mutex MapMutex; // Guards the maps
std::mutex FileMutex; // Guards the file output

// Ensure output directory exists - created once at startup
namespace {
    struct DirectoryCreator {
        DirectoryCreator() {
            std::lock_guard<std::mutex> lock(FileMutex);
            if (mkdir("res", 0755) != 0 && errno != EEXIST) {
                perror("Error creating 'res' directory");
            }
        }
    };
    DirectoryCreator ensureDirectoryExists;
}

// Demangles function names
std::string demangle(const std::string &mangledName) {
    int status = 0;
    char *demangled = abi::__cxa_demangle(mangledName.c_str(), nullptr, nullptr, &status);
    if (status == 0) {
        std::string result(demangled);
        free(demangled);
        return result;
    } else {
        std::cerr << "Failed to demangle: " << mangledName
                  << " (status=" << status << ")\n";
        return mangledName;
    }
}

// Record function execution - now completely thread-safe
extern "C" void record_function_execution(const char *funcName, bool isUnsafe) {
    std::string FuncName(funcName);
    
    // Use a single lock scope for both map operations
    std::lock_guard<std::mutex> Guard(MapMutex);
    
    // Use insert with hint to avoid multiple lookups
    auto [it, inserted] = FunctionMap.try_emplace(FuncName, isUnsafe);
    
    // Increments call count (will create entry if not exists)
    FunctionCallCount[FuncName]++;
}

// Print stats 
extern "C" void print_execution_statistics() {
    // gather all data under a single lock
    std::unordered_map<std::string, bool> localFunctionMap;
    std::unordered_map<std::string, size_t> localCallCount;
    
    {
        std::lock_guard<std::mutex> Guard(MapMutex);
        localFunctionMap = FunctionMap;
        localCallCount = FunctionCallCount;
    }

    // To Calculate statistics
    size_t UnsafeFunctionCount = 0;
    size_t TotalFunctionCount = localFunctionMap.size();
    size_t UnsafeCallCount = 0;
    size_t TotalCallCount = 0;

    for (const auto& [name, isUnsafe] : localFunctionMap) {
        if (isUnsafe) UnsafeFunctionCount++;
    }

    for (const auto& [name, count] : localCallCount) {
        TotalCallCount += count;
        if (localFunctionMap.count(name) && localFunctionMap.at(name)) {
            UnsafeCallCount += count;
        }
    }

    // Writes to file with proper locking
    {
        std::lock_guard<std::mutex> Guard(FileMutex);
        FILE* file = fopen("res/unsafe_function_count.log", "a");
        if (file) {
            fprintf(file, "Unsafe Functions Executed: %zu / Total Functions Executed: %zu\n",
                    UnsafeFunctionCount, TotalFunctionCount - 1);
            fprintf(file, "Total times unsafe functions executed: %zu / Total times all functions executed: %zu\n",
                    UnsafeCallCount, TotalCallCount - 1);
            fclose(file);
        } else {
            perror("Error opening res/unsafe_function_count.log");
        }
    }
}