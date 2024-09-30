add_llvm_component_library(LLVMUnsafe-rust-test-passes
  MyPass.cpp
  LineCount.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/Unsafe-rust-test-passes

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  AggressiveInstCombine
  Analysis
  CFGuard
  CodeGen
  Core
  Coroutines
  HipStdPar
  IPO
  InstCombine
  IRPrinter
  Passes
  ObjCARC
  Scalar
  Support
  Target
  TransformUtils
  Vectorize
  Instrumentation
  )
