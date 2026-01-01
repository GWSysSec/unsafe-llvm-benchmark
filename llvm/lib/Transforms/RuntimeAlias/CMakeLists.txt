add_llvm_component_library(LLVMRuntimeAlias
  RuntimeAlias.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/RuntimeAlias

  DEPENDS
  intrinsics_gen
  )
