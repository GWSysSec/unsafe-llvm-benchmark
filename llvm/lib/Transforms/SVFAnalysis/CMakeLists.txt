include_directories(
  ${LLVM_MAIN_SRC_DIR}/lib/SVF/svf/include
  ${LLVM_MAIN_SRC_DIR}/lib/SVF/svf-llvm/include
  ${LLVM_BINARY_DIR}/lib/SVF/include
)

add_llvm_component_library(LLVMSVFAnalysis
  UnsafeHeapAllocAnalysis.cpp
  IntToPtrDDA.cpp
  UnsafeHeapInstrumentation.cpp
  RuntimeAlias.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/SVFAnalysis

  LINK_COMPONENTS
  SvfLLVM
  SvfCore
  Core
  Support
  TransformUtils
  Analysis

  )
