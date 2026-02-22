#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "kprintf.h"
#include "elf.h"

#define PAGE_SIZE 4096ULL

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static uint64_t align_down(uint64_t x, uint64_t a) {
    return x & ~(a - 1);
}

static uint64_t align_up(uint64_t x, uint64_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

static const char *ptype_name(uint32_t t) {
    switch (t) {
        case 0: return "NULL";
        case 1: return "LOAD";
        case 2: return "DYNAMIC";
        case 3: return "INTERP";
        case 4: return "NOTE";
        case 5: return "SHLIB";
        case 6: return "PHDR";
        default: return "?";
    }
}

void elf_dump(const void *data, uint64_t size) {
    if (!data || size < 0x40) {
        kprintf("ELF: invalid buffer\n");
        return;
    }

    const uint8_t *b = (const uint8_t *)data;

    // Magic
    if (!(b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F')) {
        kprintf("ELF: bad magic\n");
        return;
    }

    // e_ident
    uint8_t ei_class = b[4]; // 2 = ELF64
    uint8_t ei_data  = b[5]; // 1 = little endian

    if (ei_class != 2) {
        kprintf("ELF: not ELF64 (class=%p)\n", (void*)(uint64_t)ei_class);
        return;
    }
    if (ei_data != 1) {
        kprintf("ELF: not little-endian (data=%p)\n", (void*)(uint64_t)ei_data);
        return;
    }

    // ELF64 header fields (fixed offsets)
    uint64_t e_entry     = rd64(b + 0x18);
    uint64_t e_phoff     = rd64(b + 0x20);
    uint16_t e_ehsize    = rd16(b + 0x34);
    uint16_t e_phentsize = rd16(b + 0x36);
    uint16_t e_phnum     = rd16(b + 0x38);

    kprintf("ELF: valid\n");
    kprintf("entry=%p phoff=%p phentsz=%p phnum=%p ehsize=%p\n",
            (void*)e_entry, (void*)e_phoff,
            (void*)(uint64_t)e_phentsize, (void*)(uint64_t)e_phnum,
            (void*)(uint64_t)e_ehsize);

    if (e_ehsize < 0x40) {
        kprintf("ELF: bad ehsize\n");
        return;
    }

    if (e_phentsize < 0x38) {
        kprintf("ELF: unsupported phentsz (need >= 0x38)\n");
        return;
    }

    uint64_t ph_table_size = (uint64_t)e_phentsize * (uint64_t)e_phnum;
    if (e_phoff + ph_table_size > size) {
        kprintf("ELF: program header table out of range\n");
        return;
    }

    // Limit output so it stays readable
    uint16_t max_ph = e_phnum;
    if (max_ph > 24) max_ph = 24;

    for (uint16_t i = 0; i < max_ph; i++) {
        const uint8_t *ph = b + e_phoff + (uint64_t)i * (uint64_t)e_phentsize;

        // ELF64 Phdr offsets
        uint32_t p_type   = rd32(ph + 0x00);
        uint32_t p_flags  = rd32(ph + 0x04);
        uint64_t p_offset = rd64(ph + 0x08);
        uint64_t p_vaddr  = rd64(ph + 0x10);
        uint64_t p_paddr  = rd64(ph + 0x18);
        uint64_t p_filesz = rd64(ph + 0x20);
        uint64_t p_memsz  = rd64(ph + 0x28);
        uint64_t p_align  = rd64(ph + 0x30);

        (void)p_paddr;

        kprintf("ph[%p]: type=%p(%s) flags=%p off=%p vaddr=%p filesz=%p memsz=%p align=%p\n",
                (void*)(uint64_t)i,
                (void*)(uint64_t)p_type, ptype_name(p_type),
                (void*)(uint64_t)p_flags,
                (void*)p_offset,
                (void*)p_vaddr,
                (void*)p_filesz,
                (void*)p_memsz,
                (void*)p_align);

        if (p_type == 1) { // PT_LOAD
            uint64_t seg_start = align_down(p_vaddr, PAGE_SIZE);
            uint64_t seg_end   = align_up(p_vaddr + p_memsz, PAGE_SIZE);
            uint64_t pages     = (seg_end > seg_start) ? ((seg_end - seg_start) / PAGE_SIZE) : 0;

            kprintf("  LOAD plan: vaddr=[%p..%p) pages=%p file_off=%p file_bytes=%p mem_bytes=%p\n",
                    (void*)seg_start, (void*)seg_end,
                    (void*)pages,
                    (void*)p_offset,
                    (void*)p_filesz,
                    (void*)p_memsz);

            // sanity: file range must exist
            if (p_offset + p_filesz > size) {
                kprintf("  WARNING: segment file range out of initrd buffer\n");
            }
        }
    }

    if (e_phnum > max_ph) {
        kprintf("(truncated: showing first %p phdrs)\n", (void*)(uint64_t)max_ph);
    }

    kprintf("NOTE: This is an ELF load PLAN only. Actual mapping + usermode run is next milestone.\n");
}
