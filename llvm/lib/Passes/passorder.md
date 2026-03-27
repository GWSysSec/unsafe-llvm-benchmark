# Pass Pipeline Order

## Pass Directory Structure

```
llvm/lib/Transforms/
├── InstMarker/          — marks unsafe code blocks (shared foundation)
├── DynamicAnalysis/     — consolidated dynamic analysis passes
│   ├── HeapTracker        — tracks heap memory accesses in unsafe code
│   ├── DynamicLineCount   — tracks unsafe source line coverage at runtime
│   ├── CpuCycleCount      — measures CPU cycles spent in unsafe code
│   ├── ExternalCallTracker — tracks external function calls
│   ├── UnsafeFunctionTracker — tracks unsafe functions
│   └── UnsafeInstCounter  — counts unsafe instructions
├── UnsafeRustDummy/     — example/reference pass
└── SVFAnalysis/         — pointer analysis (separate project)
    ├── UnsafeHeapAllocAnalysis — SVF-based heap allocation analysis
    ├── UnsafeHeapInstrumentation — heap instrumentation based on SVF
    ├── IntToPtrDDA        — inttoptr demand-driven analysis
    └── RuntimeAlias       — runtime alias analysis
```

## buildO0DefaultPipeline (No Optimization)

Location: PassBuilderPipelines.cpp

Pass Order:
1. **InstMarkerPass()** — instruction marking (if enabled)
2. **UnsafeRustDummyPass()** — dummy pass (if enabled)
3. **RuntimeAliasPass()** — runtime alias analysis [SVF] (if enabled)
4. **UnsafeHeapAllocAnalysis** — SVF heap analysis [SVF] (if enabled)
5. SampleProfileProbePass() — profile instrumentation (if PGO enabled)
6. addPGOInstrPassesForO0() — PGO instrumentation for O0 (if enabled)
7. AddDiscriminatorsPass() — debug discriminators (if enabled)
8. AlwaysInlinerPass() — minimal inlining
9. MergeFunctionsPass() — function merging (if enabled)
10. LowerMatrixIntrinsicsPass() — matrix intrinsics lowering (if enabled)
11. CoroEarlyPass/CoroSplitPass/CoroCleanupPass — coroutine handling
12. GlobalDCEPass() — dead code elimination
13. **Post-optimization stats** (if enabled):
    - HeapTrackerPass()
    - UnsafeFunctionTrackerPass()
    - UnsafeInstCounterPass()
    - DynamicLineCountPass()
    - CpuCycleCountPass()
    - ExternalCallTrackerPass()
14. AnnotationRemarksPass() — annotation remarks

## buildPerModuleDefaultPipeline (O1/O2/O3)

Location: PassBuilderPipelines.cpp

Pass Order:
1. **InstMarkerPass()** — instruction marking (if enabled)
2. **UnsafeRustDummyPass()** — dummy pass (if enabled)
3. **RuntimeAliasPass()** — runtime alias analysis [SVF] (if enabled)
4. Annotation2MetadataPass() — convert annotations to metadata
5. ForceFunctionAttrsPass() — force function attributes
6. AddDiscriminatorsPass() — debug discriminators (if PGO)
7. buildModuleSimplificationPipeline() — core simplification (includes inlining)
8. **UnsafeHeapAllocAnalysis + UnsafeHeapInstrumentation** [SVF] (if enabled)
   - Placed after simplification but before optimization
   - Box::new / exchange_malloc / __rust_alloc are inlined at this point
9. buildModuleOptimizationPipeline() — core optimization
10. PseudoProbeUpdatePass() — pseudo probe updates (if PGO)
11. addAnnotationRemarksPass() — annotation remarks
12. **Post-optimization stats** (if enabled):
    - HeapTrackerPass()
    - UnsafeFunctionTrackerPass()
    - UnsafeInstCounterPass()
    - DynamicLineCountPass()
    - CpuCycleCountPass()
    - ExternalCallTrackerPass()

## Key Design Decisions

### InstMarker
- Shared foundation for both dynamic analysis and SVF projects
- Must run first — all other passes depend on its metadata

### Post-Optimization Stats Collection
- All dynamic analysis passes run after optimizations complete
- Stats capture final optimized code characteristics
- Stats won't be eliminated by optimization passes

### SVF Pipeline Placement
- RuntimeAlias runs early (before simplification)
- UnsafeHeapAllocAnalysis runs after simplification but before optimization
  (inlined allocations are visible, but aggressive opts haven't run yet)
