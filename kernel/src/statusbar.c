/*
 * statusbar.c — Persistent top status bar.
 *
 * Draws a 20-pixel strip at y=0 showing:
 *   left:   "FiFi OS"
 *   center: IP address (or "no network")
 *   right:  uptime
 *
 * Updates once per second via statusbar_on_tick() from pit_on_tick().
 * Writes directly to framebuffer — bypasses the scrollable console area.
 */

#include "statusbar.h"
#include "console.h"
#include "net.h"
#include "pit.h"

/* ── Layout ──────────────────────────────────────────────────────────────── */
#define STATUS_H    20u    /* total pixel height of bar                      */
#define TEXT_Y       2u    /* glyph top within bar (centres 16px in 18px)    */
#define SEP_Y       18u    /* separator start row                             */
#define SEP_H        2u    /* separator height (pixels)                       */

/* ── Colors ──────────────────────────────────────────────────────────────── */
#define BAR_BG      0x001a1a2eu   /* dark navy background                    */
#define BAR_ACCENT  0x00e8eeffu   /* bright white — "FiFi OS" label          */
#define BAR_FG      0x0090aad0u   /* muted blue-grey — IP, uptime            */
#define BAR_SEP     0x003060c0u   /* blue separator line                     */

/* ── State ───────────────────────────────────────────────────────────────── */
static uint64_t g_fb_w     = 0;
static uint64_t g_last_sec = (uint64_t)-1;   /* force draw on first tick    */

/* ── Mini helpers (no libc in kernel) ───────────────────────────────────── */

static size_t sb_strlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}

/* Write decimal of v into buf[], NUL-terminate, return char count. */
static size_t sb_udec(char *buf, uint64_t v) {
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[20]; size_t n = 0;
    while (v) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    for (size_t i = 0; i < n; i++) buf[i] = tmp[n - 1u - i];
    buf[n] = '\0';
    return n;
}

/* Append src to dst starting at pos; return new position. */
static size_t sb_cat(char *dst, size_t pos, const char *src) {
    for (size_t i = 0; src[i]; i++) dst[pos++] = src[i];
    dst[pos] = '\0';
    return pos;
}

/* ── Rendering ───────────────────────────────────────────────────────────── */

static void bar_str(uint64_t px, const char *s, uint32_t fg) {
    for (size_t i = 0; s[i]; i++)
        console_render_glyph(px + (uint64_t)i * 8u, TEXT_Y,
                             (unsigned char)s[i], fg, BAR_BG);
}

static void bar_draw(void) {
    if (!console_ready() || g_fb_w == 0) return;

    /* Background */
    console_fill_rect(0, 0, g_fb_w, STATUS_H - SEP_H, BAR_BG);
    /* Bottom separator line */
    console_fill_rect(0, SEP_Y, g_fb_w, SEP_H, BAR_SEP);

    /* ── Left: "FiFi OS" ─────────────────────────────────────────────────── */
    bar_str(8u, "FiFi OS", BAR_ACCENT);

    /* ── Centre: IP address ──────────────────────────────────────────────── */
    char ip_str[24] = {0};
    size_t ip_len;
    if (net_ip == 0u) {
        ip_len = sb_cat(ip_str, 0, "no network");
    } else {
        size_t p = 0;
        char num[8];
        sb_udec(num, (net_ip >> 24) & 0xFFu); p = sb_cat(ip_str, p, num);
        ip_str[p++] = '.'; ip_str[p] = '\0';
        sb_udec(num, (net_ip >> 16) & 0xFFu); p = sb_cat(ip_str, p, num);
        ip_str[p++] = '.'; ip_str[p] = '\0';
        sb_udec(num, (net_ip >>  8) & 0xFFu); p = sb_cat(ip_str, p, num);
        ip_str[p++] = '.'; ip_str[p] = '\0';
        sb_udec(num,  net_ip        & 0xFFu); p = sb_cat(ip_str, p, num);
        ip_len = p;
    }
    uint64_t ip_px = (g_fb_w - (uint64_t)ip_len * 8u) / 2u;
    bar_str(ip_px, ip_str, BAR_FG);

    /* ── Right: uptime ───────────────────────────────────────────────────── */
    char up[24] = {0};
    size_t up_pos = 0;
    uint64_t secs = pit_ticks() / 100u;
    uint64_t mins = secs / 60u; secs %= 60u;
    uint64_t hrs  = mins / 60u; mins %= 60u;
    char num[8];
    if (hrs > 0u) {
        sb_udec(num, hrs);  up_pos = sb_cat(up, up_pos, num);
        up_pos = sb_cat(up, up_pos, "h ");
    }
    sb_udec(num, mins); up_pos = sb_cat(up, up_pos, num);
    up_pos = sb_cat(up, up_pos, "m ");
    sb_udec(num, secs); up_pos = sb_cat(up, up_pos, num);
    sb_cat(up, up_pos, "s");

    uint64_t up_px = g_fb_w - (uint64_t)sb_strlen(up) * 8u - 8u;
    bar_str(up_px, up, BAR_FG);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void statusbar_init(uint64_t fb_width) {
    g_fb_w = fb_width;
    console_set_y_offset(STATUS_H);
    bar_draw();
}

void statusbar_on_tick(void) {
    uint64_t now_sec = pit_ticks() / 100u;
    if (now_sec == g_last_sec) return;
    g_last_sec = now_sec;
    bar_draw();
}
