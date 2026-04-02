#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"

#define PAGE_SIZE 4096ULL

/* x86_64 page entry flags */
#define PTE_P   (1ULL << 0)   /* present */
#define PTE_RW  (1ULL << 1)   /* writable */
#define PTE_US  (1ULL << 2)   /* user */
#define PTE_NX  (1ULL << 63)  /* no-execute (if supported) */

static inline void *phys_to_virt(uint64_t phys) {
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

static inline uint64_t align_up(uint64_t x) {
    return (x + 0xFFFULL) & ~0xFFFULL;
}

static inline uint16_t idx_pml4(uint64_t v) { return (v >> 39) & 0x1FF; }
static inline uint16_t idx_pdpt(uint64_t v) { return (v >> 30) & 0x1FF; }
static inline uint16_t idx_pd  (uint64_t v) { return (v >> 21) & 0x1FF; }
static inline uint16_t idx_pt  (uint64_t v) { return (v >> 12) & 0x1FF; }
static uint64_t *ensure_table(uint64_t *parent, uint16_t index, bool user) {
    uint64_t e = parent[index];
    if ((e & PTE_P) && user) { parent[index] |= PTE_US; }


    if (e & PTE_P) {
        uint64_t child_phys = e & ~0xFFFULL;
        return (uint64_t*)phys_to_virt(child_phys);
    }

    uint64_t new_phys = pmm_alloc_page();
    if (!new_phys) return 0;

    uint64_t *child = (uint64_t*)phys_to_virt(new_phys);

    /* zero page table */
    for (int i = 0; i < 512; i++) child[i] = 0;

    parent[index] = (new_phys & ~0xFFFULL) | PTE_P | PTE_RW | (user ? PTE_US : 0);
    return child;
}

static inline uint64_t make_pte_flags(vmm_flags_t flags) {
    uint64_t f = PTE_P; /* map_page always creates a present mapping */

    if (flags & VMM_WRITE) f |= PTE_RW;
    if (flags & VMM_USER)  f |= PTE_US;
    if (flags & VMM_NX)    f |= PTE_NX;

    return f;
}

/* Kernel's permanent PML4 physical address — shared across all process page maps */
static uint64_t g_kernel_cr3 = 0;

void vmm_init(uint64_t hhdm_offset) {
    (void)hhdm_offset;

    uint64_t pml4_phys = read_cr3_phys();
    g_kernel_cr3 = pml4_phys;   /* save kernel CR3 before any user maps exist */
    kprintf("VMM init: CR3=%p\n", (void*)pml4_phys);

#ifdef FIFI_VMM_API_TEST
    kprintf("VMM test: starting API self-test...\n");

    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        kprintf("VMM test: pmm_alloc_page failed\n");
    } else {
        uint64_t test_virt = 0xffff900000000000ULL;

        uint64_t before = vmm_translate(test_virt);
        kprintf("VMM test: translate(before)=%p\n", (void*)before);

        if (before != 0) {
            kprintf("VMM test: warning: test_virt already mapped\n");
        } else if (!vmm_map_page(test_virt, phys, VMM_WRITE)) {
            kprintf("VMM test: vmm_map_page failed\n");
        } else {
            volatile uint64_t *v = (volatile uint64_t*)test_virt;
            *v = 0x1122334455667788ULL;

            uint64_t got_phys = vmm_translate(test_virt);
            kprintf("VMM test: translate(after)=%p expected=%p\n", (void*)got_phys, (void*)phys);

            volatile uint64_t *h = (volatile uint64_t*)pmm_phys_to_virt(phys);
            kprintf("VMM test: read via HHDM=%p read via virt=%p\n", (void*)*h, (void*)*v);

            if (!vmm_unmap_page(test_virt)) {
                kprintf("VMM test: vmm_unmap_page failed\n");
            } else {
                uint64_t after = vmm_translate(test_virt);
                kprintf("VMM test: translate(unmapped)=%p (expected 0)\n", (void*)after);
            }
        }
    }
    kprintf("VMM test: done\n");
#endif

}

