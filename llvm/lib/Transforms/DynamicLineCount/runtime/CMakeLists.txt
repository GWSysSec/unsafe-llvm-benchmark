add_llvm_library(LLVMDynamicLineCountRuntime
  SHARED
  DynamicLineCountRuntime.cpp

  DEPENDS
  intrinsics_gen
  
  LINK_COMPONENTS
  Support
)

# visibility settings for exports
target_compile_definitions(LLVMDynamicLineCountRuntime PRIVATE
  "LLVM_DYNAMICLINECOUNT_BUILD_SHARED"
)

# set target properties
set_target_properties(LLVMDynamicLineCountRuntime PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  OUTPUT_NAME "LLVMDynamicLineCountRuntime"
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

# install runtime components
install(TARGETS LLVMDynamicLineCountRuntime
  LIBRARY DESTINATION lib${LLVM_LIBDIR_SUFFIX}
  ARCHIVE DESTINATION lib${LLVM_LIBDIR_SUFFIX}
  RUNTIME DESTINATION bin
)

# Install headers
install(FILES
  DynamicLineCountRuntime.h
  DESTINATION include/llvm/Transforms/DynamicLineCount/runtime
)
