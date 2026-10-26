#define SZ_AVOID_LIBC 1
#define SZ_DEBUG 0

#include "stringzilla/find.h"

__attribute__((export_name("find_boundary")))
int find_boundary(unsigned distance_from_memory_end, unsigned match_distance_from_memory_end,
                  unsigned reported_length) {
    unsigned memory_end = __builtin_wasm_memory_size(0) * 65536u;
    volatile char *haystack = (volatile char *)(memory_end - distance_from_memory_end);
    char needle = 'X';
    unsigned index;

    for (index = 0; index != distance_from_memory_end; ++index) haystack[index] = 'a';
    haystack[distance_from_memory_end - match_distance_from_memory_end] = needle;

    return sz_find_byte_v128((sz_cptr_t)haystack, (sz_size_t)reported_length, &needle) ==
           (sz_cptr_t)&haystack[distance_from_memory_end - match_distance_from_memory_end];
}
