# InstMarker Pass

An LLVM pass for marking unsafe instructions in Rust code. This pass serves as the **foundation layer** of the unsafe code analysis infrastructure, providing clean boundaries for downstream analysis passes.

## Core Functionality

- **Instruction Marking**: Inserts inline assembly markers (`marker_begin`/`marker_end`) around unsafe code blocks
- **Foundation Layer**: Provides a clean interface for other passes without runtime dependencies

## Architecture

InstMarker follows a **minimal foundation design**:

**InstMarkerPass**: Inserts only `marker_begin` and `marker_end` inline assembly around unsafe blocks

**No runtime calls** - InstMarker stays simple and lets downstream passes handle their own measurement/tracking.

## Integration with Other Passes

InstMarker serves as the foundation for:

- **CpuCycleCount**: Measures CPU cycles between `marker_begin`/`marker_end`
- **DynamicLineCount**: Tracks line coverage using the markers
- **HeapTracker**: Monitors memory operations within marked regions

Each downstream pass:
1. Scans for `marker_begin`/`marker_end` inline assembly
2. Implements its own runtime tracking and measurement
3. Maintains independence from other passes

## Usage

This pass has been integrated into the Rust compiler pipeline.