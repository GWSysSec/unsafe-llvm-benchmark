  buildO0DefaultPipeline (No Optimization)

  Location: /home/oscar/Projects/unsafebench/unsafe-rust-benchmark/src/llvm-project/llvm/lib/Passes/PassBuilderPipelines.cpp:2070

  Pass Order:
  1. UnsafeAnalysisPass() - Custom unsafe analysis (function-level)
  2. InstMarkerPass() - Instruction marking (ALWAYS ENABLED)
  3. UnsafeRustDummyPass() - Custom dummy pass (if enabled)
  4. SampleProfileProbePass() - Profile instrumentation (if PGO enabled)
  5. addPGOInstrPassesForO0() - PGO instrumentation for O0 (if enabled)
  6. AddDiscriminatorsPass() - Debug discriminators (if enabled)
  7. AlwaysInlinerPass() - Minimal inlining (lifetime intrinsics disabled)
  8. MergeFunctionsPass() - Function merging (if enabled)
  9. LowerMatrixIntrinsicsPass() - Matrix intrinsics lowering (if enabled)
  10. Extension point callbacks - Various callbacks for customization
  11. CoroEarlyPass/CoroSplitPass/CoroCleanupPass - Coroutine handling
  12. GlobalDCEPass() - Dead code elimination
  13. DynamicLineCountPass() - Dynamic line counting (POST-OPTIMIZATION, if enabled)
  14. AnnotationRemarksPass() - Annotation remarks

  ---
  buildPerModuleDefaultPipeline (O1/O2/O3)

  Location: /home/oscar/Projects/unsafebench/unsafe-rust-benchmark/src/llvm-project/llvm/lib/Passes/PassBuilderPipelines.cpp:1522

  Pass Order:
  1. UnsafeAnalysisPass() - Custom unsafe analysis
  2. InstMarkerPass() - Instruction marking (ALWAYS ENABLED for O1/O2/O3)
  3. Annotation2MetadataPass() - Convert annotations to metadata
  4. ForceFunctionAttrsPass() - Force function attributes
  5. AddDiscriminatorsPass() - Debug discriminators (if PGO)
  6. buildModuleSimplificationPipeline() - Core simplification
  7. buildModuleOptimizationPipeline() - Core optimization
  8. PseudoProbeUpdatePass() - Pseudo probe updates (if PGO)
  9. addAnnotationRemarksPass() - Annotation remarks
  10. CpuCycleCountPass() - CPU cycle counting (POST-OPTIMIZATION, if enabled)
  11. HeapTrackerPass() - Heap tracking (POST-OPTIMIZATION, if enabled)

  ---
  buildModuleSimplificationPipeline (Core Simplification)

  Location: /home/oscar/Projects/unsafebench/unsafe-rust-benchmark/src/llvm-project/llvm/lib/Passes/PassBuilderPipelines.cpp:1012

  Pass Order:
  1. SampleProfileProbePass() - Profile instrumentation
  2. PGOIndirectCallPromotion() - Indirect call promotion (ThinLTO)
  3. Frontend cleanup (non-ThinLTO postlink):
    - InferFunctionAttrsPass() - Infer function attributes
    - CoroEarlyPass() - Early coroutine handling
    - Early function passes:
        - LowerExpectIntrinsicPass() - Lower expect intrinsics
      - SimplifyCFGPass() - Control flow simplification
      - SROAPass() - Scalar replacement of aggregates
      - EarlyCSEPass() - Early common subexpression elimination
      - CallSiteSplittingPass() - Call site splitting (O3 only)
  4. SampleProfileLoaderPass() - Sample profile loading
  5. PGOIndirectCallPromotion() - Indirect call promotion
  6. OpenMPOptPass() - OpenMP optimizations
  7. AttributorPass() - Attributor analysis
  8. LowerTypeTestsPass() - Type test lowering (ThinLTO)
  9. IPSCCPPass() - Interprocedural SCCP
  10. CalledValuePropagationPass() - Called value propagation
  11. GlobalOptPass() - Global optimizations
  12. Global cleanup passes:
    - PromotePass() - Promote memory to register
    - InstCombinePass() - Instruction combining
    - SimplifyCFGPass() - CFG simplification
  13. PGO instrumentation passes (if enabled)
  14. MemProfUsePass() - Memory profile usage
  15. SyntheticCountsPropagation() - Synthetic counts
  16. AlwaysInlinerPass() - Always inlining
  17. buildModuleInlinerPipeline()/buildInlinerPipeline() - Inlining
  18. DeadArgumentEliminationPass() - Dead argument elimination
  19. CoroCleanupPass() - Coroutine cleanup
  20. GlobalOptPass() - Final global optimizations
  21. GlobalDCEPass() - Global dead code elimination

  ---
  buildModuleOptimizationPipeline (Core Optimization)

  Location: /home/oscar/Projects/unsafebench/unsafe-rust-benchmark/src/llvm-project/llvm/lib/Passes/PassBuilderPipelines.cpp:1335

  Pass Order:
  1. PartialInlinerPass() - Partial inlining
  2. EliminateAvailableExternallyPass() - Eliminate available externally
  3. InstrOrderFilePass() - Instruction ordering (if enabled)
  4. ReversePostOrderFunctionAttrsPass() - RPO function attributes
  5. Context-sensitive PGO passes (if enabled)
  6. RecomputeGlobalsAAPass() - Recompute globals alias analysis
  7. Function optimization passes:
    - LoopVersioningLICMPass() - Loop versioning LICM
    - LICMPass() - Loop invariant code motion
    - Float2IntPass() - Float to int conversion
    - LowerConstantIntrinsicsPass() - Lower constant intrinsics
    - LowerMatrixIntrinsicsPass() - Matrix intrinsics (if enabled)
    - EarlyCSEPass() - Early CSE after matrix lowering
    - ControlHeightReductionPass() - CHR (O3 only)
    - Loop optimization passes:
        - LoopRotatePass() - Loop rotation
      - LoopDeletionPass() - Loop deletion
    - LoopDistributePass() - Loop distribution
    - InjectTLIMappings() - TLI mappings
    - addVectorPasses() - Vectorization passes
    - LoopSinkPass() - Loop sinking
    - InstSimplifyPass() - Instruction simplification
    - DivRemPairsPass() - Div/rem pairs
    - TailCallElimPass() - Tail call elimination
    - SimplifyCFGPass() - Final CFG simplification
  8. HotColdSplittingPass() - Hot/cold splitting
  9. IROutlinerPass() - IR outlining
  10. MergeFunctionsPass() - Function merging
  11. GlobalDCEPass() - Global dead code elimination
  12. ConstantMergePass() - Constant merging
  13. CGProfilePass() - Call graph profile
  14. RelLookupTableConverterPass() - Relative lookup table converter

---

## Key Changes Made

### InstMarker Pass

- **O0**: Always enabled (was previously conditional)
- **O1/O2/O3**: Always enabled (was previously only for O3 when enabled)
- **Purpose**: Preserves original instruction information before optimizations

### Post-Optimization Stats Collection

- **O0**: DynamicLineCount moved to post-optimization (after minimal optimizations)
- **O1/O2/O3**: CpuCycleCount and HeapTracker moved to post-optimization (after full optimizations)
- **Purpose**: Captures final optimized code characteristics and prevents optimization away

### Pipeline Benefits

- Consistent InstMarker behavior across all optimization levels
- Stats collection happens after optimizations complete
- Stats won't be eliminated by optimization passes
- Better separation of concerns between optimization and instrumentation
