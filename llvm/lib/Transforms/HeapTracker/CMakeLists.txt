add_llvm_component_library(LLVMHeapTracker
  HeapTracker.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/HeapTracker

  LINK_COMPONENTS
  Analysis
  Core
  InstMarker
  Support
)