#include <stdint.h>
#include <stdbool.h>

#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"

#define PAGE_SIZE 4096ULL

/* x86_64 page entry flags */
#define PTE_P   (1ULL << 0)   /* present */
#define PTE_RW  (1ULL << 1)   /* writable */
#define PTE_US  (1ULL << 2)   /* user (not using yet) */

static uint64_t g_hhdm = 0;

static inline void *phys_to_virt(uint64_t phys) {
    /* If you already have pmm_phys_to_virt(), use that */
    return pmm_phys_to_virt(phys);
}

static inline uint64_t read_cr3_phys(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & ~0xFFFULL;
}

static inline void invlpg(uint64_t virt) {
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

static inline uint64_t align_down(uint64_t x) {
    return x & ~0xFFFULL;
}

static inline uint16_t idx_pml4(uint64_t v) { return (v >> 39) & 0x1FF; }
static inline uint16_t idx_pdpt(uint64_t v) { return (v >> 30) & 0x1FF; }
static inline uint16_t idx_pd  (uint64_t v) { return (v >> 21) & 0x1FF; }
static inline uint16_t idx_pt  (uint64_t v) { return (v >> 12) & 0x1FF; }

static uint64_t *ensure_table(uint64_t *parent, uint16_t index) {
    uint64_t e = parent[index];

    if (e & PTE_P) {
        uint64_t child_phys = e & ~0xFFFULL;
        return (uint64_t*)phys_to_virt(child_phys);
    }

    uint64_t new_phys = pmm_alloc_page();
    if (!new_phys) return 0;

    uint64_t *child = (uint64_t*)phys_to_virt(new_phys);

    /* zero page table */
    for (int i = 0; i < 512; i++) child[i] = 0;

    parent[index] = (new_phys & ~0xFFFULL) | PTE_P | PTE_RW;
    return child;
}

void vmm_init(uint64_t hhdm_offset) {
    g_hhdm = hhdm_offset;
    (void)g_hhdm;

    uint64_t pml4_phys = read_cr3_phys();
    kprintf("VMM init: CR3=%p\n", (void*)pml4_phys);
}

bool vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    virt = align_down(virt);
    phys = align_down(phys);

    uint64_t pml4_phys = read_cr3_phys();
    uint64_t *pml4 = (uint64_t*)phys_to_virt(pml4_phys);

    uint64_t *pdpt = ensure_table(pml4, idx_pml4(virt));
    if (!pdpt) return false;

    uint64_t *pd = ensure_table(pdpt, idx_pdpt(virt));
    if (!pd) return false;

    uint64_t *pt = ensure_table(pd, idx_pd(virt));
    if (!pt) return false;

    uint16_t i = idx_pt(virt);
    pt[i] = (phys & ~0xFFFULL) | PTE_P | (flags & 0xFFFULL);

    invlpg(virt);
    return true;
}

uint64_t vmm_virt_to_phys(uint64_t virt) {
    uint64_t pml4_phys = read_cr3_phys();
    uint64_t *pml4 = (uint64_t*)phys_to_virt(pml4_phys);

    uint64_t e1 = pml4[idx_pml4(virt)];
    if (!(e1 & PTE_P)) return 0;

    uint64_t *pdpt = (uint64_t*)phys_to_virt(e1 & ~0xFFFULL);
    uint64_t e2 = pdpt[idx_pdpt(virt)];
    if (!(e2 & PTE_P)) return 0;

    uint64_t *pd = (uint64_t*)phys_to_virt(e2 & ~0xFFFULL);
    uint64_t e3 = pd[idx_pd(virt)];
    if (!(e3 & PTE_P)) return 0;

    uint64_t *pt = (uint64_t*)phys_to_virt(e3 & ~0xFFFULL);
    uint64_t e4 = pt[idx_pt(virt)];
    if (!(e4 & PTE_P)) return 0;

    uint64_t phys = (e4 & ~0xFFFULL) | (virt & 0xFFFULL);
    return phys;
}
