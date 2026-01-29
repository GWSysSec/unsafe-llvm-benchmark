
file(GLOB_RECURSE SVF_CORE_SOURCES ${CMAKE_CURRENT_LIST_DIR}/lib/*.cpp)

# Use add_llvm_library to integrate typical build flags
add_llvm_library(SvfCore
    ${SVF_CORE_SOURCES}
    
    LINK_LIBS
    ${Z3_LIBRARIES}
)
