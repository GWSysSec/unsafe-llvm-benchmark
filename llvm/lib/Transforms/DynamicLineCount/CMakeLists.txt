# Build the pass with runtime code included directly
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

  # Build as a static library for easier linking with Rust
  STATIC
)

# Make runtime symbols visible and ensure they're not stripped
set_target_properties(LLVMDynamicLineCount PROPERTIES
  ENABLE_EXPORTS ON
  POSITION_INDEPENDENT_CODE ON
)

# Explicitly link with libc
target_link_libraries(LLVMDynamicLineCount PRIVATE
  c
)

# Add compiler flags to ensure static library works properly
target_compile_options(LLVMDynamicLineCount PRIVATE
  -fvisibility=default
  -fno-rtti
  -fPIC
  -ffunction-sections
  -fdata-sections
)
