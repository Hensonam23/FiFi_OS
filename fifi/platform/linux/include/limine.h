#ifndef LIMINE_H
#define LIMINE_H 1
#include <stdint.h>

/* Minimal stubs for Linux userspace — shadows the real limine.h.
 * Uses the same guard (LIMINE_H) so kernel/include/limine.h is skipped
 * when pmm.h includes it from its own directory. */

struct limine_framebuffer {
    void     *address;
    uint64_t  width;
    uint64_t  height;
    uint64_t  pitch;    /* bytes per row */
    uint16_t  bpp;
};

struct limine_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct limine_memmap_response {
    uint64_t entry_count;
    struct limine_memmap_entry **entries;
};

#endif /* LIMINE_H */
