#!/bin/bash
# Script to rebuild simplified DynamicLineCount runtime

# Find important paths
BUILD_DIR="/home/oscar/Projects/unsafebench/unsafe-rust-benchmark/build/x86_64-unknown-linux-gnu/llvm/build"
SRC_DIR="/home/oscar/Projects/unsafebench/unsafe-rust-benchmark/src/llvm-project"

echo "Rebuilding DynamicLineCount runtime library..."
echo "Build directory: $BUILD_DIR"

# Backup current directory
CURRENT_DIR=$(pwd)

# Go to build directory and rebuild just the runtime
cd "$BUILD_DIR" || { echo "Error: Build directory not found!"; exit 1; }

# Rebuild just the runtime library
ninja LLVMDynamicLineCountRuntime

# Check if build was successful
if [ $? -ne 0 ]; then
  echo "Error: Failed to build LLVMDynamicLineCountRuntime!"
  exit 1
fi

echo "Build successful!"
echo
echo "To use this runtime, make sure your .cargo/config.toml contains:"
echo
echo "[target.x86_64-unknown-linux-gnu]"
echo "rustflags = ["
echo "    \"-C\", \"passes=unsafe-analysis-pass,instmarker,dynamic-line-count\","
echo "    \"-C\", \"link-arg=-L$BUILD_DIR/lib\","
echo "    \"-l\", \"static=LLVMDynamicLineCountRuntime\""
echo "]"
echo
echo "To check the symbols in the runtime library:"
echo "nm $BUILD_DIR/lib/libLLVMDynamicLineCountRuntime.a | grep \"update_unsafe_line_counter\""
echo

# Return to original directory
cd "$CURRENT_DIR"