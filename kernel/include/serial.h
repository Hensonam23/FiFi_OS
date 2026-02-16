#pragma once
#include <stdint.h>

void serial_init(void);
void serial_write_char(char c);
void serial_write(const char *s);

/* Debug helpers */
void print_hex_u16(uint16_t v);
void print_hex_u64(uint64_t v);
