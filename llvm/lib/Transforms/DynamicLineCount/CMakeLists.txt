add_llvm_component_library(LLVMDynamicLineCount
  DynamicLineCount.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/DynamicLineCount

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  Core
  InstMarker
  Support
)