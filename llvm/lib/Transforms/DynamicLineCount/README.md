# DynamicLineCount Pass

A line-level coverage analysis pass that tracks execution of unsafe Rust code at the source line granularity. This pass provides detailed coverage statistics showing which unsafe lines were executed during program runtime.

## Overview

DynamicLineCount instruments unsafe Rust code to provide comprehensive line-level coverage analysis. It operates independently of other analysis passes while building upon InstMarker's unsafe instruction identification.

## Architecture

### Pass Component (DynamicLineCount.cpp)
- **Line Registration**: Registers all unsafe lines at program startup via module constructors
- **Execution Tracking**: Instruments each unsafe instruction with runtime calls
- **Primary Package Filtering**: Respects `CARGO_PRIMARY_PACKAGE=1` environment variable

### Runtime Component (DynamicLineCountRuntime.cpp/h)
- **Thread-Safe Coverage**: Uses atomic counters and mutex protection for concurrent access
- **Bitmap Tracking**: Efficient line tracking using bitmaps for memory optimization
- **File-Based Organization**: Tracks coverage statistics per source file
- **Automatic Reporting**: Generates coverage reports at program exit

## Key Features

### Line-Level Precision
- **Individual Line Tracking**: Each unsafe source line is tracked independently
- **Bitmap Storage**: Memory-efficient storage using bit arrays for line status
- **Duplicate Prevention**: Ensures each line is registered only once per file

### Coverage Analysis
- **Per-File Statistics**: Coverage breakdown by source file
- **Missing Line Detection**: Identifies specific lines that weren't executed
- **Percentage Calculations**: Provides coverage percentages at file and program level

### Thread Safety
- **Atomic Operations**: Lock-free counters for performance-critical paths
- **Mutex Protection**: Thread-safe file operations and reporting
- **Race Condition Prevention**: Careful synchronization during bitmap updates

## Runtime File Format

The `/tmp/unsafe_coverage.stat` file contains:

```
===== Unsafe Code Coverage Statistics =====
Total Unsafe Lines: 42
Executed Unsafe Lines: 35
Missing Unsafe Lines: 7
Coverage Percentage: 83.33%
Total Unsafe Blocks: 15
Total Unsafe Instructions: 58
Avg Instructions Per Block: 3.87
```

## Console Output

Real-time coverage report at program exit:

```
=== Unsafe Code Coverage Report ===

File                          |     Unsafe |   Executed |    Missing |   Coverage
------------------------------------------------------------------------------
src/main.rs                   |         25 |         20 |          5 |     80.00%
   Missing lines: 42, 58, 73, 89, 105
src/utils.rs                  |         17 |         15 |          2 |     88.24%
   Missing lines: 23, 67

=== Final Summary ===
Total Unsafe Lines: 42
Total Executed: 35
Total Missing: 7
Overall Coverage: 83.33%

  ❌ Missing 7 unsafe lines (83.33% coverage)
```

## Integration

### Build Configuration
```cmake
# CMakeLists.txt includes runtime directly
add_llvm_component_library(LLVMDynamicLineCount
  DynamicLineCount.cpp
  runtime/DynamicLineCountRuntime.cpp
  STATIC
)
```

### Compiler Integration
```bash
# Enable DynamicLineCount
rustc --emit=llvm-ir \
  -C llvm-args=-enable-dynamic-line-count \
  -C link-arg=-lLLVMDynamicLineCount
```

### Runtime Symbols
The runtime exports these symbols:
- `update_unsafe_line_counter(line, file)` - Register unsafe line at startup
- `mark_unsafe_line_executed(line, file)` - Mark line as executed
- `total_unsafe_block_count(size)` - Legacy block counting (for compatibility)
- `print_coverage_stats()` - Generate coverage report

## Dependencies

- **InstMarker**: Provides unsafe instruction identification
- **pthreads**: For thread-safe operations and mutex protection
- **C++ atomics**: For lock-free counter operations
- **Debug Information**: Requires debug metadata for source line mapping

## Implementation Details

### Two-Phase Approach
1. **Registration Phase**: Module constructors register all unsafe lines at program startup
2. **Execution Phase**: Runtime calls track which lines are actually executed

### Memory Management
- **Fixed-Size Arrays**: Avoids dynamic allocation for better performance
- **Bitmap Compression**: Efficient storage of line status (32 lines per word)
- **String Deduplication**: Reuses global strings for common file names

### Performance Optimizations
- **Lock-Free Hot Path**: Execution tracking uses atomic operations only
- **Lazy Initialization**: Constructor-based initialization with single-shot semantics
- **Efficient Bitmap Scanning**: Optimized algorithms for missing line detection

## Thread Safety Design

### Atomic Variables
- `file_count`: Number of tracked files
- `total_blocks`: Count of unsafe blocks (legacy compatibility)
- `total_instructions`: Count of unsafe instructions (legacy compatibility)
- `runtime_initialized`: Ensures single initialization

### Mutex Protection
- **File Operations**: Creating new file entries and bitmap updates
- **Reporting**: Consistent state during coverage report generation
- **Registration**: Thread-safe line registration during startup

## Usage Examples

### Basic Coverage Analysis
```rust
// This unsafe block will be tracked
unsafe {
    *ptr = value;  // Line tracked individually
    func();        // Each line counted separately
}

// Another unsafe block
unsafe {
    another_operation();  // Independent line tracking
}
```

### Coverage Filtering
```bash
# Only track primary package (recommended for Cargo projects)
CARGO_PRIMARY_PACKAGE=1 cargo run
```

## Troubleshooting

### Common Issues
1. **No coverage file**: Check that unsafe code exists and InstMarker is enabled
2. **Missing lines**: Verify debug information is available (`-g` flag)
3. **Zero coverage**: Ensure `CARGO_PRIMARY_PACKAGE=1` for Cargo projects
4. **Linking errors**: Verify runtime library is properly linked

### Debug Information
The pass outputs debug information when processing functions with many unsafe lines (>10), helping identify instrumentation activity.

## Comparison with Other Passes

### vs. CpuCycleCount
- **DynamicLineCount**: Focuses on coverage analysis, tracks individual lines
- **CpuCycleCount**: Focuses on performance measurement, uses block boundaries

### vs. HeapTracker
- **DynamicLineCount**: Line-level execution tracking
- **HeapTracker**: Memory operation analysis within unsafe regions

### vs. InstMarker
- **DynamicLineCount**: Consumer of InstMarker's unsafe analysis
- **InstMarker**: Foundation layer providing unsafe instruction identification

## Architecture Benefits

- **Independent Operation**: No dependency on other analysis passes' runtime functions
- **Precise Tracking**: Line-level granularity for detailed coverage analysis
- **Efficient Storage**: Bitmap-based approach minimizes memory overhead
- **Thread-Safe Design**: Supports concurrent execution without data races
- **Comprehensive Reporting**: Detailed statistics with missing line identification