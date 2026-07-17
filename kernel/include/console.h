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
/* Alpha-blend a colored rect over whatever is already drawn (alpha 0..255, 255=opaque). */
void     console_blend_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t color, uint8_t alpha);
/* Vertical gradient fill from color c0 (top row) to c1 (bottom row). */
void     console_fill_vgrad(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t c0, uint32_t c1);
void     console_render_glyph(uint64_t px, uint64_t py, unsigned char ch, uint32_t fg, uint32_t bg);
void     console_render_glyph_fg(uint64_t px, uint64_t py, unsigned char ch, uint32_t fg);
void     console_render_glyph_scaled(uint64_t px, uint64_t py, unsigned char ch, uint64_t scale, uint32_t fg, uint32_t bg);
/* Codepoint-aware variants: take a decoded Unicode codepoint (UTF-8 text). */
void     console_render_glyph_cp(uint64_t px, uint64_t py, uint32_t cp, uint32_t fg, uint32_t bg);
void     console_render_glyph_fg_cp(uint64_t px, uint64_t py, uint32_t cp, uint32_t fg);
void     console_render_glyph_scaled_cp(uint64_t px, uint64_t py, uint32_t cp, uint64_t scale, uint32_t fg, uint32_t bg);
uint64_t           console_fb_width(void);
uint64_t           console_fb_height(void);
uint64_t           console_cols(void);   /* visible character columns */
uint64_t           console_rows(void);   /* visible character rows    */
uint64_t           console_viewport_x(void);
uint64_t           console_viewport_y(void);
volatile uint32_t *console_fb_ptr(void);
uint64_t           console_pitch32(void);

/* Double buffering — call console_backbuf_init() once after pmm_init()/vmm_init().
 * After that all rendering goes to a RAM backbuf; call console_flip_if_dirty()
 * at the end of each frame tick to push the completed frame to VRAM. */
void console_backbuf_init(void);
bool console_flip_if_dirty(void);
/* Dirty range helpers — let the cursor layer force-flush specific rows */
void      console_mark_dirty_rows(uint32_t y0, uint32_t y1);
uint32_t *console_backbuf_ptr(void);
uint64_t  console_backbuf_pitch32(void);
/* Pixel capture/paste for shadow-buffer drag.  buf must be w*h uint32_t's. */
bool console_capture_rect(uint32_t *buf, uint64_t x, uint64_t y, uint64_t w, uint64_t h);
void console_paste_rect(const uint32_t *buf, uint64_t x, uint64_t y, uint64_t w, uint64_t h);
void console_blit_scaled(const uint32_t *src, uint64_t sw, uint64_t sh,
                         uint64_t dx, uint64_t dy, uint64_t dw, uint64_t dh);
/* Scale-blit a source sub-rect into a dest rect (clamped), for wallpaper fit modes. */
void console_blit_scaled_src(const uint32_t *src, uint64_t sw, uint64_t sh,
                             int64_t sx0, int64_t sy0, uint64_t scw, uint64_t sch,
                             uint64_t dx, uint64_t dy, uint64_t dw, uint64_t dh);
/* Same but ARGB source with per-pixel alpha blending (app-icon logos). */
void console_blit_scaled_alpha(const uint32_t *src, uint64_t sw, uint64_t sh,
                               uint64_t dx, uint64_t dy, uint64_t dw, uint64_t dh);

/* PSF font loading — loads a .psf file from VFS into the console renderer */
bool        console_load_psf(const char *path);
#ifdef __linux__
/* Unified loader: .ttf/.otf/.ttc → scalable AA font at px_size; else PSF. */
bool        console_load_font(const char *path, int px_size);
/* Draw `s` in the font at `path` (scaled to target_h) without changing the
 * active font — used for font-name previews. Returns pixel width drawn. */
uint64_t    console_render_ttf_name(const char *path, const char *s, uint64_t px, uint64_t py,
                                    uint32_t target_h, uint32_t fg, uint64_t max_w);
#endif
uint32_t    console_font_width(void);
uint32_t    console_font_height(void);
const char *console_font_name(void);
/* Render `s` in the font at `path` (scaled to target_h) without changing the
 * active font — used for per-font preview labels. Returns pixel width drawn. */
uint64_t    console_render_psf_string(const char *path, const char *s,
                                      uint64_t px, uint64_t py,
                                      uint32_t target_h, uint32_t fg);

/* Terminal scrollback ring buffer (64KB).
 * All console_putc output is captured here even while suppressed. */
#define CONSOLE_TSB_CAP (64u * 1024u)
void     console_set_suppress_draw(bool on);  /* suppress screen output */
int      console_tsb_count_lines(void);        /* total newlines in ring */
/* Fill buf with line content, line_from_end=0 is newest. Returns bytes written. */
int      console_tsb_get_line(int line_from_end, char *buf, int maxlen);
