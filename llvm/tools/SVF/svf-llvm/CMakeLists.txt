
# Generate extapi.bc
set(EXTAPI_SRC ${CMAKE_CURRENT_LIST_DIR}/lib/extapi.c)
set(SVF_BUILD_EXTAPI_BC ${CMAKE_CURRENT_BINARY_DIR}/extapi.bc)

# We use system clang because in-tree clang might not be ready/enabled
find_program(SYSTEM_CLANG clang REQUIRED)

add_custom_command(
    OUTPUT ${SVF_BUILD_EXTAPI_BC}
    COMMAND ${SYSTEM_CLANG} -w -S -c -fPIC -std=gnu11 -emit-llvm -o ${SVF_BUILD_EXTAPI_BC} ${EXTAPI_SRC}
    DEPENDS ${EXTAPI_SRC}
    COMMENT "Generating extapi.bc from ${EXTAPI_SRC}..."
)

add_custom_target(gen_extapi_ir ALL DEPENDS ${SVF_BUILD_EXTAPI_BC})

file(GLOB_RECURSE SVF_LLVM_SOURCES ${CMAKE_CURRENT_LIST_DIR}/lib/*.cpp)

# Map generic components to LLVM libraries
add_llvm_library(SvfLLVM
    ${SVF_LLVM_SOURCES}

    DEPENDS
    gen_extapi_ir
    
    LINK_LIBS
    SvfCore
    ${Z3_LIBRARIES}

    LINK_COMPONENTS
    Core
    Support
    IRReader
    BitReader
    Analysis
    TransformUtils
    Linker
    LTO
    Target
    BitWriter
    ScalarOpts
    IPO
    Instrumentation
    Demangle
)

# Build SVF Tools (wpa, svf-ex, etc.)
add_subdirectory(tools)
