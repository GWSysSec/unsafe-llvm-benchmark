# add the runtime subdirectory
add_subdirectory(runtime)

add_llvm_component_library(LLVMDynamicLineCount
  DynamicLineCount.cpp

  DEPENDS
  intrinsics_gen
  LLVMDynamicLineCountRuntime
  
  LINK_COMPONENTS
  Core
  Support
  IRReader
  InstCombine
  )
