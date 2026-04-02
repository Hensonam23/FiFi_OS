#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "exec.h"
#include "elf_types.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "thread.h"
#include "usermode.h"
#include "kprintf.h"

/* ── helpers ──────────────────────────────────────────────────────────────── */

static void memzero_e(void *dst, size_t n) {
    volatile uint8_t *p = (volatile uint8_t*)dst;
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

static void memcpy_e(void *dst, const void *src, size_t n) {
    volatile uint8_t       *d = (volatile uint8_t*)dst;
    const volatile uint8_t *s = (const volatile uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

/* Allocate and map a range of user virtual pages into the CURRENT CR3.
 * Returns 0 on success, -1 on failure (unwinds any partial allocation). */
static int map_user_pages(uint64_t va, size_t size, vmm_flags_t flags) {
    const uint64_t PAGE = 0x1000ULL;
    uint64_t start = va & ~0xFFFULL;
    uint64_t end   = (va + size + 0xFFFULL) & ~0xFFFULL;

    for (uint64_t v = start; v < end; v += PAGE) {
        (void)vmm_unmap_page_and_free(v);

        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            (void)vmm_unmap_range_and_free(start, v - start);
            return -1;
        }
        if (!vmm_map_page(v, phys, flags)) {
            pmm_free_page(phys);
            (void)vmm_unmap_range_and_free(start, v - start);
            return -1;
        }

        /* Zero page via HHDM virt address so we don't rely on user mapping yet */
        volatile uint8_t *vp = (volatile uint8_t*)(uintptr_t)v;
        for (uint64_t i = 0; i < PAGE; i++) vp[i] = 0;
    }

    thread_user_map_add(start, end - start);
    return 0;
}

/* Tighten permissions on an already-mapped range (W^X enforcement). */
static int protect_user_pages(uint64_t va, size_t size, vmm_flags_t flags) {
    const uint64_t PAGE = 0x1000ULL;
    uint64_t start = va & ~0xFFFULL;
    uint64_t end   = (va + size + 0xFFFULL) & ~0xFFFULL;

    for (uint64_t v = start; v < end; v += PAGE) {
        uint64_t phys = vmm_translate(v) & ~0xFFFULL;
        if (!phys) return -1;
        if (!vmm_map_page(v, phys, flags)) return -1;
    }
    return 0;
}

/* ── exec_load ────────────────────────────────────────────────────────────── *
 *
 * Replaces the current thread's address space with the ELF at 'path'.
 * On success: modifies ctx so iretq goes to the new program — never returns
 *             to caller via normal path.
 * On failure: returns -1, ctx unchanged, old address space still intact.
 */
int exec_load(isr_ctx_t *ctx, const char *path) {
    /* 1. Read ELF from VFS */
    const void *data = 0;
    uint64_t    size = 0;
    if (vfs_read(path, &data, &size) < 0 || !data || size < sizeof(Elf64_Ehdr)) {
        kprintf("[exec] vfs_read failed: %s\n", path);
        return -1;
    }

    const uint8_t  *buf = (const uint8_t*)data;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr*)buf;

    /* 2. Validate ELF header */
    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3 ||
        eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB ||
        eh->e_machine != EM_X86_64 || eh->e_phoff == 0 || eh->e_phnum == 0) {
        kprintf("[exec] invalid ELF: %s\n", path);
        return -1;
    }

    uint64_t ph_end = eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize;
    if (ph_end > size || eh->e_phentsize < sizeof(Elf64_Phdr)) {
        kprintf("[exec] bad phdr table\n");
        return -1;
    }

    /* 3. Tear down old address space, create fresh one */
    if (g_cur_cr3()) {
        uint64_t old_cr3 = g_cur_cr3();
        g_cur_set_cr3(0);
        vmm_switch_to(vmm_get_kernel_cr3());
        vmm_destroy_user_pagemap(old_cr3);
        thread_user_maps_zero_current();
    }

    uint64_t new_cr3 = vmm_create_user_pagemap();
    if (!new_cr3) {
        kprintf("[exec] failed to create page map\n");
        return -1;
    }
    g_cur_set_cr3(new_cr3);
    vmm_switch_to(new_cr3);

    kprintf("[exec] loading %s entry=%p\n", path, (void*)eh->e_entry);

    /* 4. Map and load PT_LOAD segments */
    const uint8_t *phbase = buf + (size_t)eh->e_phoff;
    for (unsigned i = 0; i < (unsigned)eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr*)(phbase + (size_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;

        if (ph->p_offset + ph->p_filesz > size) {
            kprintf("[exec] LOAD[%u] out of range\n", i);
            goto fail;
        }

        vmm_flags_t flags = VMM_USER | VMM_WRITE;
        if ((ph->p_flags & PF_X) == 0) flags |= VMM_NX;

        if (map_user_pages(ph->p_vaddr, ph->p_memsz, flags) < 0) {
            kprintf("[exec] LOAD[%u] map failed\n", i);
            goto fail;
        }

        memcpy_e((void*)(uintptr_t)ph->p_vaddr, buf + ph->p_offset, ph->p_filesz);

        if (ph->p_memsz > ph->p_filesz)
            memzero_e((void*)(uintptr_t)(ph->p_vaddr + ph->p_filesz),
                      ph->p_memsz - ph->p_filesz);

        /* W^X: tighten permissions */
        vmm_flags_t final = VMM_USER;
        if (ph->p_flags & PF_X) {
            /* RX — no write, no NX */
        } else {
            if (ph->p_flags & PF_W) final |= VMM_WRITE;
            final |= VMM_NX;
        }
        if (protect_user_pages(ph->p_vaddr, ph->p_memsz, final) < 0) {
            kprintf("[exec] protect failed\n");
            goto fail;
        }
    }

    /* 5. Map trampoline (RX) */
    if (map_user_pages((uint64_t)FIFI_USER_TRAMPOLINE_VA, 0x1000ULL,
                       VMM_USER | VMM_WRITE) < 0) {
        kprintf("[exec] trampoline map failed\n");
        goto fail;
    }
    for (size_t i = 0; i < sizeof(FIFI_USER_TRAMPOLINE_CODE); i++)
        ((volatile uint8_t*)(uintptr_t)FIFI_USER_TRAMPOLINE_VA)[i] =
            FIFI_USER_TRAMPOLINE_CODE[i];
    if (protect_user_pages((uint64_t)FIFI_USER_TRAMPOLINE_VA, 0x1000ULL,
                            VMM_USER) < 0) {
        kprintf("[exec] trampoline protect failed\n");
        goto fail;
    }

    /* 6. Map user stack (RW NX) */
    if (map_user_pages((uint64_t)FIFI_USER_STACK_BASE,
                       (uint64_t)(FIFI_USER_STACK_TOP - FIFI_USER_STACK_BASE),
                       VMM_USER | VMM_WRITE | VMM_NX) < 0) {
        kprintf("[exec] stack map failed\n");
        goto fail;
    }

    /* 7. Redirect iretq to the new program.
     *    isr_ctx_t ends with rflags. Above it on the stack (higher address)
     *    are the CPU-pushed user RSP and SS (because this is a ring3 syscall). */
    ctx->rip    = eh->e_entry;
    ctx->cs     = (uint64_t)FIFI_USER_CS;
    ctx->rflags = 0x202ULL;           /* IF=1 */

    /* Zero GP registers — fresh program starts clean */
    ctx->rax = 0; ctx->rbx = 0; ctx->rcx = 0; ctx->rdx = 0;
    ctx->rsi = 0; ctx->rdi = 0; ctx->rbp = 0;
    ctx->r8  = 0; ctx->r9  = 0; ctx->r10 = 0; ctx->r11 = 0;
    ctx->r12 = 0; ctx->r13 = 0; ctx->r14 = 0; ctx->r15 = 0;

    uint64_t *iret_extra = (uint64_t*)(ctx + 1); /* [0]=user rsp, [1]=user ss */
    iret_extra[0] = (uint64_t)FIFI_USER_STACK_TOP;
    iret_extra[1] = (uint64_t)FIFI_USER_DS;

    kprintf("[exec] exec OK, iretq -> %p\n", (void*)eh->e_entry);
    return 0;

fail:
    /* Teardown the partially-built new map and switch back to kernel map */
    {
        uint64_t bad_cr3 = g_cur_cr3();
        g_cur_set_cr3(0);
        vmm_switch_to(vmm_get_kernel_cr3());
        if (bad_cr3) vmm_destroy_user_pagemap(bad_cr3);
        thread_user_maps_zero_current();
    }
    return -1;
}
