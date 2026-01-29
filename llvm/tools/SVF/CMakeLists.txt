
set(SVF_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SVF_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

# Options
option(SVF_ENABLE_RTTI "Enable RTTI" ON)
option(SVF_ENABLE_ASSERTIONS "Enable Assertions" ON)

# Z3 Handling: Use LLVM's if available, else find it
if (LLVM_Z3_FOUND OR LLVM_ENABLE_Z3_SOLVER)
    message(STATUS "SVF: Using LLVM Z3 Provider")
    set(Z3_LIBRARIES ${LLVM_Z3_LIBRARIES})
else()
    # Fallback to local search if LLVM didn't expose it
    find_package(Z3 REQUIRED)
endif()

# Global Includes for SVF
include_directories(
    ${SVF_SOURCE_DIR}/svf/include
    ${SVF_SOURCE_DIR}/svf-llvm/include
    ${SVF_BINARY_DIR}/include
)

# Generate config.h
configure_file(${SVF_SOURCE_DIR}/cmake/SVFConfigHdr.cmake.in ${SVF_BINARY_DIR}/include/Util/config.h @ONLY)

# Propagate options
if (SVF_ENABLE_RTTI)
   # LLVM might create flags, we ensure consistency
endif()

# Core and LLVM adapter
add_subdirectory(svf)
add_subdirectory(svf-llvm)
