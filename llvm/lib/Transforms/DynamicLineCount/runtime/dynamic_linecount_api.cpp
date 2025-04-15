//===-- dynamic_linecount_api.cpp - C API Implementation ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "dynamic_linecount_api.h"
#include "DynamicLineCountRuntime.h"

extern "C" {

void dynamic_linecount_register_unsafe_line(int64_t line_num, const char *file_path) {
    update_unsafe_line_counter(line_num, file_path);
}

void dynamic_linecount_mark_executed(int64_t line_num, const char *file_path) {
    mark_unsafe_line_executed(line_num, file_path);
}

void dynamic_linecount_track_block(int64_t block_size) {
    total_unsafe_block_count(block_size);
}

void dynamic_linecount_print_stats(void) {
    print_coverage_stats();
}

}