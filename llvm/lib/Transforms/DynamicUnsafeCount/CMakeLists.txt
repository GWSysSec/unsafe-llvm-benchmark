add_llvm_component_library(LLVMDynamicUnsafeCount
    InlineMarker.cpp
    FunctionCount.cpp
    InlineCountUnsafe.cpp
    



    LINK_COMPONENTS
    Core
    Analysis
    )