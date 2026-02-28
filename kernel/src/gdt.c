#include <stdint.h>
#include <stdbool.h>

/*
Selectors chosen to MATCH your current CS=0x28.
Index = selector >> 3.

  0: null
  1-4: unused (kept zero)
  5: kernel code  (0x28)
  6: kernel data  (0x30)
  7: user data    (0x38) -> use 0x3B in ring3
  8: user code    (0x40) -> use 0x43 in ring3
  9-10: TSS desc  (0x48)
*/

#define GDT_KERNEL_CS  0x28
#define GDT_KERNEL_DS  0x30
#define GDT_USER_DS    0x3B
#define GDT_USER_CS    0x43
#define GDT_TSS_SEL    0x48

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_high;
} gdt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} gdt_tss_desc_t;

typedef struct __attribute__((packed)) {
    uint32_t rsv0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t rsv1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t rsv2;
    uint16_t rsv3;
    uint16_t iopb;
} tss64_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdtr_t;

static struct __attribute__((packed)) {
    gdt_entry_t    e[9];   /* 0..8 */
    gdt_tss_desc_t tss;    /* 9..10 */
} g_gdt;

static gdtr_t  g_gdtr;
static tss64_t g_tss;

static bool     g_tss_loaded = false;
static uint64_t g_tss_rsp0_shadow = 0;

static gdt_entry_t make_seg(uint8_t access, uint8_t flags) {
    gdt_entry_t d = {0};
    d.limit_low = 0;
    d.base_low  = 0;
    d.base_mid  = 0;
    d.access    = access;
    d.gran      = flags;   /* flags go in high nibble area; we keep limit=0 */
    d.base_high = 0;
    return d;
}

static void set_tss_desc(gdt_tss_desc_t *d, uint64_t base, uint32_t limit) {
    d->limit_low  = (uint16_t)(limit & 0xFFFF);
    d->base_low   = (uint16_t)(base & 0xFFFF);
    d->base_mid   = (uint8_t)((base >> 16) & 0xFF);
    d->access     = 0x89; /* present=1, type=9 (available 64-bit TSS) */
    d->gran       = (uint8_t)((limit >> 16) & 0x0F);
    d->base_high  = (uint8_t)((base >> 24) & 0xFF);
    d->base_upper = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    d->reserved   = 0;
}

static inline void lgdt(const gdtr_t *g) {
    __asm__ volatile ("lgdt %0" : : "m"(*g) : "memory");
}

static inline void ltr(uint16_t sel) {
    __asm__ volatile ("ltr %0" : : "r"(sel) : "memory");
}

static inline void load_seg_regs(uint16_t ds) {
    __asm__ volatile (
        "mov %0, %%ds\n"
        "mov %0, %%es\n"
        "mov %0, %%ss\n"
        "xor %%ax, %%ax\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        :
        : "r"(ds)
        : "ax", "memory"
    );
}

void gdt_init(void) {
    /* zero everything */
    for (int i = 0; i < 9; i++) g_gdt.e[i] = (gdt_entry_t){0};
    g_gdt.tss = (gdt_tss_desc_t){0};
    g_tss = (tss64_t){0};

    /* Kernel code/data at selectors 0x28/0x30 */
    /* access:
        kernel code = 0x9A
        kernel data = 0x92
        user code   = 0xFA
        user data   = 0xF2
       flags:
        64-bit code needs L=1 -> 0x20
    */
    g_gdt.e[5] = make_seg(0x9A, 0x20); /* kernel code */
    g_gdt.e[6] = make_seg(0x92, 0x00); /* kernel data */
    g_gdt.e[7] = make_seg(0xF2, 0x00); /* user data */
    g_gdt.e[8] = make_seg(0xFA, 0x20); /* user code */

    /* TSS */
    g_tss.iopb = sizeof(tss64_t);
    set_tss_desc(&g_gdt.tss, (uint64_t)&g_tss, (uint32_t)(sizeof(tss64_t) - 1));

    g_gdtr.limit = (uint16_t)(sizeof(g_gdt) - 1);
    g_gdtr.base  = (uint64_t)&g_gdt;

    lgdt(&g_gdtr);

    /* reload DS/SS etc to our known kernel data selector */
    load_seg_regs(GDT_KERNEL_DS);

    /* load task register */
    ltr(GDT_TSS_SEL);
    g_tss_loaded = true;
}

bool gdt_tss_loaded(void) { return g_tss_loaded; }

uint64_t gdt_tss_rsp0(void) { return g_tss_rsp0_shadow; }

void gdt_tss_set_rsp0(uint64_t rsp0) {
    g_tss_rsp0_shadow = rsp0;
    g_tss.rsp0 = rsp0;
}


/* selectors for ring3 transitions */
uint16_t gdt_user_cs(void) { return (uint16_t)GDT_USER_CS; }
uint16_t gdt_user_ds(void) { return (uint16_t)GDT_USER_DS; }
