#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "gui.h"
#include "console.h"
#include "mouse.h"

#define STATUS_H   20u
#define TITLE_H    24u
#define BORDER     1u
#define PAD        4u

#define COL_DESKTOP  0x001a1a2eu
#define COL_BORDER   0x003060c0u
#define COL_TITLE_FG 0x00e8eeffu
#define COL_WIN_BG   0x00101010u
#define COL_CLOSE    0x00993333u
#define COL_HINT     0x00607890u

typedef enum { GUI_WINDOWED, GUI_DESKTOP_ONLY } gui_state_t;
static gui_state_t g_state = GUI_DESKTOP_ONLY;

/* Persistent window geometry set by gui_open() */
static uint64_t g_win_x, g_win_y, g_win_w, g_win_h;
static uint64_t g_btn_x;   /* close button left edge (absolute px) */

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

static void gui_open(void) {
    uint64_t fb_w = console_fb_width();
    uint64_t fb_h = console_fb_height();
    uint64_t fw   = console_font_width();
    uint64_t fh   = console_font_height();
    uint64_t avail = fb_h - STATUS_H;

    /* Desktop fill */
    console_fill_rect(0, STATUS_H, fb_w, avail, COL_DESKTOP);

    /* Window geometry */
    g_win_w = fb_w * 88 / 100;
    g_win_h = avail * 90 / 100;
    g_win_x = (fb_w - g_win_w) / 2;
    g_win_y = STATUS_H + (avail - g_win_h) / 2;

    /* Title bar */
    console_fill_rect(g_win_x, g_win_y, g_win_w, TITLE_H, COL_BORDER);

    const char *title = "Terminal";
    uint64_t tlen = (uint64_t)gui_strlen(title);
    uint64_t tpx  = g_win_x + (g_win_w - tlen * fw) / 2;
    uint64_t tpy  = g_win_y + (TITLE_H - fh) / 2;
    gui_draw_str(tpx, tpy, title, COL_TITLE_FG, COL_BORDER);

    /* Close button */
    g_btn_x = g_win_x + g_win_w - TITLE_H;
    console_fill_rect(g_btn_x, g_win_y, TITLE_H, TITLE_H, COL_CLOSE);
    console_render_glyph(g_btn_x + (TITLE_H - fw) / 2, tpy, 'x', COL_TITLE_FG, COL_CLOSE);

    /* Borders */
    console_fill_rect(g_win_x, g_win_y + TITLE_H, BORDER, g_win_h - TITLE_H, COL_BORDER);
    console_fill_rect(g_win_x + g_win_w - BORDER, g_win_y + TITLE_H, BORDER, g_win_h - TITLE_H, COL_BORDER);
    console_fill_rect(g_win_x, g_win_y + g_win_h - BORDER, g_win_w, BORDER, COL_BORDER);

    /* Content background */
    uint64_t inner_x = g_win_x + BORDER;
    uint64_t inner_y = g_win_y + TITLE_H;
    uint64_t inner_w = g_win_w - 2 * BORDER;
    uint64_t inner_h = g_win_h - TITLE_H - BORDER;
    console_fill_rect(inner_x, inner_y, inner_w, inner_h, COL_WIN_BG);

    /* Console viewport inside window */
    console_set_viewport(inner_x + PAD, inner_y + PAD,
                         inner_w - 2 * PAD, inner_h - 2 * PAD);

    g_state = GUI_WINDOWED;
}

static void gui_close(void) {
    uint64_t fb_w = console_fb_width();
    uint64_t fb_h = console_fb_height();
    uint64_t fw   = console_font_width();
    uint64_t fh   = console_font_height();

    /* Erase window with desktop color */
    console_fill_rect(g_win_x, g_win_y, g_win_w, g_win_h, COL_DESKTOP);

    /* Park console viewport at 1x1 in the bottom-left so shell output is invisible */
    console_set_viewport(0, fb_h - fh, fw, fh);

    /* "Click to reopen" hint centered on desktop */
    const char *hint = "Click anywhere to reopen Terminal";
    uint64_t hlen = (uint64_t)gui_strlen(hint);
    uint64_t hx   = (fb_w - hlen * fw) / 2;
    uint64_t hy   = STATUS_H + (fb_h - STATUS_H - fh) / 2;
    gui_draw_str(hx, hy, hint, COL_HINT, COL_DESKTOP);

    g_state = GUI_DESKTOP_ONLY;
}

void gui_init(void) {
    gui_open();
}

void gui_on_tick(void) {
    int32_t cx, cy;
    if (!mouse_consume_click(&cx, &cy)) return;

    if (g_state == GUI_DESKTOP_ONLY) {
        /* any click reopens the terminal */
        gui_open();
        return;
    }

    /* GUI_WINDOWED: check close button hit region */
    int32_t bx = (int32_t)g_btn_x;
    int32_t wy = (int32_t)g_win_y;
    if (cx >= bx && cx < bx + (int32_t)TITLE_H &&
        cy >= wy && cy < wy + (int32_t)TITLE_H) {
        gui_close();
    }
}
