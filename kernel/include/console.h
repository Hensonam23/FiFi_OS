#pragma once
#include <stdint.h>
#include <limine.h>

void console_init(struct limine_framebuffer *fb);
void console_putc(char c);
void console_write(const char *s);
