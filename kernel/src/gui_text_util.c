#include "gui_internal.h"

void gui_toast(const char *msg, uint32_t color) {
    int _i = 0;
    while (msg[_i] && _i < 62) { g_toast_msg[_i] = msg[_i]; _i++; }
    g_toast_msg[_i] = '\0';
    g_toast_color   = color;
    g_toast_ticks   = (int)TOAST_TICKS;
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Panel (taskbar) top-left Y for a horizontal panel. TOP sits just below the
 * status bar; BOTTOM (default; also LEFT/RIGHT until the vertical layout lands)
 * hugs the bottom edge. Single source of truth for the panel's Y so draw, hit
 * and struts agree. */
uint64_t panel_y(void) {
    if (g_theme.panel_edge == PANEL_TOP)
        return g_theme.statusbar ? STATUS_H : 0u;
    return console_fb_height() - TASKBAR_H;
}

uint64_t desk_top(void) {
    uint64_t t = g_theme.statusbar ? STATUS_H : 0u;
    if (g_theme.panel_edge == PANEL_TOP) t += TASKBAR_H;   /* panel below status bar */
    return t;
}
uint64_t desk_bot(void) {
    uint64_t b = console_fb_height();
    if (g_theme.panel_edge != PANEL_TOP) b -= TASKBAR_H;   /* bottom-anchored panel */
    return b;
}
uint64_t desk_avail(void) { return desk_bot() - desk_top(); }

size_t gui_strlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}

bool gui_streq(const char *a, const char *b) {
    while (*a && *b) if (*a++ != *b++) return false;
    return *a == *b;
}

void gui_draw_str(uint64_t px, uint64_t py, const char *s,
                         uint32_t fg, uint32_t bg) {
    uint64_t fw = console_font_width();
    for (size_t i = 0; s[i]; i++)
        console_render_glyph(px + (uint64_t)i * fw, py,
                             (unsigned char)s[i], fg, bg);
}

/* Transparent-background string (only letter pixels drawn). For title-bar text so the
 * glyph cell background never paints beyond the title bar. */
void gui_draw_str_fg(uint64_t px, uint64_t py, const char *s, uint32_t fg) {
    uint64_t fw = console_font_width();
    for (size_t i = 0; s[i]; i++)
        console_render_glyph_fg(px + (uint64_t)i * fw, py, (unsigned char)s[i], fg);
}

void gui_draw_str_clip_fg(uint64_t px, uint64_t py, const char *s,
                                 uint32_t fg, uint64_t max_chars) {
    uint64_t fw = console_font_width();
    size_t len = gui_strlen(s);
    if (len <= max_chars) {
        gui_draw_str_fg(px, py, s, fg);
    } else if (max_chars >= 3) {
        for (size_t i = 0; i < max_chars - 3; i++)
            console_render_glyph_fg(px + (uint64_t)i * fw, py, (unsigned char)s[i], fg);
        for (size_t i = 0; i < 3; i++)
            console_render_glyph_fg(px + (uint64_t)(max_chars-3+i) * fw, py, '.', COL_FB_MUTED);
    }
}

/* Draw str at integer scale (each glyph pixel = scale×scale block) */
void gui_draw_str_scaled(uint64_t px, uint64_t py, const char *s,
                                 uint64_t scale, uint32_t fg, uint32_t bg) {
    uint64_t fw = console_font_width();
    for (size_t i = 0; s[i]; i++)
        console_render_glyph_scaled(px + (uint64_t)i * fw * scale, py,
                                    (unsigned char)s[i], scale, fg, bg);
}

/* Draw str clipped to max_chars wide */
void gui_draw_str_clip(uint64_t px, uint64_t py, const char *s,
                               uint32_t fg, uint32_t bg, uint64_t max_chars) {
    uint64_t fw = console_font_width();
    size_t len = gui_strlen(s);
    if (len <= max_chars) {
        gui_draw_str(px, py, s, fg, bg);
    } else if (max_chars >= 3) {
        /* truncate with "..." */
        for (size_t i = 0; i < max_chars - 3; i++)
            console_render_glyph(px + (uint64_t)i * fw, py, (unsigned char)s[i], fg, bg);
        for (size_t i = 0; i < 3; i++)
            console_render_glyph(px + (uint64_t)(max_chars-3+i) * fw, py, '.', COL_FB_MUTED, bg);
    }
}

/* Simple int-to-string, returns pointer into buf */
char *gui_itoa(int n, char *buf, int bufsz) {
    if (bufsz < 2) { buf[0]='\0'; return buf; }
    if (n == 0) { buf[0]='0'; buf[1]='\0'; return buf; }
    int i = 0;
    if (n < 0) { buf[i++] = '-'; n = -n; }
    char tmp[16]; int j = 0;
    while (n > 0 && j < 15) { tmp[j++] = '0' + (n % 10); n /= 10; }
    while (j > 0 && i < bufsz-1) buf[i++] = tmp[--j];
    buf[i] = '\0';
    return buf;
}

/* IPv4 uint32 (host byte order) to "x.x.x.x\0" in buf (need 16 bytes) */
void gui_ip4_str(uint32_t ip, char *buf, int bufsz) {
    char tmp[16]; int ti = 0;
    for (int byte = 3; byte >= 0; byte--) {
        uint32_t octet = (ip >> (uint32_t)(byte * 8)) & 0xFFu;
        char nb[4]; int ni = 0;
        if (octet == 0) { nb[ni++] = '0'; }
        else { uint32_t v = octet; while (v > 0) { nb[ni++] = '0' + (int)(v % 10); v /= 10; } }
        for (int k = ni - 1; k >= 0 && ti < 14; k--) tmp[ti++] = nb[k];
        if (byte > 0 && ti < 14) tmp[ti++] = '.';
    }
    tmp[ti] = '\0';
    int i = 0; while (tmp[i] && i < bufsz - 1) { buf[i] = tmp[i]; i++; } buf[i] = '\0';
}

/* Zero-pad 2-digit int into out[0..2] (null-terminated) */
void gui_itoa_pad2(uint64_t n, char *out) {
    n %= 100;
    out[0] = '0' + (int)(n / 10);
    out[1] = '0' + (int)(n % 10);
    out[2] = '\0';
}
