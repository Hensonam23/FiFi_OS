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
/* GUI viewport — constrain text rendering to a sub-rect of the framebuffer */
void     console_set_viewport(uint64_t x, uint64_t y, uint64_t w, uint64_t h);
void     console_set_viewport_norender(uint64_t x, uint64_t y, uint64_t w, uint64_t h);
void     console_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t color);
void     console_render_glyph(uint64_t px, uint64_t py, unsigned char ch, uint32_t fg, uint32_t bg);
void     console_render_glyph_scaled(uint64_t px, uint64_t py, unsigned char ch, uint64_t scale, uint32_t fg, uint32_t bg);
uint64_t           console_fb_width(void);
uint64_t           console_fb_height(void);
uint64_t           console_viewport_x(void);
uint64_t           console_viewport_y(void);
volatile uint32_t *console_fb_ptr(void);
uint64_t           console_pitch32(void);

/* Double buffering — call console_backbuf_init() once after pmm_init()/vmm_init().
 * After that all rendering goes to a RAM backbuf; call console_flip_if_dirty()
 * at the end of each frame tick to push the completed frame to VRAM. */
void console_backbuf_init(void);
bool console_flip_if_dirty(void);
/* Pixel capture/paste for shadow-buffer drag.  buf must be w*h uint32_t's. */
bool console_capture_rect(uint32_t *buf, uint64_t x, uint64_t y, uint64_t w, uint64_t h);
void console_paste_rect(const uint32_t *buf, uint64_t x, uint64_t y, uint64_t w, uint64_t h);

/* PSF font loading — loads a .psf file from VFS into the console renderer */
bool        console_load_psf(const char *path);
uint32_t    console_font_width(void);
uint32_t    console_font_height(void);
const char *console_font_name(void);

/* Terminal scrollback ring buffer (64KB).
 * All console_putc output is captured here even while suppressed. */
#define CONSOLE_TSB_CAP (64u * 1024u)
void     console_set_suppress_draw(bool on);  /* suppress screen output */
int      console_tsb_count_lines(void);        /* total newlines in ring */
/* Fill buf with line content, line_from_end=0 is newest. Returns bytes written. */
int      console_tsb_get_line(int line_from_end, char *buf, int maxlen);
