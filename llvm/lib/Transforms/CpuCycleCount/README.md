# CpuCycleCount Pass

A measurement pass that tracks CPU cycles consumed by unsafe Rust code blocks. This pass builds upon InstMarker's foundation to provide precise cycle-level performance analysis.

## Overview

CpuCycleCount uses RDTSCP (Read Time Stamp Counter and Processor ID) instructions to measure CPU cycles with high precision. It identifies unsafe code regions marked by InstMarker and instruments them for cycle measurement.

## Architecture

### Pass Component (CpuCycleCount.cpp)
- **Marker Detection**: Scans for `marker_begin`/`marker_end` inline assembly from InstMarker
- **Instrumentation**: Inserts `cpu_cycle_start_measurement()` and `cpu_cycle_end_measurement()` calls
- **Program Tracking**: Adds program-level cycle measurement to main function

### Runtime Component (CpuCycleCountRuntime.cpp/h)
- **Thread-Safe Counters**: Uses atomic variables for concurrent access
- **RDTSCP Measurement**: Serializing timestamp counter reads for accuracy
- **Statistics Generation**: Produces detailed performance reports

## Key Features

### Measurement Precision
- **RDTSCP**: Serializing instruction prevents out-of-order execution effects
- **Wraparound Handling**: Handles 64-bit counter overflow (extremely rare)
- **Thread Safety**: Lock-free measurement path, mutex-protected reporting

### Performance Tracking
- **Per-Block Cycles**: Individual unsafe block cycle consumption
- **Total Program Cycles**: Overall program execution time
- **Statistical Analysis**: Average cycles per block, percentage breakdown

### Output Formats
- **Console Report**: Real-time statistics at program exit
- **File Output**: `cpu_cycle.stat` with detailed measurements
- **Performance Insights**: Categorized performance analysis

## Integration

### Build Configuration
```cmake
# CMakeLists.txt includes runtime directly
add_llvm_component_library(LLVMCpuCycleCount
  CpuCycleCount.cpp
  runtime/CpuCycleCountRuntime.cpp
  STATIC
)
```

### Compiler Integration
```bash
# Enable CpuCycleCount
rustc --emit=llvm-ir \
  -C llvm-args=-enable-cpu-cycle-count \
  -C link-arg=-lLLVMCpuCycleCount
```

### Runtime Symbols
The runtime exports these symbols:
- `cpu_cycle_start_measurement()` - Begin cycle measurement
- `cpu_cycle_end_measurement(uint64_t)` - End measurement and accumulate
- `print_cpu_cycle_stats()` - Generate statistics report
- `cpu_cycle_program_start()` - Track total program cycles
- `cpu_cycle_program_end()` - Complete program measurement

## Dependencies

- **InstMarker**: Provides `marker_begin`/`marker_end` markers
- **X86 RDTSCP**: Requires x86_64 architecture with RDTSCP support
- **pthreads**: For mutex protection during reporting
- **C++ atomics**: For thread-safe counter operations

## Performance Considerations

### Measurement Overhead
- **Minimal**: Two RDTSCP reads + one atomic add per unsafe block
- **Lock-Free**: Hot path uses only atomic operations
- **No Allocation**: No heap allocation in measurement path

### Accuracy Factors
- **Includes**: CPU instruction execution, cache misses, memory access
- **Excludes**: OS interrupts, context switches, hypervisor overhead
- **Precision**: Nanosecond-level timing resolution on modern CPUs

## Thread Safety

### Atomic Operations
- `total_unsafe_cycles`: Accumulates cycle counts across threads
- `total_unsafe_blocks`: Counts executed unsafe blocks
- `runtime_initialized`: Ensures single initialization

### Memory Ordering
- **Sequential Consistency**: Default ordering for simplicity
- **Constructor Safety**: Automatic initialization via `__attribute__((constructor))`
- **Exit Safety**: Automatic reporting via `atexit()`

## Usage Examples

### Basic Measurement
```rust
// Rust code with unsafe blocks
unsafe {
    // This block will be measured
    *ptr = value;
}
```

## Troubleshooting

### Common Issues
1. **No runtime file**: Check InstMarker is running first
2. **Zero measurements**: Verify CARGO_PRIMARY_PACKAGE=1 if using Cargo
3. **Linking errors**: Ensure runtime library is properly linked

### Debug Information
The pass outputs debug information showing:
- Module processing status
- Number of instrumented blocks
- Pass execution confirmation