/*
 * splash.c — Boot splash screen shown once at startup before the shell prompt.
 *
 * Displays "FiFi OS" in large scaled text centred on screen, a subtitle,
 * a separator, and a neofetch-style info block.  Colors match the status bar.
 *
 * Layout (STATUS_H = 20, scale = 4, 1920×1080 example):
 *   y=20..43   — top margin (gap below status bar)
 *   y=44..107  — "FiFi OS" at 4× scale (64 px tall)
 *   y=108..123 — subtitle line (regular 16 px font)
 *   y=130..131 — separator line (2 px)
 *   cell row 8 — info block begins
 *   cell row 16 — "Type 'help'…" hint
 *   cell row 18 — shell prompt appears here
 */

#include "splash.h"
#include "console.h"
#include "net.h"
#include "pmm.h"

/* ── Colors — identical to statusbar.c ──────────────────────────────────── */
#define S_BG      0x001a1a2eu   /* dark navy background                     */
#define S_TITLE   0x00e8eeffu   /* bright accent — large title              */
#define S_LABEL   0x004888c8u   /* medium blue — info labels                */
#define S_VALUE   0x00c8d8ffu   /* light blue-white — info values           */
#define S_SEP     0x003060c0u   /* separator and subtle decorative lines    */
#define S_DIM     0x00607890u   /* dimmed — subtitle / hint text            */

#define STATUS_H  20u           /* must match statusbar.c                   */
#define SCALE     4u            /* title scale: each font pixel → 4×4 block */

/* ── String helpers ──────────────────────────────────────────────────────── */

