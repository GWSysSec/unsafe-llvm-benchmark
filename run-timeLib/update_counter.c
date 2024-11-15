// update_counter.c
#include <stdio.h>

static long instruction_count = 0;

void update_counter(long count) {
    instruction_count += count;
}

void print_inline_count() {
    printf("Total Unsafe Instructions Executed Inside Markers: %ld\n", instruction_count);
}
