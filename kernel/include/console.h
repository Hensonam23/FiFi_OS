#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <limine.h>

void console_init(struct limine_framebuffer *fb);

bool console_ready(void);
void console_set_colors(uint32_t fg, uint32_t bg);
void console_clear(void);

void console_putc(char c);
void console_write(const char *s);

void console_get_cursor(uint32_t *x, uint32_t *y);
void console_set_cursor(uint32_t x, uint32_t y);