bool vmm_map_page(uint64_t virt, uint64_t phys, vmm_flags_t flags) {
    bool user = (flags & VMM_USER) != 0;
    virt = align_down(virt);
    phys = align_down(phys);

    uint64_t pml4_phys = read_cr3_phys();
    uint64_t *pml4 = (uint64_t*)phys_to_virt(pml4_phys);

    uint64_t *pdpt = ensure_table(pml4, idx_pml4(virt), user);
    if (!pdpt) return false;

    uint64_t *pd = ensure_table(pdpt, idx_pdpt(virt), user);
    if (!pd) return false;

    uint64_t *pt = ensure_table(pd, idx_pd(virt), user);
    if (!pt) return false;

    uint16_t i = idx_pt(virt);
    pt[i] = (phys & ~0xFFFULL) | make_pte_flags(flags);

    invlpg(virt);
    return true;
}

static int table_is_empty(uint64_t *tbl) {
    for (int i = 0; i < 512; i++) {
        if (tbl[i] & PTE_P) return 0;
    }
    return 1;
}

bool vmm_unmap_page(uint64_t virt) {
    uint64_t v = align_down(virt);

    uint64_t pml4_phys = read_cr3_phys();
    uint64_t *pml4 = (uint64_t*)phys_to_virt(pml4_phys);

    uint16_t i1 = idx_pml4(v);
    uint64_t e1 = pml4[i1];
    if (!(e1 & PTE_P)) return false;

    uint64_t pdpt_phys = e1 & 0x000FFFFFFFFFF000ULL;
    uint64_t *pdpt = (uint64_t*)phys_to_virt(pdpt_phys);

    uint16_t i2 = idx_pdpt(v);
    uint64_t e2 = pdpt[i2];
    if (!(e2 & PTE_P)) return false;

    uint64_t pd_phys = e2 & 0x000FFFFFFFFFF000ULL;
    uint64_t *pd = (uint64_t*)phys_to_virt(pd_phys);

    uint16_t i3 = idx_pd(v);
    uint64_t e3 = pd[i3];
    if (!(e3 & PTE_P)) return false;

    uint64_t pt_phys = e3 & 0x000FFFFFFFFFF000ULL;
    uint64_t *pt = (uint64_t*)phys_to_virt(pt_phys);

    uint16_t i4 = idx_pt(v);
    if (!(pt[i4] & PTE_P)) return false;

    pt[i4] = 0;
    invlpg(v);

    if (!table_is_empty(pt)) return true;

    pd[i3] = 0;
    pmm_free_page(pt_phys);

    if (!table_is_empty(pd)) return true;

    pdpt[i2] = 0;
    pmm_free_page(pd_phys);

    if (!table_is_empty(pdpt)) return true;

    pml4[i1] = 0;
    pmm_free_page(pdpt_phys);
    vmm_flush_tlb();
    return true;
}


bool vmm_unmap_page_and_free(uint64_t virt) {
    uint64_t v = align_down(virt);
    uint64_t phys = vmm_translate(v);
    if (!phys) return true;

    phys &= 0x000FFFFFFFFFF000ULL;
    if (!vmm_unmap_page(v)) return false;

    pmm_free_page(phys);
    return true;
}

uint64_t vmm_translate(uint64_t virt) {
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

    return (e4 & 0x000FFFFFFFFFF000ULL) | (virt & 0xFFFULL);
}


