#include <stdint.h>
#include <stddef.h>
#include "fork.h"
#include "vmm.h"
#include "pmm.h"
#include "thread.h"
#include "heap.h"
#include "kprintf.h"
#include "syscall.h"

typedef struct fork_child_ctx {
    uint64_t r15;       /* offset   0 */
    uint64_t r14;       /* offset   8 */
    uint64_t r13;       /* offset  16 */
    uint64_t r12;       /* offset  24 */
    uint64_t r11;       /* offset  32 */
    uint64_t r10;       /* offset  40 */
    uint64_t r9;        /* offset  48 */
    uint64_t r8;        /* offset  56 */
    uint64_t rsi;       /* offset  64 */
    uint64_t rdi;       /* offset  72 */
    uint64_t rbp;       /* offset  80 */
    uint64_t rdx;       /* offset  88 */
    uint64_t rcx;       /* offset  96 */
    uint64_t rbx;       /* offset 104 */
    uint64_t rip;       /* offset 112 */
    uint64_t cs;        /* offset 120 */
    uint64_t rflags;    /* offset 128 */
    uint64_t user_rsp;  /* offset 136 */
    uint64_t user_ss;   /* offset 144 */
    uint64_t child_cr3; /* offset 152 */
} fork_child_ctx_t;

extern void fork_enter_user(const fork_child_ctx_t *fc);

static uint64_t clone_pagemap(uint64_t src_cr3) {
    uint64_t dst_cr3 = vmm_create_user_pagemap();
    if (!dst_cr3) return 0;

    uint64_t *src_pml4 = (uint64_t *)pmm_phys_to_virt(src_cr3);

    /* Walk lower half only: PML4 entries 0-255 */
    for (int i1 = 0; i1 < 256; i1++) {
        if (!(src_pml4[i1] & 1)) continue;
        uint64_t pdpt_phys = src_pml4[i1] & 0x000FFFFFFFFFF000ULL;
        uint64_t *src_pdpt = (uint64_t *)pmm_phys_to_virt(pdpt_phys);

        for (int i2 = 0; i2 < 512; i2++) {
            if (!(src_pdpt[i2] & 1)) continue;
            uint64_t pd_phys = src_pdpt[i2] & 0x000FFFFFFFFFF000ULL;
            uint64_t *src_pd = (uint64_t *)pmm_phys_to_virt(pd_phys);

            for (int i3 = 0; i3 < 512; i3++) {
                if (!(src_pd[i3] & 1)) continue;
                uint64_t pt_phys = src_pd[i3] & 0x000FFFFFFFFFF000ULL;
                uint64_t *src_pt = (uint64_t *)pmm_phys_to_virt(pt_phys);

                for (int i4 = 0; i4 < 512; i4++) {
                    uint64_t pte = src_pt[i4];
                    if (!(pte & 1)) continue;

                    uint64_t src_phys = pte & 0x000FFFFFFFFFF000ULL;

                    uint64_t new_phys = pmm_alloc_page();
                    if (!new_phys) goto fail;

                    /* Copy page content via HHDM */
                    uint8_t *src_page = (uint8_t *)pmm_phys_to_virt(src_phys);
                    uint8_t *dst_page = (uint8_t *)pmm_phys_to_virt(new_phys);
                    for (int b = 0; b < 4096; b++) {
                        dst_page[b] = src_page[b];
                    }

                    /* Reconstruct virtual address */
                    uint64_t va = ((uint64_t)i1 << 39) |
                                  ((uint64_t)i2 << 30) |
                                  ((uint64_t)i3 << 21) |
                                  ((uint64_t)i4 << 12);

                    /* Preserve flags from source PTE */
                    vmm_flags_t flags = 0;
                    if (pte & VMM_USER)  flags |= VMM_USER;
                    if (pte & VMM_WRITE) flags |= VMM_WRITE;
                    if (pte & VMM_NX)    flags |= VMM_NX;

                    if (!vmm_map_page_into(dst_cr3, va, new_phys, flags)) {
                        pmm_free_page(new_phys);
                        goto fail;
                    }
                }
            }
        }
    }

    return dst_cr3;

fail:
    vmm_destroy_user_pagemap(dst_cr3);
    return 0;
}

static void fork_child_fn(void *arg) {
    fork_child_ctx_t *fc_ptr = (fork_child_ctx_t *)arg;
    fork_child_ctx_t local_fc = *fc_ptr;
    kfree(fc_ptr);

    g_cur_set_cr3(local_fc.child_cr3);
    vmm_switch_to(local_fc.child_cr3);
    fork_enter_user(&local_fc);
    /* never returns */
}

long do_fork(isr_ctx_t *ctx) {
    uint64_t parent_cr3 = g_cur_cr3();
    if (!parent_cr3) return -1;

    uint64_t user_rsp = ((uint64_t *)(ctx + 1))[0];
    uint64_t user_ss  = ((uint64_t *)(ctx + 1))[1];

    uint64_t child_cr3 = clone_pagemap(parent_cr3);
    if (!child_cr3) return -1;

    fork_child_ctx_t *fc = (fork_child_ctx_t *)kmalloc(sizeof(fork_child_ctx_t));
    if (!fc) {
        vmm_destroy_user_pagemap(child_cr3);
        return -1;
    }

    fc->r15      = ctx->r15;
    fc->r14      = ctx->r14;
    fc->r13      = ctx->r13;
    fc->r12      = ctx->r12;
    fc->r11      = ctx->r11;
    fc->r10      = ctx->r10;
    fc->r9       = ctx->r9;
    fc->r8       = ctx->r8;
    fc->rsi      = ctx->rsi;
    fc->rdi      = ctx->rdi;
    fc->rbp      = ctx->rbp;
    fc->rdx      = ctx->rdx;
    fc->rcx      = ctx->rcx;
    fc->rbx      = ctx->rbx;
    fc->rip      = ctx->rip;
    fc->cs       = ctx->cs;
    fc->rflags   = ctx->rflags;
    fc->user_rsp = user_rsp;
    fc->user_ss  = user_ss;
    fc->child_cr3 = child_cr3;

    int child_slot = thread_create("fork_child", fork_child_fn, fc);
    if (child_slot < 0) {
        kfree(fc);
        vmm_destroy_user_pagemap(child_cr3);
        return -1;
    }

    kprintf("[fork] parent=%d child=%d\n", (int)thread_current_tid(), child_slot);
    return (long)child_slot;
}
