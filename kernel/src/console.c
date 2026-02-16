#include <stdint.h>
#include <stddef.h>
#include "console.h"

/* Public domain-ish minimal 8x8 font (only basic ASCII 32..127)
   To keep this step short, we draw only a very small subset and
   fallback to a solid block for unknown chars.
   Next milestone we'll drop in a full font table. */

static struct {
    volatile uint32_t *pix;
    uint64_t pitch32;
    uint64_t w, h;
    uint64_t cx, cy;
    uint32_t fg;
    uint32_t bg;
} con;

static void put_pixel(uint64_t x, uint64_t y, uint32_t c) {
    if (x >= con.w || y >= con.h) return;
    con.pix[y * con.pitch32 + x] = c;
}

static void draw_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t c) {
    for (uint64_t yy = 0; yy < h; yy++)
        for (uint64_t xx = 0; xx < w; xx++)
            put_pixel(x + xx, y + yy, c);
}

static void scroll_if_needed(void) {
    const uint64_t char_w = 8, char_h = 16; /* 8x16 cells (we scale) */
    const uint64_t cols = con.w / char_w;
    const uint64_t rows = con.h / char_h;

    if (con.cy < rows) return;

    /* crude scroll: clear screen and reset (simple for now) */
    draw_rect(0, 0, con.w, con.h, con.bg);
    con.cx = 0;
    con.cy = 0;
}

static void draw_glyph_block(uint64_t px, uint64_t py) {
    /* placeholder glyph: a simple 6x10 block centered */
    for (uint64_t y = 3; y < 13; y++)
        for (uint64_t x = 1; x < 7; x++)
            put_pixel(px + x, py + y, con.fg);
}

void console_init(struct limine_framebuffer *fb) {
    con.pix = (volatile uint32_t *)fb->address;
    con.pitch32 = fb->pitch / 4;
    con.w = fb->width;
    con.h = fb->height;
    con.cx = 0;
    con.cy = 0;

    con.fg = 0x00FFFFFF; /* white */
    con.bg = 0x00101010; /* near-black */

    draw_rect(0, 0, con.w, con.h, con.bg);
}

void console_putc(char c) {
    const uint64_t char_w = 8, char_h = 16;
    const uint64_t cols = con.w / char_w;

    if (c == '\n') {
        con.cx = 0;
        con.cy++;
        scroll_if_needed();
        return;
    }

    if (c == '\r') {
        con.cx = 0;
        return;
    }

    uint64_t px = con.cx * char_w;
    uint64_t py = con.cy * char_h;

    /* clear cell bg */
    draw_rect(px, py, char_w, char_h, con.bg);

    /* temporary: draw a placeholder glyph block */
    (void)c;
    draw_glyph_block(px, py);

    con.cx++;
    if (con.cx >= cols) {
        con.cx = 0;
        con.cy++;
        scroll_if_needed();
    }
}

void console_write(const char *s) {
    for (size_t i = 0; s[i]; i++) {
        console_putc(s[i]);
    }
}
