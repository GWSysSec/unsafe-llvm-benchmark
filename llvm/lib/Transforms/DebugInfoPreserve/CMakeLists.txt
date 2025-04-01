add_llvm_component_library(LLVMDebugInfoPreserve
  DebugInfoPreserver.cpp
  
  LINK_COMPONENTS
  Core
  Support
)