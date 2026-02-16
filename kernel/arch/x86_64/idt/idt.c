#include <stdint.h>
#include "idt.h"
#include "serial.h"

extern void isr0(void);
extern void isr8(void);
extern void isr13(void);
extern void isr14(void);

struct __attribute__((packed)) idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
};

struct __attribute__((packed)) idt_ptr {
    uint16_t limit;
    uint64_t base;
};

static struct idt_entry idt[256] __attribute__((aligned(16)));
static struct idt_ptr idtp;

static inline uint16_t read_cs(void) {
    uint16_t cs;
    __asm__ __volatile__("mov %%cs, %0" : "=r"(cs));
    return cs;
}

static void print_hex_u64(uint64_t v) {
    const char *hex = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) buf[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
    buf[18] = 0;
    serial_write(buf);
}

static void print_hex_u16(uint16_t v) {
    const char *hex = "0123456789ABCDEF";
    char buf[7];
    buf[0] = '0'; buf[1] = 'x';
    buf[2] = hex[(v >> 12) & 0xF];
    buf[3] = hex[(v >> 8) & 0xF];
    buf[4] = hex[(v >> 4) & 0xF];
    buf[5] = hex[(v >> 0) & 0xF];
    buf[6] = 0;
    serial_write(buf);
}

static void idt_set_gate(int n, void *handler) {
    uint64_t h = (uint64_t)handler;
    idt[n].offset_low  = h & 0xFFFF;
    idt[n].selector    = read_cs();
    idt[n].ist         = 0;
    idt[n].type_attr   = 0x8E;
    idt[n].offset_mid  = (h >> 16) & 0xFFFF;
    idt[n].offset_high = (h >> 32) & 0xFFFFFFFF;
    idt[n].zero        = 0;
}

void idt_init(void) {
    serial_write("FiFi OS: idt_init start\n");
    serial_write("FiFi OS: CS=");
    print_hex_u16(read_cs());
    serial_write("\n");

    for (int i = 0; i < 256; i++) idt[i] = (struct idt_entry){0};

    serial_write("FiFi OS: set gates...\n");
    idt_set_gate(0,  isr0);
    idt_set_gate(8,  isr8);
    idt_set_gate(13, isr13);
    idt_set_gate(14, isr14);

    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    idtp.base  = (uint64_t)&idt[0];

    serial_write("FiFi OS: IDT base=");
    print_hex_u64(idtp.base);
    serial_write(" limit=");
    print_hex_u16(idtp.limit);
    serial_write("\n");

    serial_write("FiFi OS: about to lidt\n");
    __asm__ __volatile__("lidt %0" : : "m"(idtp));
    serial_write("FiFi OS: lidt done\n");
}