static bool vmm_user_page_ok(uint64_t virt, bool write) {
    uint64_t pml4_phys = read_cr3_phys();
    uint64_t *pml4 = (uint64_t*)phys_to_virt(pml4_phys);

    uint64_t e1 = pml4[idx_pml4(virt)];
    if (!(e1 & PTE_P)) return false;
    if (!(e1 & PTE_US)) return false;
    if (write && !(e1 & PTE_RW)) return false;

    uint64_t *pdpt = (uint64_t*)phys_to_virt(e1 & ~0xFFFULL);
    uint64_t e2 = pdpt[idx_pdpt(virt)];
    if (!(e2 & PTE_P)) return false;
    if (!(e2 & PTE_US)) return false;
    if (write && !(e2 & PTE_RW)) return false;

    uint64_t *pd = (uint64_t*)phys_to_virt(e2 & ~0xFFFULL);
    uint64_t e3 = pd[idx_pd(virt)];
    if (!(e3 & PTE_P)) return false;
    if (!(e3 & PTE_US)) return false;
    if (write && !(e3 & PTE_RW)) return false;

    uint64_t *pt = (uint64_t*)phys_to_virt(e3 & ~0xFFFULL);
    uint64_t e4 = pt[idx_pt(virt)];
    if (!(e4 & PTE_P)) return false;
    if (!(e4 & PTE_US)) return false;
    if (write && !(e4 & PTE_RW)) return false;

    return true;
}

bool vmm_user_accessible(uint64_t virt, size_t size, bool write) {
    if (size == 0) return true;
    uint64_t end = virt + (uint64_t)size;
    if (end < virt) return false; // overflow

    uint64_t v0 = align_down(virt);
    for (uint64_t v = v0; v < end; v += PAGE_SIZE) {
        if (!vmm_user_page_ok(v, write)) return false;
    }
    return true;
}

/* legacy name kept for compatibility */
uint64_t vmm_virt_to_phys(uint64_t virt) {
    return vmm_translate(virt);
}

void vmm_invlpg(uint64_t virt) {
    __asm__ volatile ("invlpg (%0)" :: "r"((void*)virt) : "memory");
}

void vmm_flush_tlb(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

bool vmm_map_range(uint64_t virt, uint64_t phys, size_t size, vmm_flags_t flags) {
    uint64_t v0 = align_down(virt);
    uint64_t p0 = align_down(phys);

    uint64_t lead = virt - v0;
    uint64_t bytes = align_up((uint64_t)size + lead);

    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
        if (!vmm_map_page(v0 + off, p0 + off, flags)) {
            return false;
        }
    }
    return true;
}

bool vmm_unmap_range(uint64_t virt, size_t size) {
    uint64_t v0 = align_down(virt);

    uint64_t lead = virt - v0;
    uint64_t bytes = align_up((uint64_t)size + lead);

    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
        (void)vmm_unmap_page(v0 + off);
    }
    return true;
}

bool vmm_unmap_range_and_free(uint64_t virt, size_t size) {
    if (size == 0) return true;

    uint64_t v0 = align_down(virt);
    uint64_t lead = virt - v0;
    uint64_t bytes = align_up((uint64_t)size + lead);

    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
        uint64_t cur = v0 + off;

        if (!vmm_unmap_page_and_free(cur)) {
            return false;
        }
    }
    return true;
}

/* ── Per-process address space management ─────────────────────────────────────
 *
 * Each user task gets its own PML4 (page table root, loaded into CR3).
 * Kernel higher-half mappings (PML4 entries 256-511) are shared across all
 * page maps so the kernel stays accessible from every process.
 *
 * User lower-half (entries 0-255) is private per process.
 */


void vmm_set_kernel_cr3(uint64_t cr3_phys) {
    g_kernel_cr3 = cr3_phys;
}

uint64_t vmm_get_kernel_cr3(void) {
    return g_kernel_cr3;
}

/*
 * vmm_create_user_pagemap()
 * Allocates a fresh PML4 for a user task.
 * Copies kernel higher-half entries (256-511) so the kernel is reachable.
 * Returns physical address of new PML4, or 0 on failure.
 */
