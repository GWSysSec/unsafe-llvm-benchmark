# Dynamic Analysis Passes for Unsafe Rust

Custom LLVM passes that instrument unsafe Rust code at the IR level for runtime profiling and benchmarking. Part of the [unsafe-rust-benchmark](https://github.com/GWSysSec/unsafe-rust-benchmark) project.

## Architecture

### Two-Layer Instrumentation

The system spans both rustc and LLVM:

**Layer 1 — rustc** (`compiler/rustc_codegen_llvm/`):
During MIR-to-LLVM-IR lowering, rustc attaches metadata to instructions generated inside `unsafe {}` blocks:
- `!unsafe_inst` — marks every instruction inside an unsafe block or `unsafe fn`
- `!stdlib_call` — marks calls to stdlib crates (`std`, `core`, `alloc`, etc.) inside unsafe blocks

rustc has authoritative knowledge of what is unsafe (via `DefId`, the crate graph, and MIR unsafety analysis). LLVM passes consume this metadata downstream.

**Layer 2 — LLVM passes** (this directory):
Read metadata from Layer 1, convert it to durable markers, and insert runtime instrumentation calls.

### The Marker System

InstMarker is the foundation pass. It converts ephemeral `!unsafe_inst` metadata into **durable inline assembly markers** that survive LLVM optimization:

```
Before InstMarker:
    %x = load i64, ptr %p, !unsafe_inst !0    ← metadata, fragile

After InstMarker:
    call void asm sideeffect "# unsafe_region_begin", ...  ← marker, durable
    %x = load i64, ptr %p, !unsafe_inst !0
    call void asm sideeffect "# unsafe_region_end", ...
```

**Why markers?** LLVM does not preserve custom metadata through optimization. Standard metadata like `!dbg` and `!tbaa` have special preservation logic in the optimizer; custom metadata like `!unsafe_inst` does not. When optimization passes transform, inline, or eliminate instructions, custom metadata is dropped. Inline assembly with `hasSideEffects=true` is treated as an opaque barrier — the optimizer cannot remove or reorder it.

**Metadata survival at different optimization levels:**

| Artifact | O0 | O2 | ThinLTO |
|----------|----|----|---------|
| `!unsafe_inst` metadata | survives | survives on non-eliminated instructions | survives on non-eliminated instructions |
| InstMarker inline asm | survives | survives | survives |
| `unsafe_source_lines` NamedMDNode | survives | survives | survives |

Note: earlier documentation claimed `!unsafe_inst` is "lost at O2". This is imprecise. The metadata survives on instructions that the optimizer keeps; it is lost only when the instruction itself is eliminated (dead code elimination, constant folding, etc.). Since survival is not guaranteed for all instructions, the inline asm markers are the **authoritative signal** for downstream passes.

### Pass Pipeline Placement

All passes run inside `buildPerModuleDefaultPipeline()` and `buildThinLTOPreLinkDefaultPipeline()` in `PassBuilderPipelines.cpp`. The placement is:

```
                    ┌─────────────────────────────────┐
                    │  InstMarker (EARLY, before opt)  │
                    │  reads !unsafe_inst → markers     │
                    └──────────────┬──────────────────┘
                                   │
                    ┌──────────────▼──────────────────┐
                    │  LLVM optimization passes (O2)   │
                    │  metadata may be dropped          │
                    │  markers survive                  │
                    └──────────────┬──────────────────┘
                                   │
                    ┌──────────────▼──────────────────┐
                    │  Analysis passes (LATE, post-opt) │
                    │  read markers, insert runtime     │
                    │  instrumentation calls             │
                    └─────────────────────────────────┘
```

**Why this placement?**
- InstMarker must run **before** optimization to capture all `!unsafe_inst` metadata before any is lost.
- Analysis passes run **after** optimization so they instrument the final IR — what actually executes at runtime.

**Why both PerModuleDefault and ThinLTO pre-link?**

Cargo defaults to ThinLocal LTO at O1+. Without `--emit=llvm-ir` or `lto=false`, rustc uses `Lto::ThinLocal`, which routes through `buildThinLTOPreLinkDefaultPipeline()`, not `buildPerModuleDefaultPipeline()`. Passes must be registered in both pipelines.

For ThinLTO, all passes are placed in the **pre-link** pipeline (not post-link). This is because passes create global constructor/destructor functions (`@llvm.global_ctors`, `@llvm.global_dtors`) for runtime initialization and cleanup. ThinLTO internalization in the post-link phase can strip these functions, breaking the runtime. Pre-link placement ensures ctors/dtors enter the module summary and survive.

**LTO mode and optimization level are independent:**

| | O0 | O2 |
|---|---|---|
| `lto=false` | No opt, PerModuleDefault pipeline | Full opt, PerModuleDefault pipeline |
| ThinLocal (Cargo default at O1+) | N/A (forced off) | Full opt, ThinLTO pre-link + post-link pipeline |
| `lto=thin` | N/A | Full opt, cross-crate ThinLTO pipeline |

For profiling workflows, `lto=false` with `opt-level=2` is recommended: you get realistic O2 optimization quality with the simpler PerModuleDefault pipeline.

## Pass Descriptions

### InstMarker (foundation)
- **Type:** Function pass
- **Location:** `../InstMarker/InstMarker.cpp`
- **Reads:** `!unsafe_inst` metadata on instructions
- **Emits:** Inline asm `marker_begin` / `marker_end` pairs around unsafe regions
- **Emits:** `unsafe_source_lines` NamedMDNode (source locations of unsafe code)
- **Must run first.** All other passes depend on its markers.

### UnsafeFunctionTracker
- **Type:** Module pass
- **Purpose:** Track which functions contain unsafe code and which are executed at runtime
- **Reads:** Inline asm markers from InstMarker
- **Emits:** Global metadata table with per-function flags, function entry instrumentation
- **Runtime hooks:** `__unsafe_init_metadata()`, `__unsafe_record_function()`, `__unsafe_dump_stats()`

**Design choice — `hasUnsafeRegions` vs `hasUnsafeInst`:**

The metadata table stores two flags per function:
- `hasUnsafeRegions`: function contains marker pairs (source-level unsafe)
- `hasUnsafeInst`: function has `!unsafe_inst` metadata inside a marker region

`hasUnsafeRegions` is the **primary flag** for determining whether a function is "unsafe." It is based on marker presence, which is authoritative across all optimization levels. `hasUnsafeInst` is a secondary refinement — it indicates that the metadata also survived optimization, meaning the original unsafe instructions were not eliminated. The runtime reports a function as unsafe if either flag is set.

### UnsafeInstCounter
- **Type:** Function pass
- **Purpose:** Count and categorize unsafe instructions executed at runtime
- **Reads:** Inline asm markers
- **Categories:** LOAD, STORE, CALL, CAST, GEP, OTHER
- **Runtime hooks:** `__unsafe_record_block()`
- **Counts all instructions between markers**, not just those with metadata. This gives the runtime-truth view: how many instructions actually execute inside unsafe regions.

### HeapTracker
- **Type:** Function pass
- **Purpose:** Track heap memory accesses (load/store) inside unsafe regions
- **Reads:** Inline asm markers, identifies SESE (Single Entry Single Exit) regions
- **Runtime hooks:** `__unsafe_heap_record_access()`, `__unsafe_heap_dump_stats()`

### DynamicLineCount
- **Type:** Module pass
- **Purpose:** Track which unsafe source lines are executed at runtime
- **Reads:** `unsafe_source_lines` NamedMDNode, `!dbg` locations
- **Requires:** `debug=2` in the Cargo profile (for line/column info)
- **Runtime hooks:** `__unsafe_line_executed()`, `__unsafe_line_dump_stats()`

### ExternalCallTracker
- **Type:** Module pass
- **Purpose:** Track calls to external functions (FFI), distinguishing those inside vs outside unsafe regions
- **Reads:** Inline asm markers
- **Co-runs with:** CpuCycleCount (same experiment)

### CpuCycleCount
- **Type:** Module pass
- **Purpose:** Measure CPU cycles spent in unsafe vs safe code using TSC (Time Stamp Counter)
- **Reads:** Inline asm markers
- **MUST RUN LAST** — it removes markers after instrumenting them. All other passes must run before this one.
- **Runtime hooks:** `__unsafe_cpu_cycle_*()` family

### UnsafeAnalysisUtils (shared utilities)
- **Header:** `include/llvm/Transforms/DynamicAnalysis/UnsafeAnalysisUtils.h`
- **Implementation:** `UnsafeAnalysisUtils.cpp`
- **Purpose:** Shared infrastructure used by all passes. Extracted in Phase 0 to eliminate duplication — these functions were previously copy-pasted across every pass.
- **Provides:**
  - `isPrimaryPackage()` — checks `CARGO_PRIMARY_PACKAGE` env var. Every pass calls this as its first guard: if false, the pass returns `PreservedAnalyses::all()` immediately. This ensures only the primary crate (not dependencies) is instrumented.
  - `getInstrumentationDebugLoc(Instruction *InsertPt)` — finds the nearest valid `DILocation` for an instrumentation call. LLVM's verifier requires all calls in debug-info-enabled modules to carry a `!dbg` location; this helper walks the insertion point's neighbors to find one.
  - `isMarkerBegin(Instruction &I)` / `isMarkerEnd(Instruction &I)` — detect InstMarker's inline asm `marker_begin` / `marker_end` calls.
  - `isMarkerInstruction(Instruction &I, bool &isBegin, bool &isEnd)` — combined check, returns both flags. Used in linear-scan region detection loops.
  - `isMarkerAsm(const CallInst &CI)` — low-level check for inline asm with marker comment strings.
  - `verifyUnsafeMarkers(Function &F)` — debug-only assertion that validates marker well-formedness: every `marker_begin` has a matching `marker_end` in the same BB, no orphaned ends, no improper nesting. Called by downstream passes as a sanity check. No-op in release builds (`#ifdef NDEBUG`).

## Unsafe Region Detection

Passes need to determine which instructions are "inside an unsafe region" (between `marker_begin` and `marker_end`). Two approaches are used:

### Linear scan (simple, intra-BB only)

Used by: **UnsafeInstCounter**, **UnsafeFunctionTracker**, **ExternalCallTracker**

```cpp
bool inRegion = false;
for (Instruction &I : BB) {
    if (isMarkerBegin(I))  inRegion = true;
    if (isMarkerEnd(I))    inRegion = false;
    if (inRegion) { /* instruction is in unsafe region */ }
}
```

This works when `marker_begin` and `marker_end` are in the **same basic block**, which is the common case for simple `unsafe {}` blocks. However, it misses unsafe regions that span multiple basic blocks (e.g., `unsafe { if cond { ... } else { ... } }`), because the begin and end markers land in different BBs.

### SESE region detection (robust, cross-BB)

Used by: **HeapTracker**

Uses `DominatorTree` and `PostDominatorTree` to validate marker pairs across basic blocks:
- A valid region requires: `marker_begin` **dominates** `marker_end` AND `marker_end` **post-dominates** `marker_begin`
- An instruction is inside a region if its BB is dominated by the begin and post-dominated by the end

This correctly handles control flow divergence (if/else, loops, match arms) within unsafe blocks.

### Current status and plan

| Pass | Region detection | Cross-BB correct? |
|---|---|---|
| HeapTracker | SESE (DomTree + PostDomTree) | Yes |
| UnsafeInstCounter | Linear scan | No — misses cross-BB regions |
| UnsafeFunctionTracker | Linear scan | N/A — only checks marker presence, not containment |
| ExternalCallTracker | Linear scan | No — misses cross-BB regions |
| CpuCycleCount | Direct marker-site instrumentation | N/A — instruments at marker locations, not between |
| DynamicLineCount | NamedMDNode-based | N/A — uses source line info, not markers |

Migrating UnsafeInstCounter and ExternalCallTracker to SESE is a planned improvement. The SESE validation logic in HeapTracker should be extracted into `UnsafeAnalysisUtils` as a shared `collectValidSESERegions()` utility to avoid duplication.

## Experiment Model

Passes run **one experiment at a time**, not simultaneously. Each experiment enables only its pass(es) plus InstMarker:

| Experiment | Passes |
|---|---|
| Heap tracking | InstMarker + HeapTracker |
| Instruction counting | InstMarker + UnsafeFunctionTracker + UnsafeInstCounter |
| Line coverage | InstMarker + DynamicLineCount |
| CPU cycle measurement | InstMarker + ExternalCallTracker + CpuCycleCount |

This means passes do not need to account for each other's runtime functions — no skip-lists or cross-pass coordination (except the ExternalCallTracker + CpuCycleCount pair).

## Pass Enable Flags

Each pass has a `--enable-*` cl::opt in `PassBuilderPipelines.cpp`:

```
--enable-instmarker
--enable-heap-tracker
--enable-unsafe-function-tracker
--enable-unsafe-inst-counter
--enable-dynamic-line-count
--enable-external-call-tracker
--enable-cpu-cycle-count
```

Pass flags are typically set via `rustflags` in `.cargo/config.toml`:
```toml
[build]
rustflags = ["-C", "llvm-args=--enable-instmarker --enable-unsafe-function-tracker --enable-unsafe-inst-counter"]
```

## Runtime Library

The Rust runtime that receives instrumentation calls lives in a separate repository:
`~/Projects/unsafebench/unsafe-dyn-rust-expr/lib/perf/src/`

Each pass has a corresponding runtime module. Runtime hooks are `#[no_mangle] extern "C"` functions linked at compile time. The runtime uses lock-free data structures (atomic bitsets, cache-padded counters) for thread safety.

## Building

**Never build LLVM directly with cmake/ninja.** Always use rustc's build system:

```bash
cd ~/Projects/unsafebench/unsafe-rust-benchmark
python3 x.py build library --stage 1
```

The stage 1 compiler with custom passes is at:
`build/x86_64-unknown-linux-gnu/stage1/bin/rustc`
