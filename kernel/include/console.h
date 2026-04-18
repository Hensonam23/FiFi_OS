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

/* Status bar support — reserve pixel rows at the top of the screen */
void     console_set_y_offset(uint64_t pixels);
void     console_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t color);
void     console_render_glyph(uint64_t px, uint64_t py, unsigned char ch, uint32_t fg, uint32_t bg);
void     console_render_glyph_scaled(uint64_t px, uint64_t py, unsigned char ch, uint64_t scale, uint32_t fg, uint32_t bg);
uint64_t console_fb_width(void);
uint64_t console_fb_height(void);

/* PSF font loading — loads a .psf file from VFS into the console renderer */
bool        console_load_psf(const char *path);
uint32_t    console_font_width(void);
uint32_t    console_font_height(void);
const char *console_font_name(void);