uint64_t vmm_create_user_pagemap(void) {
    uint64_t new_pml4_phys = pmm_alloc_page();
    if (!new_pml4_phys) return 0;

    uint64_t *new_pml4 = (uint64_t*)phys_to_virt(new_pml4_phys);
    /* Zero the whole page first */
    for (int i = 0; i < 512; i++) new_pml4[i] = 0;

    if (!g_kernel_cr3) {
        /* Fallback: read current CR3 as kernel CR3 */
        g_kernel_cr3 = read_cr3_phys();
    }

    uint64_t *kernel_pml4 = (uint64_t*)phys_to_virt(g_kernel_cr3);

    /* Share kernel higher-half entries (256-511) — do NOT deep copy, just copy
     * the PML4 slot pointers so all processes see the same kernel mappings. */
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = kernel_pml4[i];
    }

    return new_pml4_phys;
}

/*
 * vmm_destroy_user_pagemap()
 * Tears down a user process's page tables and frees all physical pages.
 * Walks lower-half PML4 entries (0-255) only — never touches kernel half.
 * Also frees the page table structure pages themselves (PT, PD, PDPT, PML4).
 */
void vmm_destroy_user_pagemap(uint64_t cr3_phys) {
    if (!cr3_phys) return;
    if (cr3_phys == g_kernel_cr3) return; /* safety: never destroy kernel map */

    uint64_t *pml4 = (uint64_t*)phys_to_virt(cr3_phys);

    for (int i1 = 0; i1 < 256; i1++) {     /* lower half only */
        if (!(pml4[i1] & PTE_P)) continue;
        uint64_t pdpt_phys = pml4[i1] & ~0xFFFULL;
        uint64_t *pdpt = (uint64_t*)phys_to_virt(pdpt_phys);

        for (int i2 = 0; i2 < 512; i2++) {
            if (!(pdpt[i2] & PTE_P)) continue;
            uint64_t pd_phys = pdpt[i2] & ~0xFFFULL;
            uint64_t *pd = (uint64_t*)phys_to_virt(pd_phys);

            for (int i3 = 0; i3 < 512; i3++) {
                if (!(pd[i3] & PTE_P)) continue;
                uint64_t pt_phys = pd[i3] & ~0xFFFULL;
                uint64_t *pt = (uint64_t*)phys_to_virt(pt_phys);

                /* Free every mapped page in this PT */
                for (int i4 = 0; i4 < 512; i4++) {
                    if (!(pt[i4] & PTE_P)) continue;
                    uint64_t page_phys = pt[i4] & ~0xFFFULL;
                    pmm_free_page(page_phys);
                    pt[i4] = 0;
                }

                pmm_free_page(pt_phys);   /* free the PT page itself */
                pd[i3] = 0;
            }

            pmm_free_page(pd_phys);       /* free the PD page itself */
            pdpt[i2] = 0;
        }

        pmm_free_page(pdpt_phys);         /* free the PDPT page itself */
        pml4[i1] = 0;
    }

    pmm_free_page(cr3_phys);              /* free the PML4 page itself */
}

/*
 * vmm_switch_to(cr3_phys)
 * Loads a page map into CR3. Pass 0 to switch back to the kernel map.
 */
void vmm_switch_to(uint64_t cr3_phys) {
    if (!cr3_phys) cr3_phys = g_kernel_cr3;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3_phys) : "memory");
}

/*
 * vmm_map_page_into(cr3_phys, virt, phys, flags)
 * Maps a single page into a specific page map (identified by its CR3).
 * Uses the HHDM to walk/create page tables without switching CR3.
 */
bool vmm_map_page_into(uint64_t cr3_phys, uint64_t virt, uint64_t phys, vmm_flags_t flags) {
    bool user = (flags & VMM_USER) != 0;
    uint64_t *pml4 = (uint64_t*)phys_to_virt(cr3_phys);

    uint64_t *pdpt = ensure_table(pml4, idx_pml4(virt), user);
    if (!pdpt) return false;
    uint64_t *pd   = ensure_table(pdpt, idx_pdpt(virt), user);
    if (!pd) return false;
    uint64_t *pt   = ensure_table(pd,   idx_pd(virt),   user);
    if (!pt) return false;

    pt[idx_pt(virt)] = (phys & ~0xFFFULL) | make_pte_flags(flags);
    return true;
}
