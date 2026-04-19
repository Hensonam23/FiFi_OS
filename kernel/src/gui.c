#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "gui.h"
#include "console.h"

#define STATUS_H   20u
#define TITLE_H    24u
#define BORDER     1u
#define PAD        4u

#define COL_DESKTOP  0x001a1a2eu
#define COL_BORDER   0x003060c0u
#define COL_TITLE_FG 0x00e8eeffu
#define COL_WIN_BG   0x00101010u
#define COL_CLOSE    0x00993333u

static size_t gui_strlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}

static void gui_draw_str(uint64_t px, uint64_t py, const char *s,
                         uint32_t fg, uint32_t bg) {
    uint64_t fw = console_font_width();
    for (size_t i = 0; s[i]; i++)
        console_render_glyph(px + (uint64_t)i * fw, py,
                             (unsigned char)s[i], fg, bg);
}

void gui_init(void) {
    uint64_t fb_w = console_fb_width();
    uint64_t fb_h = console_fb_height();
    uint64_t fw   = console_font_width();
    uint64_t fh   = console_font_height();
    uint64_t avail = fb_h - STATUS_H;

    /* Desktop fill below status bar */
    console_fill_rect(0, STATUS_H, fb_w, avail, COL_DESKTOP);

    /* Window geometry */
    uint64_t win_w = fb_w * 88 / 100;
    uint64_t win_h = avail * 90 / 100;
    uint64_t win_x = (fb_w - win_w) / 2;
    uint64_t win_y = STATUS_H + (avail - win_h) / 2;

    /* Title bar */
    console_fill_rect(win_x, win_y, win_w, TITLE_H, COL_BORDER);

    /* "Terminal" centered in title bar */
    const char *title = "Terminal";
    uint64_t tlen  = (uint64_t)gui_strlen(title);
    uint64_t tpx   = win_x + (win_w - tlen * fw) / 2;
    uint64_t tpy   = win_y + (TITLE_H - fh) / 2;
    gui_draw_str(tpx, tpy, title, COL_TITLE_FG, COL_BORDER);

    /* Close button (top-right of title bar) */
    uint64_t btn_x = win_x + win_w - TITLE_H;
    console_fill_rect(btn_x, win_y, TITLE_H, TITLE_H, COL_CLOSE);
    uint64_t xpx = btn_x + (TITLE_H - fw) / 2;
    console_render_glyph(xpx, tpy, 'x', COL_TITLE_FG, COL_CLOSE);

    /* Side and bottom borders */
    console_fill_rect(win_x, win_y + TITLE_H, BORDER, win_h - TITLE_H, COL_BORDER);
    console_fill_rect(win_x + win_w - BORDER, win_y + TITLE_H, BORDER, win_h - TITLE_H, COL_BORDER);
    console_fill_rect(win_x, win_y + win_h - BORDER, win_w, BORDER, COL_BORDER);

    /* Content area background */
    uint64_t inner_x = win_x + BORDER;
    uint64_t inner_y = win_y + TITLE_H;
    uint64_t inner_w = win_w - 2 * BORDER;
    uint64_t inner_h = win_h - TITLE_H - BORDER;
    console_fill_rect(inner_x, inner_y, inner_w, inner_h, COL_WIN_BG);

    /* Set console viewport: add padding inside the border */
    uint64_t cx = inner_x + PAD;
    uint64_t cy = inner_y + PAD;
    uint64_t cw = inner_w - 2 * PAD;
    uint64_t ch = inner_h - 2 * PAD;
    console_set_viewport(cx, cy, cw, ch);
}
