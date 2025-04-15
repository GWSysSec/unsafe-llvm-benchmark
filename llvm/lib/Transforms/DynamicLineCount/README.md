# DynamicLineCount Pass for Rust

An LLVM pass for analyzing unsafe code patterns in Rust programs by tracking line-level execution.

## Detection Logic

The pass works by instrumenting source locations during compilation. It relies on the InstMarker pass to identify unsafe instructions with the `unsafe_inst` metadata. Debug locations help map instructions to source lines in the code. The pass automatically skips compiler-generated instructions or those from standard libraries.

## Counting Logic

For each unique source location, the pass inserts two types of instrumentation:

1. **Registration**: During module initialization, all unsafe lines are registered with the runtime
2. **Execution Tracking**: When unsafe code executes, the corresponding line is marked as executed

These counters track:

- Total number of unsafe lines per source file
- Which unsafe lines were actually executed at runtime
- Block-level statistics about unsafe code regions

## Runtime Implementation

The lightweight runtime library handles coverage tracking with minimal overhead:

1. Uses bitmap-based storage for efficient memory usage (supports up to 65536 lines per file)
2. Counts both total unsafe lines and executed unsafe lines
3. Generates a detailed coverage report at program termination

The runtime exposes these APIs:
- `update_unsafe_line_counter`: Register an unsafe line in the coverage database
- `mark_unsafe_line_executed`: Record that an unsafe line was executed
- `total_unsafe_block_count`: Track statistics about unsafe code blocks
- `print_coverage_stats`: Generate the coverage report (called automatically at exit)

## Integration with Rust

### Integration with Rust

To use this pass with Rust, add the following to your `.cargo/config.toml`:

```toml
[target.x86_64-unknown-linux-gnu]
rustflags = [
    "-C", "passes=unsafe-analysis-pass,instmarker,dynamic-line-count", 
    "-C", "link-arg=-L/path/to/llvm/build/lib",
    "-C", "link-arg=-lLLVMDynamicLineCountRuntime"
]
```

Replace `/path/to/llvm/build/lib` with the actual path to your LLVM build's lib directory.

### Quick Fix for Linking Errors

If you get linking errors, use one of these solutions:

1. **Copy to system library path**:
   ```bash
   sudo cp /path/to/llvm/build/lib/libLLVMDynamicLineCountRuntime.a /usr/lib/
   ```
   
   Then use this in `.cargo/config.toml`:
   ```toml
   rustflags = [
       "-C", "passes=unsafe-analysis-pass,instmarker,dynamic-line-count",
       "-C", "link-arg=-lLLVMDynamicLineCountRuntime"
   ]
   ```

2. **Copy to your project**:
   ```bash
   mkdir -p target/lib
   cp /path/to/llvm/build/lib/libLLVMDynamicLineCountRuntime.a target/lib/
   ```
   
   Then update `.cargo/config.toml`:
   ```toml
   rustflags = [
       "-C", "passes=unsafe-analysis-pass,instmarker,dynamic-line-count",
       "-C", "link-arg=-L${PWD}/target/lib",
       "-C", "link-arg=-lLLVMDynamicLineCountRuntime"
   ]
   ```

## Coverage Report

At program exit, a detailed report shows:

```
=== Unsafe Code Coverage Report ===

File                     |     Unsafe |   Executed |    Missing |   Coverage
-----------------------------------------------------------------------
src/example.rs           |         12 |         10 |          2 |     83.33%
   Missing lines: 45, 67
src/lib.rs               |          5 |          3 |          2 |     60.00%
   Missing lines: 23, 145

=== Final Summary ===
Total Unsafe Lines: 17
Total Executed: 13
Total Missing: 4
Overall Coverage: 76.47%

=== Block Statistics ===
Total Unsafe Blocks: 8
Total Unsafe Instructions: 32
Avg Instructions Per Block: 4.00

  ❌ Missing 4 unsafe lines (76.47% coverage)
```

## Key Points

- Works seamlessly with Rust projects with minimal configuration
- Provides precise insights into unsafe code usage and coverage
- Helps identify untested unsafe code paths
- Lightweight implementation with no external dependencies

This pass is an essential tool for understanding and improving the safety of Rust codebases that use unsafe code.
