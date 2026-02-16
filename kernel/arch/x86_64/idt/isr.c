#include <stdint.h>
#include "serial.h"
#include "panic.h"

struct __attribute__((packed)) isr_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    uint64_t int_no;
    uint64_t err_code;

    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
};

static void print_hex_u64(uint64_t v) {
    const char *hex = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = 0;
    serial_write(buf);
}

void isr_common_handler(struct isr_frame *f) {
    serial_write("\n[EXCEPTION] int=");
    print_hex_u64(f->int_no);
    serial_write(" err=");
    print_hex_u64(f->err_code);
    serial_write(" rip=");
    print_hex_u64(f->rip);
    serial_write("\n");

    if (f->int_no == 0)  panic("Divide by zero");
    if (f->int_no == 8)  panic("Double Fault");
    if (f->int_no == 13) panic("General Protection Fault");
    if (f->int_no == 14) panic("Page Fault");

    panic("Unhandled exception");
}
