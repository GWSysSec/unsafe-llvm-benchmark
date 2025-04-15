# Simple CMake file for building DynamicLineCount runtime library
add_llvm_library(LLVMDynamicLineCountRuntime
  STATIC
  DynamicLineCountRuntime.cpp
)

# Set basic compile flags
target_compile_options(LLVMDynamicLineCountRuntime PRIVATE
  -fvisibility=default
  -fPIC
)

# Set library properties
set_target_properties(LLVMDynamicLineCountRuntime PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

# Install runtime components
install(TARGETS LLVMDynamicLineCountRuntime
  ARCHIVE DESTINATION lib${LLVM_LIBDIR_SUFFIX}
)

# Install header
install(FILES
  DynamicLineCountRuntime.h
  DESTINATION include/llvm/Transforms/DynamicLineCount/runtime
)