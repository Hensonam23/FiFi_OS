#include <stdint.h>
#include "idt.h"
#include "io.h"
#include "serial.h"
#include "isr.h"
extern void isr_stub_128(void);

/* Local helpers (C11-safe) */
static inline uint16_t read_cs(void) {
    uint16_t cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    return cs;
}

/* Provided by serial.c (used for debug prints) */
void print_hex_u16(uint16_t v);
void print_hex_u64(uint64_t v);


/* IDT entry (x86_64) */
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idtr idtp;

static void idt_set_gate_attr(int n, void *handler, uint8_t type_attr) {
    uint64_t addr = (uint64_t)handler;

    idt[n].offset_low  = (uint16_t)(addr & 0xFFFF);
    idt[n].selector    = 0x28;              /* kernel code selector */
    idt[n].ist         = 0;
    idt[n].type_attr   = type_attr;         /* <-- important */
    idt[n].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[n].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[n].zero        = 0;
}
static void idt_set_gate(int n, void *handler) {
    idt_set_gate_attr(n, handler, 0x8E);
}

static void idt_set_gate_user(int n, void *handler) {
    // Start as normal kernel interrupt gate (0x8E) then raise DPL to 3 (-> 0xEE)
    idt_set_gate(n, handler);
    idt[n].type_attr |= 0x60;
}


void idt_init(void) {
    serial_write("FiFi OS: idt_init start\n");
    serial_write("FiFi OS: CS=");
    print_hex_u16(read_cs());
    serial_write("\n");

    for (int i = 0; i < 256; i++) {
        idt[i] = (struct idt_entry){0};
    }
    // syscall entry (int 0x80), DPL=3
    idt_set_gate_user(0x80, isr_stub_128);
    /* IMPORTANT: install exception (0-31) + IRQ (32-47) gates */
    serial_write("FiFi OS: set exception+IRQ gates 0-47...\n");
    for (int i = 0; i < 48; i++) {
        idt_set_gate(i, (void*)isr_stub_table[i]);
    }

    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    idtp.base  = (uint64_t)&idt[0];

    serial_write("FiFi OS: IDT base=");
    print_hex_u64(idtp.base);
    serial_write(" limit=");
    print_hex_u16(idtp.limit);
    serial_write("\n");

    serial_write("FiFi OS: about to lidt\n");
    __asm__ volatile ("lidt %0" : : "m"(idtp));
    serial_write("FiFi OS: lidt done\n");
}
