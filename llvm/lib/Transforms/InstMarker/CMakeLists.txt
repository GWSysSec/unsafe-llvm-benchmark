add_llvm_component_library(LLVMInstMarker
  InstMarker.cpp

  DEPENDS
  intrinsics_gen
  
  LINK_COMPONENTS
  Core
  Support
  IRReader
  Analysis
)
