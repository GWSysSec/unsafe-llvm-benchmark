
file(GLOB_RECURSE SVF_CORE_SOURCES ${CMAKE_CURRENT_LIST_DIR}/lib/*.cpp)

# Use add_llvm_library to integrate typical build flags
add_definitions(-DSVF_BUILD_DIR="${CMAKE_BINARY_DIR}")
add_definitions(-DSVF_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
add_llvm_component_library(LLVMSvfCore
    ${SVF_CORE_SOURCES}
    
    LINK_LIBS
    ${Z3_LIBRARIES}
)
