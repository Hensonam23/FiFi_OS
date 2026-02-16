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
