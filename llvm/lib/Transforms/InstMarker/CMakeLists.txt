add_llvm_component_library(LLVMInstMarker
  InstMarker.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/InstMarker

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  Core
  IRReader
  Support
)