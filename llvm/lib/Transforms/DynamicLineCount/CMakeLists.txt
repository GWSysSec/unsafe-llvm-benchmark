# Most minimal approach: build the pass with runtime code included directly
add_llvm_component_library(LLVMDynamicLineCount
  # Main pass source
  DynamicLineCount.cpp
  # Include runtime code directly in the pass
  runtime/DynamicLineCountRuntime.cpp

  DEPENDS
  intrinsics_gen
  
  LINK_COMPONENTS
  Core
  Support
  IRReader
  Analysis
  InstMarker
)

# Make runtime symbols visible
set_target_properties(LLVMDynamicLineCount PROPERTIES
  ENABLE_EXPORTS ON
  POSITION_INDEPENDENT_CODE ON
)
