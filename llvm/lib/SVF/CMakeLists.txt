
set(SVF_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SVF_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

# Options
option(SVF_ENABLE_RTTI "Enable RTTI" ON)
option(SVF_ENABLE_ASSERTIONS "Enable Assertions" ON)

# Z3 Handling: Use LLVM's if available, else find it
# Z3 Handling: Build in-tree source if available, else use LLVM's or find system install
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/z3/CMakeLists.txt")
    message(STATUS "SVF: Building Z3 from in-tree source")
    
    # Configure Z3 build: Static lib only, no tools/tests/examples
    set(Z3_BUILD_LIBZ3_SHARED OFF CACHE BOOL "Build Z3 static lib" FORCE)
    set(Z3_BUILD_EXECUTABLE OFF CACHE BOOL "Don't build Z3 shell" FORCE)
    set(Z3_BUILD_TEST_EXECUTABLES OFF CACHE BOOL "Don't build Z3 tests" FORCE)
    set(Z3_BUILD_PYTHON_BINDINGS OFF CACHE BOOL "Don't build python bindings" FORCE)
    set(Z3_BUILD_DOTNET_BINDINGS OFF CACHE BOOL "Don't build dotnet bindings" FORCE)
    set(Z3_BUILD_JAVA_BINDINGS OFF CACHE BOOL "Don't build java bindings" FORCE)
    set(Z3_INCLUDE_GIT_HASH OFF CACHE BOOL "Don't include git hash" FORCE)
    set(Z3_INCLUDE_GIT_DESCRIBE OFF CACHE BOOL "Don't include git describe" FORCE)

    add_subdirectory(z3)

    # Set variables for SVF linking
    set(Z3_LIBRARIES libz3)
    set(Z3_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/z3/src/api" "${CMAKE_CURRENT_BINARY_DIR}/z3/src/api" "${CMAKE_CURRENT_SOURCE_DIR}/z3/src/api/c++")
    
    # Ensure Z3 includes are propagated
    include_directories(${Z3_INCLUDE_DIRS})
elseif (LLVM_Z3_FOUND OR LLVM_ENABLE_Z3_SOLVER)
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
