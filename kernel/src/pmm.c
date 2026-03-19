#include <stdint.h>
#include <stddef.h>

#include "pmm.h"
#include "kprintf.h"
#include "serial.h"

static uint64_t g_pmm_total_pages = 0;
static uint64_t g_pmm_free_pages = 0;

#define PAGE_SIZE 4096ULL
#define MIN_PHYS  (16ULL * 1024 * 1024) /* skip low memory to be safe for now */

static uint64_t g_hhdm = 0;
static uint64_t g_cursor = 0;
static uint64_t g_end = 0;
static uint64_t g_free_list = 0;

static inline uint64_t align_up(uint64_t x, uint64_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

void pmm_init(struct limine_memmap_response *mm, uint64_t hhdm_offset) {
    g_hhdm = hhdm_offset;
    g_cursor = 0;
    g_end = 0;
    g_free_list = 0;

    if (!mm) {
        serial_write("FiFi OS: PMM init failed (no memmap)\n");
        return;
    }

    uint64_t best_start = 0;
    uint64_t best_len = 0;

    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (!e) continue;

        if (e->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t base = e->base;
        uint64_t end  = e->base + e->length;

        if (end <= MIN_PHYS) continue;

        uint64_t start = base;
        if (start < MIN_PHYS) start = MIN_PHYS;
        start = align_up(start, PAGE_SIZE);

        if (start + PAGE_SIZE > end) continue;

        uint64_t len = end - start;
        if (len > best_len) {
            best_len = len;
            best_start = start;
        }
    }

    if (!best_len) {
        serial_write("FiFi OS: PMM init failed (no usable region found)\n");
        return;
    }

    g_cursor = best_start;
    g_end = best_start + best_len;

    kprintf("PMM region: base=%p end=%p size=%p KiB\n",
            (void*)g_cursor,
            (void*)g_end,
            (void*)(best_len / 1024));

    // PMM stats (coarse): pages managed by the main usable region
    g_pmm_total_pages = (best_len / PAGE_SIZE);
    g_pmm_free_pages  = g_pmm_total_pages;
}


uint64_t pmm_alloc_page(void) {
    if (g_free_list) {
        uint64_t p = g_free_list;
        uint64_t *next = (uint64_t*)pmm_phys_to_virt(p);
        g_free_list = *next;
        if (g_pmm_free_pages) g_pmm_free_pages--;
        return p;
    }

    if (!g_cursor) return 0;
    if (g_cursor + PAGE_SIZE > g_end) return 0;

    uint64_t p = g_cursor;
    g_cursor += PAGE_SIZE;
    if (g_pmm_free_pages) g_pmm_free_pages--;
    return p;
}

void pmm_free_page(uint64_t phys) {
    if (!phys) return;
    if (!g_hhdm) return;

    phys &= ~(PAGE_SIZE - 1ULL);
    if (phys < MIN_PHYS) return;
    if (!g_cursor) return;
    if (phys >= g_cursor) return;

    uint64_t *node = (uint64_t*)pmm_phys_to_virt(phys);
    *node = g_free_list;
    g_free_list = phys;
    g_pmm_free_pages++;
}

uint64_t pmm_alloc_pages(size_t count) {
    if (count == 0) return 0;
    if (!g_cursor) return 0;

    uint64_t bytes = (uint64_t)count * PAGE_SIZE;
    if (g_cursor + bytes > g_end) return 0;

    uint64_t p = g_cursor;
    g_cursor += bytes;
    if (g_pmm_free_pages >= count) g_pmm_free_pages -= count;
    else g_pmm_free_pages = 0;
    return p;
}

void *pmm_phys_to_virt(uint64_t phys) {
    if (!phys) return 0;
    return (void*)(uintptr_t)(phys + g_hhdm);
}

uint64_t pmm_virt_to_phys(void *virt) {
    if (!virt) return 0;
    return (uint64_t)(uintptr_t)virt - g_hhdm;
}

uint64_t pmm_get_total_pages(void) { return g_pmm_total_pages; }
uint64_t pmm_get_free_pages(void)  { return g_pmm_free_pages; }
uint64_t pmm_get_used_pages(void)  {
    return (g_pmm_total_pages >= g_pmm_free_pages) ? (g_pmm_total_pages - g_pmm_free_pages) : 0;
}
