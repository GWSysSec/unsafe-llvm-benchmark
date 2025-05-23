# InstMarker Pass

An LLVM pass for marking and tracking unsafe instructions in Rust code. This pass identifies instructions marked with "unsafe_inst" metadata and inserts special inline assembly markers around blocks of unsafe code.

## Core Functionality

- **Unsafe Analysis**: Provides a foundational analysis for identifying and tracking unsafe code across the codebase
- **Instruction Marking**: Adds inline assembly markers around unsafe instruction blocks
- **Runtime Tracking**: Inserts calls to runtime functions for counting unsafe blocks

## Implementation Details

The pass consists of two primary components:

1. **UnsafeAnalysis**: Analyzes functions for unsafe instructions and prepares data structures for use by transformation passes
2. **InstMarkerPass**: Uses the UnsafeAnalysis results to insert assembly markers and runtime calls

## Integration

This pass serves as the foundation for the unsafe code analysis infrastructure. It's designed to:

- Run early in the optimization pipeline
- Provide analysis results for other passes like DynamicLineCount
- Insert markers used by HeapTracker for memory access tracking

## Usage

To run this pass:

```bash
opt -load-pass-plugin=LLVMInstMarker.so -passes=instmarker input.ll -o output.ll
```

This pass can also be integrated into the Rust compiler pipeline by adding appropriate flags to rustc.