LLVM_SRC_DIR ?= $(abspath ../../..)
REPO_ROOT ?= $(abspath ../../../../../../)
LLVM_BUILD_DIR ?= $(REPO_ROOT)/build/x86_64-unknown-linux-gnu/llvm
DEFAULT_LLVM_CONFIG := $(LLVM_BUILD_DIR)/bin/llvm-config
LLVM_CONFIG ?= $(if $(wildcard $(DEFAULT_LLVM_CONFIG)),$(DEFAULT_LLVM_CONFIG),llvm-config)
SVF_SRC_DIR ?= $(LLVM_SRC_DIR)/lib/SVF
CXX := $(or $(shell command -v clang++ 2>/dev/null),c++)

SRCS := UnsafeHeapAllocAnalysis.cpp UnsafeHeapInstrumentation.cpp

LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags)
LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags --libs core support analysis transformutils)
SVF_GEN_INCLUDE := $(firstword \
  $(foreach dir,$(LLVM_BUILD_DIR)/lib/SVF/include $(LLVM_BUILD_DIR)/build/lib/SVF/include,\
    $(if $(wildcard $(dir)/Util/config.h),$(dir))))

INCLUDES := \
  -I$(LLVM_SRC_DIR)/include \
  -I$(LLVM_BUILD_DIR)/include \
  -I$(SVF_SRC_DIR)/svf/include \
  -I$(SVF_SRC_DIR)/svf-llvm/include

ifneq ($(SVF_GEN_INCLUDE),)
INCLUDES += -I$(SVF_GEN_INCLUDE)
endif

CXXFLAGS := $(LLVM_CXXFLAGS) $(INCLUDES)
COMPILE_BASE := $(CXX) $(CXXFLAGS)

.PHONY: all check clean print-flags

all: compile_commands.json

compile_commands.json: Makefile
	$(file >$@,[)
	$(file >>$@,  {)
	$(file >>$@,    "directory": "$(CURDIR)",)
	$(file >>$@,    "command": "$(COMPILE_BASE) -c $(CURDIR)/UnsafeHeapAllocAnalysis.cpp",)
	$(file >>$@,    "file": "$(CURDIR)/UnsafeHeapAllocAnalysis.cpp")
	$(file >>$@,  },)
	$(file >>$@,  {)
	$(file >>$@,    "directory": "$(CURDIR)",)
	$(file >>$@,    "command": "$(COMPILE_BASE) -c $(CURDIR)/UnsafeHeapInstrumentation.cpp",)
	$(file >>$@,    "file": "$(CURDIR)/UnsafeHeapInstrumentation.cpp")
	$(file >>$@,  })
	$(file >>$@,])

check:
	$(CXX) $(CXXFLAGS) -fsyntax-only UnsafeHeapAllocAnalysis.cpp
	$(CXX) $(CXXFLAGS) -fsyntax-only UnsafeHeapInstrumentation.cpp

print-flags:
	@printf '%s\n' "$(COMPILE_BASE)"
	@printf '%s\n' "$(LLVM_LDFLAGS)"

clean:
	rm -f compile_commands.json
