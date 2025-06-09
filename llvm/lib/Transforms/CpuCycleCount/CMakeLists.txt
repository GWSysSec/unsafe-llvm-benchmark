# Build the pass with runtime code included directly
add_llvm_component_library(LLVMCpuCycleCount
  # Main pass source
  CpuCycleCount.cpp
  # Include runtime code directly in the pass
  runtime/CpuCycleCountRuntime.cpp

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
set_target_properties(LLVMCpuCycleCount PROPERTIES
  ENABLE_EXPORTS ON
  POSITION_INDEPENDENT_CODE ON
)

# Explicitly link with libc and pthread for runtime functions
target_link_libraries(LLVMCpuCycleCount PRIVATE
  c
  pthread
)

# Add compiler flags to ensure static library works properly
target_compile_options(LLVMCpuCycleCount PRIVATE
  -fvisibility=default
  -fno-rtti
  -fPIC
  -ffunction-sections
  -fdata-sections
)