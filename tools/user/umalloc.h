#pragma once
/*
 * umalloc.h — single-header malloc/free/realloc/calloc for FiFi OS userland.
 *
 * Built on sys_sbrk(). Include once in exactly one .c file that also
 * #includes "usys.h" before this header, or include usys.h first.
 *
 * Algorithm: explicit free list, first-fit, with immediate coalescing of
 * adjacent free blocks. 16-byte aligned allocations.
 *
 * Block layout (in memory, low → high):
 *   [ um_hdr_t | ... payload ... ]
 *
 * The free list is singly-linked through um_hdr_t::next.
 */

#include <stddef.h>
#include <stdint.h>

#define UM_ALIGN      16UL
#define UM_HDR_SZ     (sizeof(um_hdr_t))
#define UM_MIN_CHUNK  (4096UL)   /* minimum sbrk growth */

typedef struct um_hdr {
    size_t        size;   /* payload bytes (not including header) */
    int           free;   /* 1 = on free list */
    struct um_hdr *next;  /* next block in address order (free or not) */
} um_hdr_t;

static um_hdr_t *um_head = (um_hdr_t*)0;  /* first block ever allocated */

static inline size_t um_align(size_t n) {
    return (n + UM_ALIGN - 1) & ~(UM_ALIGN - 1);
}

/* Try to coalesce block b with its successor if both are free. */
static inline void um_coalesce(um_hdr_t *b) {
    while (b->next && b->free && b->next->free) {
        b->size += UM_HDR_SZ + b->next->size;
        b->next  = b->next->next;
    }
}

void *malloc(size_t n) {
    if (n == 0) n = 1;
    n = um_align(n);

    /* Search free list for first fit */
    um_hdr_t *prev = (um_hdr_t*)0;
    um_hdr_t *cur  = um_head;
    while (cur) {
        if (cur->free && cur->size >= n) {
            /* Split if remainder is large enough to be useful */
            if (cur->size >= n + UM_HDR_SZ + UM_ALIGN) {
                um_hdr_t *split = (um_hdr_t*)((char*)cur + UM_HDR_SZ + n);
                split->size = cur->size - n - UM_HDR_SZ;
                split->free = 1;
                split->next = cur->next;
                cur->next   = split;
                cur->size   = n;
            }
            cur->free = 0;
            (void)prev;
            return (char*)cur + UM_HDR_SZ;
        }
        prev = cur;
        cur  = cur->next;
    }

    /* No fit — grow heap */
    size_t grow = n + UM_HDR_SZ;
    if (grow < UM_MIN_CHUNK) grow = UM_MIN_CHUNK;

    um_hdr_t *blk = (um_hdr_t*)sys_sbrk(grow);
    if (blk == (um_hdr_t*)-1UL) return (void*)0;

    blk->size = grow - UM_HDR_SZ;
    blk->free = 0;
    blk->next = (um_hdr_t*)0;

    /* Link into list */
    if (!um_head) {
        um_head = blk;
    } else {
        /* Walk to tail */
        um_hdr_t *t = um_head;
        while (t->next) t = t->next;
        t->next = blk;
    }

    /* If the new block is bigger than needed, split */
    if (blk->size >= n + UM_HDR_SZ + UM_ALIGN) {
        um_hdr_t *split = (um_hdr_t*)((char*)blk + UM_HDR_SZ + n);
        split->size = blk->size - n - UM_HDR_SZ;
        split->free = 1;
        split->next = blk->next;
        blk->next   = split;
        blk->size   = n;
    }

    return (char*)blk + UM_HDR_SZ;
}

void free(void *ptr) {
    if (!ptr) return;
    um_hdr_t *b = (um_hdr_t*)((char*)ptr - UM_HDR_SZ);
    b->free = 1;
    /* Coalesce forward */
    um_coalesce(b);
    /* Coalesce backward: walk from head to find predecessor */
    um_hdr_t *p = um_head;
    while (p && p->next != b) p = p->next;
    if (p && p->free) um_coalesce(p);
}

void *calloc(size_t nmemb, size_t sz) {
    size_t total = nmemb * sz;
    void *p = malloc(total);
    if (!p) return (void*)0;
    volatile uint8_t *d = (volatile uint8_t*)p;
    for (size_t i = 0; i < total; i++) d[i] = 0;
    return p;
}

void *realloc(void *ptr, size_t n) {
    if (!ptr) return malloc(n);
    if (n == 0) { free(ptr); return (void*)0; }

    um_hdr_t *b = (um_hdr_t*)((char*)ptr - UM_HDR_SZ);
    n = um_align(n);

    if (b->size >= n) return ptr;  /* already big enough */

    void *new_ptr = malloc(n);
    if (!new_ptr) return (void*)0;

    /* copy old payload */
    size_t copy = b->size < n ? b->size : n;
    volatile uint8_t *src = (volatile uint8_t*)ptr;
    volatile uint8_t *dst = (volatile uint8_t*)new_ptr;
    for (size_t i = 0; i < copy; i++) dst[i] = src[i];

    free(ptr);
    return new_ptr;
}
