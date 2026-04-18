#pragma once
#include <stdint.h>
#include <stddef.h>

#include "limine.h"

void pmm_init(struct limine_memmap_response *mm, uint64_t hhdm_offset);

/* Returns physical address of a 4KiB page, or 0 on failure */
uint64_t pmm_alloc_page(void);
void pmm_free_page(uint64_t phys);

/* Allocate a page guaranteed to be below 4 GiB (for 32-bit DMA devices).
 * Draws from a separate low-memory zone initialized at PMM init.
 * Returns 0 if no low memory is available.                               */
uint64_t pmm_alloc_dma32_page(void);

/* Returns physical address of a contiguous run of pages, or 0 on failure */
uint64_t pmm_alloc_pages(size_t count);

/* Helpers using HHDM */
void *pmm_phys_to_virt(uint64_t phys);
uint64_t pmm_virt_to_phys(void *virt);

// PMM stats
uint64_t pmm_get_total_pages(void);
uint64_t pmm_get_free_pages(void);
uint64_t pmm_get_used_pages(void);
