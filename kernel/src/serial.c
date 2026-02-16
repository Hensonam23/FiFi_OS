#include <stdint.h>
#include "io.h"
#include "serial.h"

#define COM1 0x3F8

static int serial_transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);    // Disable interrupts
    outb(COM1 + 3, 0x80);    // Enable DLAB
    outb(COM1 + 0, 0x03);    // Divisor low byte (38400 baud)
    outb(COM1 + 1, 0x00);    // Divisor high byte
    outb(COM1 + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7);    // Enable FIFO, clear, 14-byte threshold
    outb(COM1 + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

void serial_write_char(char c) {
    while (!serial_transmit_empty()) { }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) {
    for (uint64_t i = 0; s[i] != 0; i++) {
        if (s[i] == '\n') serial_write_char('\r');
        serial_write_char(s[i]);
    }
}

/* Debug helpers (used by early boot / IDT init) */
static const char fifi_hexdigits[] = "0123456789ABCDEF";

void print_hex_u16(uint16_t v) {
    char buf[2 + 4 + 1];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 4; i++) {
        int shift = (3 - i) * 4;
        buf[2 + i] = fifi_hexdigits[(v >> shift) & 0xF];
    }
    buf[6] = '\0';
    serial_write(buf);
}

void print_hex_u64(uint64_t v) {
    char buf[2 + 16 + 1];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int shift = (15 - i) * 4;
        buf[2 + i] = fifi_hexdigits[(v >> shift) & 0xF];
    }
    buf[18] = '\0';
    serial_write(buf);
}
