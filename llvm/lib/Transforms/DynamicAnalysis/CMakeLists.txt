add_llvm_component_library(LLVMDynamicAnalysis
  UnsafeAnalysisUtils.cpp
  HeapTracker.cpp
  DynamicLineCount.cpp
  CpuCycleCount.cpp
  ExternalCallTracker.cpp
  UnsafeFunctionTracker.cpp
  UnsafeInstCounter.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/DynamicAnalysis

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  Core
  IRReader
  InstMarker
  Support
)
