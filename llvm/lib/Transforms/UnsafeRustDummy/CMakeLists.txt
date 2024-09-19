add_llvm_component_library(LLVMUnsafeRustDummy
    UnsafeRustDummy.cpp

    LINK_COMPONENTS
    Core
    Analysis
    )
