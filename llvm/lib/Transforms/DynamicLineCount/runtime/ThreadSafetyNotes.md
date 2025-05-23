# Thread Safety Improvements in DynamicLineCount Runtime

This document explains the thread safety issues in the previous implementation and how they were addressed in the recent changes.

## Old Implementation Issues

1. **Recursive Lock Pattern**: 
   - Used a boolean flag `in_runtime` as a crude locking mechanism
   - `in_runtime = 1` at function start, `in_runtime = 0` at end
   - This approach has several problems:
     - Not actually thread-safe (boolean operations aren't atomic)
     - Can cause deadlocks if interrupted between setting/clearing the flag
     - Prevents any recursive functionality within the runtime

2. **Manual Initialization**:
   - Used `ensure_initialized()` function called at start of each public function
   - Relies on checking a global `initialized` flag
   - Race condition: multiple threads could call this at the same time

3. **Signal Handling**:
   - Simple signal handler only disabled the runtime
   - No guarantees that data remained consistent after signal
   - No proper reporting mechanism on program termination

4. **Bitmap Operations**:
   - Used macros for bit manipulation which can be error-prone
   - Didn't consistently use unsigned values for bitwise operations

## New Implementation Improvements

1. **Constructor Attribute for Initialization**:
   - Uses `__attribute__((constructor))` function which is guaranteed by the loader to run exactly once before any other code
   - Registration happens automatically at program startup
   - Completely eliminates race conditions during initialization

2. **Removed Recursive Lock Pattern**:
   - Completely removed the `in_runtime` flag
   - Each function now handles its operations directly without artificial locking
   - Operations are designed to be idempotent and safe even with concurrent calls

3. **Exit Handler Registration**:
   - Uses `atexit()` to register clean termination handler
   - Ensures coverage report will be printed even if program exits normally
   - Registration happens during constructor, guaranteeing it runs once

4. **Improved Bit Manipulation**:
   - Explicitly uses unsigned int types (`1U <<` instead of `1 <<`) for consistent behavior
   - Better variable naming with `bit_offset` for clarity
   - More consistent boundary checks

## Key Insight

Rather than trying to add complex locking, the new implementation makes each operation safe by design through:

1. **Idempotence**: Operations can be safely repeated (e.g., marking a line as unsafe multiple times has the same effect as marking it once)
2. **Atomic Bit Operations**: Bit operations within a 32-bit word are naturally atomic on most platforms
3. **Proper Initialization**: One-time initialization via constructor attributes
4. **Simpler Logic**: Removing control functions and focusing on core functionality

This approach is significantly more robust for multi-threaded Rust applications where multiple threads might execute unsafe code simultaneously.