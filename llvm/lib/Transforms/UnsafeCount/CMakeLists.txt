add_llvm_component_library(LLVMUnsafeCount
  UnsafeFunctionTracker.cpp
  UnsafeInstCounter.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/UnsafeCount

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  Core
  InstMarker
  IRReader
  Support
)