static size_t sp_len(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static size_t sp_udec(char *buf, uint64_t v) {
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[20]; size_t n = 0;
    while (v) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    for (size_t i = 0; i < n; i++) buf[i] = tmp[n - 1u - i];
    buf[n] = '\0'; return n;
}

static size_t sp_cat(char *dst, size_t pos, const char *src) {
    for (size_t i = 0; src[i]; i++) dst[pos++] = src[i];
    dst[pos] = '\0'; return pos;
}

/* ── Pixel-coord string — bypasses cell buffer, used for centred lines ───── */
static void px_str(uint64_t px, uint64_t py, const char *s, uint32_t fg) {
    uint64_t cw = (uint64_t)console_font_width();
    for (size_t i = 0; s[i]; i++)
        console_render_glyph(px + (uint64_t)i * cw, py,
                             (unsigned char)s[i], fg, S_BG);
}

/* Centre a string horizontally in pixels. */
static uint64_t centre_x(uint64_t fw, const char *s) {
    uint64_t w = (uint64_t)sp_len(s) * (uint64_t)console_font_width();
    return (fw > w) ? (fw - w) / 2u : 4u;
}

/* ── Print one info row: label at fixed col, value right of it ───────────── */
#define LABEL_COL  6u
#define VALUE_COL  18u

static void info_row(uint32_t row, const char *label, const char *value) {
    console_set_cursor(LABEL_COL, row);
    console_set_colors(S_LABEL, S_BG);
    console_write(label);

    console_set_cursor(VALUE_COL, row);
    console_set_colors(S_VALUE, S_BG);
    console_write(value);
}

/* ── Public ──────────────────────────────────────────────────────────────── */

void splash_show(void) {
    uint64_t fw   = console_fb_width();
    uint64_t ffw  = (uint64_t)console_font_width();
    uint64_t ffh  = (uint64_t)console_font_height();
    uint64_t char_w = ffw * SCALE;
    uint64_t char_h = ffh * SCALE;

    /* Fill text area with the navy background and clear cell buffer */
    console_set_colors(S_TITLE, S_BG);
    console_clear();

    /* ── Large "FiFi OS" title ───────────────────────────────────────────── */
    const char *title = "FiFi OS";
    uint64_t    tlen  = (uint64_t)sp_len(title);
    uint64_t    tw    = tlen * char_w;
    uint64_t    tx    = (fw > tw) ? (fw - tw) / 2u : 4u;
    uint64_t    ty    = STATUS_H + 24u;   /* ~1.5 rows below status bar */

    for (uint64_t i = 0; i < tlen; i++)
        console_render_glyph_scaled(tx + i * char_w, ty,
                                    (unsigned char)title[i],
                                    SCALE, S_TITLE, S_BG);

    /* ── Subtitle — centred, dimmed ─────────────────────────────────────── */
    const char *sub   = "Alpha v4.0  |  x86_64  |  freestanding";
    uint64_t    sub_y = ty + char_h + 8u;
    px_str(centre_x(fw, sub), sub_y, sub, S_DIM);

    /* ── Separator line ──────────────────────────────────────────────────── */
    uint64_t sep_y = sub_y + 20u;
    uint64_t sep_x = fw / 5u;
    uint64_t sep_w = fw * 3u / 5u;
    console_fill_rect(sep_x, sep_y, sep_w, 2u, S_SEP);

    /* ── Info block — first cell row below separator ─────────────────────── */
    uint32_t row = (uint32_t)((sep_y + ffh / 2u - STATUS_H) / ffh);

    info_row(row++, "OS:",      "FiFi OS Alpha v4.0");
    info_row(row++, "Arch:",    "x86_64");
    info_row(row++, "Shell:",   "fifi shell");
    info_row(row++, "Kernel:",  "freestanding, Limine / UEFI");

    /* Memory */
    {
        char mem[24] = {0}; size_t p = 0;
        uint64_t mb = (pmm_get_total_pages() * 4096u) / (1024u * 1024u);
        char num[12]; sp_udec(num, mb); p = sp_cat(mem, p, num);
        sp_cat(mem, p, " MB");
        info_row(row++, "Memory:", mem);
    }

    /* Network */
    {
        char ip[24] = {0}; size_t p = 0;
        if (net_ip == 0u) {
            sp_cat(ip, 0, "not connected");
        } else {
            char num[8];
            sp_udec(num, (net_ip >> 24) & 0xFFu); p = sp_cat(ip, p, num);
            ip[p++] = '.'; ip[p] = '\0';
            sp_udec(num, (net_ip >> 16) & 0xFFu); p = sp_cat(ip, p, num);
            ip[p++] = '.'; ip[p] = '\0';
            sp_udec(num, (net_ip >>  8) & 0xFFu); p = sp_cat(ip, p, num);
            ip[p++] = '.'; ip[p] = '\0';
            sp_udec(num,  net_ip        & 0xFFu); sp_cat(ip, p, num);
        }
        info_row(row++, "Network:", ip);
    }


    /* ── Decorative colour chips ─────────────────────────────────────────── */
    row++;   /* blank row */
    {
        /* Draw small filled rectangles in the theme palette */
        uint64_t chip_y  = (uint64_t)(STATUS_H + row * 16u) + 3u;
        uint64_t chip_x  = (uint64_t)(LABEL_COL * 8u);
        uint64_t chip_w  = 14u;
        uint64_t chip_h  = 10u;
        uint32_t chips[] = { 0x001a1a2eu, 0x003060c0u, 0x004888c8u,
                              0x0090aad0u, 0x00c8d8ffu, 0x00e8eeffu };
        for (size_t ci = 0; ci < 6; ci++)
            console_fill_rect(chip_x + (uint64_t)ci * (chip_w + 4u),
                              chip_y, chip_w, chip_h, chips[ci]);
    }
    row++;

    /* ── Hint line ───────────────────────────────────────────────────────── */
    row++;
    console_set_cursor(LABEL_COL, row);
    console_set_colors(S_DIM, S_BG);
    console_write("Type 'help' to see available commands.");

    /* ── Leave cursor two rows below hint, reset to working colors ────────── */
    row += 2u;
    console_set_cursor(0u, row);
    console_set_colors(0x00ffffffu, S_BG);   /* white on navy for shell text */
}
