add_llvm_component_library(LLVMSourceCodeMapping
    SourceCodeMapping.cpp

    LINK_COMPONENTS
    Core
    Analysis
    )
