# Add the runtime subdirectory
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
  Analysis
  InstMarker
)

# Explicitly link with the runtime library
target_link_libraries(LLVMDynamicLineCount PRIVATE
  LLVMDynamicLineCountRuntime
)

# Make sure the pass can find runtime symbols
set_target_properties(LLVMDynamicLineCount PROPERTIES
  ENABLE_EXPORTS ON
  INTERFACE_LINK_LIBRARIES "LLVMDynamicLineCountRuntime"
)
