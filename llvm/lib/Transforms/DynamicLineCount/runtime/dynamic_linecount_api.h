//===-- dynamic_linecount_api.h - C API for DynamicLineCount --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This header provides a plain C API for the DynamicLineCount runtime.
// It can be used when linking with the runtime from languages other than C++.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_DYNAMICLINECOUNT_RUNTIME_API_H
#define LLVM_TRANSFORMS_DYNAMICLINECOUNT_RUNTIME_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Registers an unsafe line of code in the coverage database.
 * This function is typically called during program initialization
 * to register all unsafe lines that exist in the program.
 *
 * @param line_num The line number in the source file
 * @param file_path The source file path
 */
void dynamic_linecount_register_unsafe_line(int64_t line_num, const char *file_path);

/**
 * Marks an unsafe line as executed at runtime.
 * This function is called when an unsafe line is executed during program run.
 *
 * @param line_num The line number in the source file
 * @param file_path The source file path
 */
void dynamic_linecount_mark_executed(int64_t line_num, const char *file_path);

/**
 * Records an unsafe block with a specific size (number of instructions).
 * This function is used by the InstMarker pass to track blocks of unsafe code.
 *
 * @param block_size The number of instructions in this unsafe block
 */
void dynamic_linecount_track_block(int64_t block_size);

/**
 * Prints the coverage statistics to stdout.
 * This function is automatically called at program exit,
 * but can also be called manually if needed.
 */
void dynamic_linecount_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif // LLVM_TRANSFORMS_DYNAMICLINECOUNT_RUNTIME_API_H