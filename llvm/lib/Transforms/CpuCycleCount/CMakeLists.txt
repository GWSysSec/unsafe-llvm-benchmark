add_llvm_component_library(LLVMCpuCycleCount
  CpuCycleCount.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/CpuCycleCount

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  Core
  InstMarker
  IRReader
  Support
)