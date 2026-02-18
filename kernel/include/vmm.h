#pragma once
#include <stdint.h>
#include <stdbool.h>

void vmm_init(uint64_t hhdm_offset);

/* Map 4KiB page: virt -> phys. flags are x86_64 PTE bits (like RW). */
bool vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/* Translate a virtual address to physical. Returns 0 if unmapped. */
uint64_t vmm_virt_to_phys(uint64_t virt);
