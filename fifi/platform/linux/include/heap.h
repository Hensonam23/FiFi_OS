#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Shadows kernel/include/heap.h — maps kernel heap API to glibc malloc */

static inline void     heap_init(void)                    { }
static inline void    *kmalloc(size_t sz)                 { return malloc(sz); }
static inline void    *kzalloc(size_t sz)                 { return calloc(1, sz); }
static inline void     kfree(void *p)                     { free(p); }
static inline void    *kmalloc_aligned(size_t sz, size_t align) {
    void *p = NULL;
    size_t a = align < sizeof(void *) ? sizeof(void *) : align;
    return posix_memalign(&p, a, sz) == 0 ? p : NULL;
}
static inline void    *heap_get_cur_page(void)  { return NULL; }
static inline uint64_t heap_get_offset(void)    { return 0; }
