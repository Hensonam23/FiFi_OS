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
/* When the panel is on TOP, the status bar moves to the BOTTOM so the top edge
 * holds only the panel (no stacked double-bar). Otherwise the status bar is at
 * the top. Disabled status bar => no bar at all (desktop grows). */
bool statusbar_bottom(void) { return g_theme.statusbar && g_theme.panel_edge == PANEL_TOP; }
uint64_t statusbar_y(void) {
    return statusbar_bottom() ? console_fb_height() - STATUS_H : 0u;
}

uint64_t panel_y(void) {
    /* TOP panel sits at the very top (the status bar, if any, is at the bottom). */
    if (g_theme.panel_edge == PANEL_TOP) return 0u;
    return console_fb_height() - TASKBAR_H;
}

bool panel_is_vertical(void) {
    return g_theme.panel_edge == PANEL_LEFT || g_theme.panel_edge == PANEL_RIGHT;
}
/* Left X of a vertical (left/right) panel strip. */
uint64_t panel_x(void) {
    if (g_theme.panel_edge == PANEL_RIGHT) return console_fb_width() - TASKBAR_H;
    return 0;   /* LEFT (or n/a) */
}

uint64_t desk_top(void) {
    uint64_t t = 0u;
    if (g_theme.statusbar && !statusbar_bottom()) t = STATUS_H;   /* status bar at top */
    /* An auto-hidden panel floats over windows, so it reserves no desktop space. */
    if (g_theme.panel_edge == PANEL_TOP && !g_theme.panel_autohide) t += TASKBAR_H;
    return t;
}
uint64_t desk_bot(void) {
    uint64_t b = console_fb_height();
    if (g_theme.panel_edge == PANEL_BOTTOM && !g_theme.panel_autohide) b -= TASKBAR_H;
    if (statusbar_bottom()) b -= STATUS_H;   /* status bar relocated to the bottom */
    return b;
}
/* Horizontal desktop bounds — a left/right panel reserves a vertical strip. */
uint64_t desk_left(void) {
    if (g_theme.panel_edge == PANEL_LEFT && !g_theme.panel_autohide) return TASKBAR_H;
    return 0u;
}
uint64_t desk_right(void) {
    uint64_t r = console_fb_width();
    if (g_theme.panel_edge == PANEL_RIGHT && !g_theme.panel_autohide) r -= TASKBAR_H;
    return r;
}
uint64_t desk_avail(void)  { return desk_bot() - desk_top(); }
uint64_t desk_availw(void) { return desk_right() - desk_left(); }

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
