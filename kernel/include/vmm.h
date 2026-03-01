#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
  Step 10 VMM API

  Flags are "VMM_*" and map to common x86_64 meanings:
  - VMM_WRITE: writable mapping
  - VMM_USER: user-accessible (later)
  - VMM_NX:   no-execute (later)
*/

typedef uint64_t vmm_flags_t;

enum {
    VMM_WRITE = (1ULL << 1),
    VMM_USER  = (1ULL << 2),
    VMM_NX    = (1ULL << 63),
};

void vmm_init(uint64_t hhdm_offset);

bool vmm_map_page(uint64_t virt, uint64_t phys, vmm_flags_t flags);
bool vmm_unmap_page(uint64_t virt);

uint64_t vmm_translate(uint64_t virt);

/* legacy name kept for compatibility */
uint64_t vmm_virt_to_phys(uint64_t virt);

void vmm_invlpg(uint64_t virt);
void vmm_flush_tlb(void);

bool vmm_map_range(uint64_t virt, uint64_t phys, size_t size, vmm_flags_t flags);
bool vmm_unmap_range(uint64_t virt, size_t size);

// Check that [virt, virt+size) is mapped user-accessible (and writable if write=true)
bool vmm_user_accessible(uint64_t virt, size_t size, bool write);
