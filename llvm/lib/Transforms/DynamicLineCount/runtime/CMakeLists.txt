add_llvm_library(LLVMDynamicLineCountRuntime
  STATIC
  DynamicLineCountRuntime.cpp

  DEPENDS
  intrinsics_gen
  
  LINK_COMPONENTS
  Support
)

# Define static library build
target_compile_definitions(LLVMDynamicLineCountRuntime PRIVATE
  "LLVM_DYNAMICLINECOUNT_STATIC"
)

# Explicitly link with C++ standard library
target_link_libraries(LLVMDynamicLineCountRuntime PRIVATE stdc++)

# Set target properties
set_target_properties(LLVMDynamicLineCountRuntime PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  OUTPUT_NAME "LLVMDynamicLineCountRuntime"
)

# Install runtime components
install(TARGETS LLVMDynamicLineCountRuntime
  LIBRARY DESTINATION lib${LLVM_LIBDIR_SUFFIX}
  ARCHIVE DESTINATION lib${LLVM_LIBDIR_SUFFIX}
)

# Install header with correct path
install(FILES
  DynamicLineCountRuntime.h
  DESTINATION include/llvm/Transforms/DynamicLineCount/runtime
)
