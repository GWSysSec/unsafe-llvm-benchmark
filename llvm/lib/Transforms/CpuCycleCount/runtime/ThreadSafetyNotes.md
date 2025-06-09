# Thread Safety Notes for CpuCycleCountRuntime

## Overview
The CpuCycleCountRuntime is designed to be thread-safe and handle concurrent access from multiple threads executing unsafe Rust code blocks simultaneously.

## Thread Safety Mechanisms

### 1. Atomic Variables
- `std::atomic<uint64_t> total_unsafe_cycles`: Accumulates total CPU cycles
- `std::atomic<uint64_t> total_unsafe_blocks`: Counts executed unsafe blocks  
- `std::atomic<int> runtime_initialized`: Ensures single initialization

### 2. Lock-Free Operations
- **Cycle measurement**: `cpu_cycle_start_measurement()` and `cpu_cycle_end_measurement()` use atomic operations
- **Counter updates**: All counter increments use `fetch_add()` for atomic updates
- **Status checks**: Initialization status checked with `load()`

### 3. Mutex Protection
- **Statistics reporting**: `pthread_mutex_t cycle_mutex` protects consistent state during reporting
- **File operations**: Protected during statistics file writing

## Performance Considerations

### RDTSCP Usage
- Uses `RDTSCP` instruction which is serializing (ensures instruction ordering)
- More accurate than `RDTSC` as it prevents out-of-order execution effects
- Includes aux register for CPU identification (not currently used)

### Measurement Overhead
- Minimal overhead: Two RDTSCP reads + one atomic add per unsafe block
- No heap allocation or complex data structures in hot path
- Lock-free measurement path for minimal contention

### Wraparound Handling
- Handles potential 64-bit counter wraparound (extremely unlikely)
- Calculation: `(end >= start) ? (end - start) : (UINT64_MAX - start + end + 1)`

## Correctness Guarantees

### Race Condition Prevention
- No data races on shared counters (atomic operations)
- No torn reads/writes on 64-bit values (atomic guarantees)
- Consistent initialization across threads

### Memory Ordering
- Default memory ordering (sequential consistency) for simplicity
- Could be optimized to relaxed ordering if needed for performance

## Integration Notes

### Compiler Integration
- Uses `__attribute__((constructor))` for automatic initialization
- Compatible with static linking into LLVM passes
- No C++ runtime dependencies beyond atomics and pthreads

### Error Handling
- Graceful degradation if initialization fails
- Safe early returns for invalid measurements
- File I/O errors reported but don't crash runtime

## Comparison with DynamicLineCount
- **Similarity**: Both use atomic counters and mutex for reporting
- **Difference**: CpuCycleCount focuses on timing vs. line coverage
- **Advantage**: Simpler data structures (just counters vs. bitmaps)