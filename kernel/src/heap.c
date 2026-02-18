#include <stddef.h>
#include <stdint.h>

#include "heap.h"
#include "pmm.h"
#include "kprintf.h"

#define PAGE_SIZE 4096ULL

static uint8_t *cur = 0;
static size_t   off = 0;

static inline uintptr_t align_up_uintptr(uintptr_t x, uintptr_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

static void memzero(void *p, size_t n) {
    volatile uint8_t *b = (volatile uint8_t*)p;
    for (size_t i = 0; i < n; i++) b[i] = 0;
}

void heap_init(void) {
    uint64_t phys = pmm_alloc_page();
    void *virt = pmm_phys_to_virt(phys);

    cur = (uint8_t*)virt;
    off = 0;

    kprintf("Heap init: phys=%p virt=%p\n", (void*)phys, virt);
}

void *kmalloc_aligned(size_t size, size_t align) {
    if (align < 16) align = 16;
    if (!cur) return 0;

    /* big alloc: just grab full pages */
    if (size >= PAGE_SIZE) {
        size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t phys = pmm_alloc_pages(pages);
        if (!phys) return 0;
        return pmm_phys_to_virt(phys);
    }

    uintptr_t base = (uintptr_t)cur;
    uintptr_t ptr = align_up_uintptr(base + off, align);
    size_t new_off = (size_t)((ptr - base) + size);

    /* need new page if this one is full */
    if (new_off > PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return 0;
        cur = (uint8_t*)pmm_phys_to_virt(phys);
        off = 0;

        base = (uintptr_t)cur;
        ptr = align_up_uintptr(base + off, align);
        new_off = (size_t)((ptr - base) + size);

        if (new_off > PAGE_SIZE) return 0; /* should not happen for small allocs */
    }

    off = new_off;
    return (void*)ptr;
}

void *kmalloc(size_t size) {
    return kmalloc_aligned(size, 16);
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) memzero(p, size);
    return p;
}
