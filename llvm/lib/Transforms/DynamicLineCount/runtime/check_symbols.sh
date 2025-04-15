#!/bin/bash
# Script to check if the DynamicLineCountRuntime library has all the required symbols

LIBRARY_PATH="$1"
if [ -z "$LIBRARY_PATH" ]; then
  # Try to find the library in the build directory
  LIBRARY_PATH=$(find /home/oscar/Projects/unsafebench/unsafe-rust-benchmark/build -name "libLLVMDynamicLineCountRuntime.a" | head -1)
  if [ -z "$LIBRARY_PATH" ]; then
    echo "Error: Library path not provided and library not found in build directory."
    echo "Usage: $0 /path/to/libLLVMDynamicLineCountRuntime.a"
    exit 1
  fi
fi

echo "Checking symbols in $LIBRARY_PATH..."

# Check if the library exists
if [ ! -f "$LIBRARY_PATH" ]; then
  echo "Error: Library file not found: $LIBRARY_PATH"
  exit 1
fi

# List of required symbols
REQUIRED_SYMBOLS=(
  "update_unsafe_line_counter"
  "mark_unsafe_line_executed"
  "total_unsafe_block_count"
  "print_coverage_stats"
  "dynamic_linecount_register_unsafe_line"
  "dynamic_linecount_mark_executed"
  "dynamic_linecount_track_block"
  "dynamic_linecount_print_stats"
)

# Check for each required symbol
MISSING=0
for sym in "${REQUIRED_SYMBOLS[@]}"; do
  if nm "$LIBRARY_PATH" | grep -q "$sym"; then
    echo "✅ Symbol found: $sym"
  else
    echo "❌ Symbol MISSING: $sym"
    MISSING=$((MISSING+1))
  fi
done

# Also check for C++ symbols that might be causing problems
echo -e "\nChecking for C++ symbols:"
CPP_SYMBOLS=(
  "operator new"
  "operator delete"
  "std::__throw_bad_alloc"
  "std::mutex::lock"
)

for sym in "${CPP_SYMBOLS[@]}"; do
  if nm "$LIBRARY_PATH" | grep -q "$sym"; then
    echo "✅ C++ symbol found: $sym"
  else
    echo "ℹ️ C++ symbol not in library (will need to be linked separately): $sym"
  fi
done

if [ $MISSING -eq 0 ]; then
  echo -e "\n✅ All required symbols are present in the library."
else
  echo -e "\n❌ $MISSING required symbols are missing from the library."
fi

echo -e "\nTo link this library in Rust, use these flags:"
echo "-C link-arg=-L$(dirname "$LIBRARY_PATH")"
echo "-C link-arg=-Wl,--whole-archive"
echo "-l static=LLVMDynamicLineCountRuntime"
echo "-C link-arg=-Wl,--no-whole-archive"
echo "-l stdc++"
echo "-l pthread"
echo "-l m"
echo "-l dl"
echo "-l rt"