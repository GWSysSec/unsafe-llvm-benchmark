add_llvm_component_library(LLVMHeapTracker
  HeapTracker.cpp

  LINK_COMPONENTS
  Core
  Analysis
  Support
  InstMarker
)
