/* FiFi Terminal v2 — tabs, PSF2 Unicode, truecolor SGR, font zoom.
 * Build: gcc -O2 -static -o fifi-terminal terminal.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <poll.h>
#include <termios.h>
#include <signal.h>
#include <time.h>

/* ── IPC protocol ────────────────────────────────────────────────────────── */
#include "../../shared/app_ipc.h"

/* ── Grid limits ─────────────────────────────────────────────────────────── */
#define MAX_COLS   220
#define MAX_ROWS    60
#define SCROLLBACK 500
#define MAX_TABS     8

/* ── Default window ──────────────────────────────────────────────────────── */
#define DEF_WIN_W  840
#define DEF_WIN_H  540
#define TAB_BAR_H   28
#define PAD          4
/* The compositor draws its own SSD titlebar over the top TITLE_H px of every
 * native window's buffer (same convention as fifi-settings/filebrowser). Draw
 * our tab bar and grid BELOW that reserved strip so the compositor title never
 * hides the tabs. */
#define TITLE_H     24
#define CONTENT_Y   (TITLE_H + TAB_BAR_H)   /* terminal grid top */

/* ── Font paths (smallest → largest) ────────────────────────────────────── */
static const char *FONT_PATHS[3] = {
    "/fifi-data/fonts/ter16b.psf",
    "/fifi-data/fonts/ter20b.psf",
    "/fifi-data/fonts/ter24b.psf",
};
static int g_font_idx = 1;  /* default: ter20b (PSF2 + unicode table) */

/* ── Font ────────────────────────────────────────────────────────────────── */
typedef struct {
    uint8_t  *data;      /* raw glyph bitmaps */
    int       n;         /* glyph count */
    int       sz;        /* bytes per glyph */
    int       w, h;      /* pixel dimensions */
    /* Unicode table (PSF2 only) — sorted by codepoint for binary search */
    uint32_t *cps;       /* codepoints array */
    uint16_t *gis;       /* glyph indices array */
    int       nc;        /* entry count */
} Font;

static Font g_font;

static bool font_load(Font *f, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    fstat(fd, &st);
    uint8_t *raw = malloc((size_t)st.st_size);
    if (!raw) { close(fd); return false; }
    if (read(fd, raw, (size_t)st.st_size) < st.st_size) {
        free(raw); close(fd); return false;
    }
    close(fd);

    free(f->data); free(f->cps); free(f->gis);
    memset(f, 0, sizeof(*f));

    /* PSF2: magic 0x864ab572 */
    if (st.st_size >= 32 && raw[0]==0x72 && raw[1]==0xb5 && raw[2]==0x4a && raw[3]==0x86) {
        uint32_t version, hdr_size, flags, length, glyph_size, height, width;
        memcpy(&version,    raw+4,  4);
        memcpy(&hdr_size,   raw+8,  4);
        memcpy(&flags,      raw+12, 4);
        memcpy(&length,     raw+16, 4);
        memcpy(&glyph_size, raw+20, 4);
        memcpy(&height,     raw+24, 4);
        memcpy(&width,      raw+28, 4);
        (void)version;

        /* Validate header fields against the file size (64-bit math: the u32
         * products can wrap) so a truncated/corrupt font can't OOB-read. */
        uint64_t gtotal = (uint64_t)length * glyph_size;
        if (!length || !glyph_size || !width || !height ||
            hdr_size < 32 || (uint64_t)hdr_size + gtotal > (uint64_t)st.st_size ||
            (uint64_t)height * ((width + 7) / 8) > glyph_size) {
            free(raw); return false;
        }

        f->n  = (int)length;
        f->sz = (int)glyph_size;
        f->w  = (int)width;
        f->h  = (int)height;
        f->data = malloc((size_t)length * glyph_size);
        if (!f->data) { free(raw); return false; }
        memcpy(f->data, raw + hdr_size, (size_t)length * glyph_size);

        /* Parse Unicode table if present */
        if ((flags & 1) && hdr_size + gtotal < (uint64_t)st.st_size) {
            size_t tpos = (size_t)(hdr_size + gtotal);
            /* First pass: count entries */
            int cap = 2048, cnt = 0;
            uint32_t *tcps = malloc((size_t)cap * sizeof(uint32_t));
            uint16_t *tgis = malloc((size_t)cap * sizeof(uint16_t));
            if (!tcps || !tgis) {
                free(tcps); free(tgis); free(raw);
                free(f->data); memset(f, 0, sizeof(*f));
                return false;
            }

            uint16_t gi = 0;
            while (tpos < (size_t)st.st_size && gi < (uint16_t)length) {
                uint8_t b = raw[tpos];
                if (b == 0xFF) { gi++; tpos++; continue; }
                if (b == 0xFE) { tpos++; continue; } /* seq marker — skip, treat next cp normally */
                /* Decode UTF-8 codepoint */
                uint32_t cp = 0; size_t seq = 1;
                if      (b < 0x80) { cp = b; seq = 1; }
                else if (b < 0xE0) { cp = b & 0x1F; seq = 2; }
                else if (b < 0xF0) { cp = b & 0x0F; seq = 3; }
                else               { cp = b & 0x07; seq = 4; }
                for (size_t k = 1; k < seq && tpos+k < (size_t)st.st_size; k++)
                    cp = (cp << 6) | (raw[tpos+k] & 0x3F);
                tpos += seq;
                if (cnt >= cap) {
                    cap *= 2;
                    uint32_t *ncps = realloc(tcps, (size_t)cap * sizeof(uint32_t));
                    if (ncps) tcps = ncps;
                    uint16_t *ngis = realloc(tgis, (size_t)cap * sizeof(uint16_t));
                    if (ngis) tgis = ngis;
                    /* on failure keep the entries parsed so far */
                    if (!ncps || !ngis) break;
                }
                tcps[cnt] = cp;
                tgis[cnt] = gi;
                cnt++;
            }

            /* Sort by codepoint (insertion sort — small enough) */
            for (int i = 1; i < cnt; i++) {
                uint32_t kcp = tcps[i]; uint16_t kgi = tgis[i];
                int j = i - 1;
                while (j >= 0 && tcps[j] > kcp) {
                    tcps[j+1] = tcps[j]; tgis[j+1] = tgis[j]; j--;
                }
                tcps[j+1] = kcp; tgis[j+1] = kgi;
            }
            f->cps = tcps; f->gis = tgis; f->nc = cnt;
        }
    }
    /* PSF1: magic 0x0436 */
    else if (st.st_size >= 4 && raw[0]==0x36 && raw[1]==0x04) {
        uint8_t mode = raw[2], charsize = raw[3];
        f->n  = (mode & 1) ? 512 : 256;
        f->sz = charsize;
        f->w  = 8;
        f->h  = charsize;
        if (4 + (int64_t)f->n * f->sz > (int64_t)st.st_size) { free(raw); return false; }
        f->data = malloc((size_t)f->n * f->sz);
        if (!f->data) { free(raw); return false; }
        memcpy(f->data, raw + 4, (size_t)f->n * f->sz);
        /* No unicode table in PSF1 — ASCII glyphs map directly by codepoint */
    }
    else { free(raw); return false; }

    free(raw);
    return true;
}

/* Look up glyph index for a codepoint. Returns 0xFFFF if not found. */
static uint16_t font_glyph(const Font *f, uint32_t cp) {
    if (f->nc > 0) {
        /* Binary search in unicode table */
        int lo = 0, hi = f->nc - 1;
        while (lo <= hi) {
            int mid = (lo + hi) >> 1;
            if (f->cps[mid] == cp) return f->gis[mid];
            if (f->cps[mid]  < cp) lo = mid + 1;
            else                    hi = mid - 1;
        }
        return 0xFFFF;
    }
    /* PSF1 — direct index for ASCII */
    if (cp < (uint32_t)f->n) return (uint16_t)cp;
    return 0xFFFF;
}

/* ── Color helpers ───────────────────────────────────────────────────────── */
static const uint32_t ANSI_NORMAL[8] = {
    0xFF0e1418u, 0xFF993333u, 0xFF33993eu, 0xFF999933u,
    0xFF336699u, 0xFF993399u, 0xFF339999u, 0xFFd8e8f8u,
};
static const uint32_t ANSI_BRIGHT[8] = {
    0xFF506070u, 0xFFcc4444u, 0xFF44cc44u, 0xFFcccc44u,
    0xFF4488ccu, 0xFFcc44ccu, 0xFF44ccccu, 0xFFf8f8f8u,
};
static const uint32_t COL_FG_DEF = 0xFFd8e8f8u;
static const uint32_t COL_BG_DEF = 0xFF0e1418u;

static uint32_t color256(uint8_t n) {
    static const uint8_t cube6[6] = {0, 95, 135, 175, 215, 255};
    if (n < 8)   return ANSI_NORMAL[n];
    if (n < 16)  return ANSI_BRIGHT[n - 8];
    if (n >= 232) { uint8_t v = 8 + (uint8_t)((n-232)*10); return 0xFF000000u|(uint32_t)(v<<16)|(uint32_t)(v<<8)|v; }
    uint8_t i = n - 16;
    uint8_t b = i % 6; i /= 6;
    uint8_t g = i % 6; i /= 6;
    uint8_t r = i;
    return 0xFF000000u | ((uint32_t)cube6[r]<<16) | ((uint32_t)cube6[g]<<8) | cube6[b];
}

/* ── Cell ────────────────────────────────────────────────────────────────── */
typedef struct { uint32_t cp; uint32_t fg; uint32_t bg; } Cell;

/* ── Tab (one PTY session + display state) ───────────────────────────────── */
typedef struct {
    Cell     cells[MAX_ROWS][MAX_COLS];
    Cell     scrollback[SCROLLBACK][MAX_COLS];
    int      sb_w, sb_n, sb_off;  /* ring write ptr, line count, scroll offset */
    int      rows, cols, cx, cy;
    bool     wrap;   /* VT100 deferred wrap: last column written, wrap before next glyph */
    uint32_t fg, bg;
    bool     bold;
    bool     cur_vis, cur_hide, dirty;
    int      pty;
    pid_t    pid;
    char     title[64];
    /* ESC/CSI parser */
    bool     in_esc, in_csi, in_osc;
    char     csibuf[64];
    int      csilen;
    /* UTF-8 decoder */
    int      utf8r;
    uint32_t utf8cp;
    /* Saved cursor */
    int      svx, svy;
} Tab;

static Tab g_tabs[MAX_TABS];
static int g_n_tabs = 0;
static int g_active = 0;

/* ── Window state ────────────────────────────────────────────────────────── */
static int       g_win_w  = DEF_WIN_W;
static int       g_win_h  = DEF_WIN_H;
static uint32_t *g_fb     = NULL;
static int       g_ipc_fd = -1;
static uint8_t   g_prev_btns = 0;
/* Last known cursor position — used to draw hover feedback on the tab-bar
 * controls (+ / × / A- / A+). Updated on every IPC_INPUT_MOUSE event. */
static int       g_mouse_x   = -1;
static int       g_mouse_y   = -1;
static bool      g_in_tabbar = false;  /* cursor was over the tab bar last event */

/* ── FB helpers ──────────────────────────────────────────────────────────── */
static inline void fb_set(int x, int y, uint32_t c) {
    if ((unsigned)x < (unsigned)g_win_w && (unsigned)y < (unsigned)g_win_h)
        g_fb[y * g_win_w + x] = c;
}

static void fb_fill(int x, int y, int w, int h, uint32_t c) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > g_win_w) x1 = g_win_w;
    if (y1 > g_win_h) y1 = g_win_h;
    for (int r = y; r < y1; r++)
        for (int cc = x; cc < x1; cc++)
            g_fb[r * g_win_w + cc] = c;
}

static void fb_vline(int x, int y, int h, uint32_t c) { fb_fill(x, y, 1, h, c); }

/* Render one glyph at pixel position (px,py) using g_font */
static void fb_glyph(int px, int py, uint32_t cp, uint32_t fg, uint32_t bg) {
    const Font *f = &g_font;
    uint16_t gi = font_glyph(f, cp);
    if (gi == 0xFFFF) {
        /* Not in font — ASCII fallback */
        uint8_t asc = ' ';
        /* Minimal table for unmapped codepoints */
        if (cp >= 0x250C && cp <= 0x257F) asc = (cp==0x2500||cp==0x2501||cp==0x2504||cp==0x2508||cp==0x254C||cp==0x254E||cp==0x2550)?'-':(cp==0x2502||cp==0x2503||cp==0x2506||cp==0x250A||cp==0x254F||cp==0x2551)?'|':'+';
        else if (cp == 0x2571) asc = '/';
        else if (cp == 0x2572) asc = '\\';
        else if (cp == 0x2573) asc = 'X';
        else if (cp >= 0x2574 && cp <= 0x257F) asc = (cp & 1) ? '|' : '-';
        else if (cp >= 0x2580 && cp <= 0x259F) asc = '#';
        else if (cp == 0x2190 || cp == 0x21D0) asc = '<';
        else if (cp == 0x2192 || cp == 0x21D2) asc = '>';
        else if (cp == 0x2191 || cp == 0x21D1) asc = '^';
        else if (cp == 0x2193 || cp == 0x21D3) asc = 'v';
        else if (cp == 0x2022 || cp == 0x00B7 || cp == 0x2219) asc = '.';
        else if (cp == 0x2014 || cp == 0x2013) asc = '-';
        else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201C || cp == 0x201D) asc = '"';
        else if (cp == 0x2713 || cp == 0x2714 || cp == 0x2705) asc = '+';
        else if (cp == 0x2717 || cp == 0x2718 || cp == 0x274C) asc = 'x';
        else if (cp == 0x2026) asc = '.';
        else if (cp >= 0x00C0 && cp <= 0x00D6) asc = (uint8_t)(cp < 0x00C7 ? 'A' : cp == 0x00C7 ? 'C' : cp < 0x00CB ? 'E' : cp < 0x00D0 ? 'I' : cp == 0x00D0 ? 'D' : cp == 0x00D1 ? 'N' : 'O');
        else if (cp >= 0x00E0 && cp <= 0x00F6) asc = (uint8_t)(cp < 0x00E7 ? 'a' : cp == 0x00E7 ? 'c' : cp < 0x00EB ? 'e' : cp < 0x00F0 ? 'i' : cp == 0x00F0 ? 'd' : cp == 0x00F1 ? 'n' : 'o');
        else if (cp == 0x00A0) asc = ' ';
        else if (cp >= 0x00B8 && cp <= 0x00BF) asc = '?';
        else if (cp >= 0x2800 && cp <= 0x28FF) asc = '*';
        else if (cp >= 0x25A0 && cp <= 0x25FF) asc = (cp == 0x25CF || cp == 0x25CB) ? 'o' : (cp == 0x25B2 || cp == 0x25B3) ? '^' : (cp == 0x25BC || cp == 0x25BD) ? 'v' : (cp == 0x25B6) ? '>' : (cp == 0x25C0) ? '<' : '#';
        gi = font_glyph(f, asc);
        if (gi == 0xFFFF) gi = font_glyph(f, ' ');
        if (gi == 0xFFFF) gi = 0;
    }
    if (gi >= (uint16_t)f->n) gi = 0;
    const uint8_t *bits = f->data + (size_t)gi * f->sz;
    int bpr = (f->w + 7) / 8;
    for (int row = 0; row < f->h; row++) {
        for (int col = 0; col < f->w; col++) {
            uint32_t px2 = (bits[row * bpr + col / 8] & (0x80u >> (col & 7))) ? fg : bg;
            fb_set(px + col, py + row, px2);
        }
    }
}

/* Render a string, centered in a box of width bw at pixel (bx,by,bw,bh) */
static void fb_str_center(int bx, int by, int bw, int bh, const char *s, uint32_t fg, uint32_t bg) {
    int len = (int)strlen(s);
    int tw = len * (g_font.w + 1);
    int ox = bx + (bw - tw) / 2;
    int oy = by + (bh - g_font.h) / 2;
    if (ox < bx) ox = bx;
    fb_fill(bx, by, bw, bh, bg);
    /* Clip to box */
    for (int i = 0; s[i] && ox < bx + bw; i++, ox += g_font.w + 1)
        fb_glyph(ox, oy, (uint8_t)s[i], fg, bg);
}

/* ── Tab cell helpers ────────────────────────────────────────────────────── */
static void tab_clear_region(Tab *t, int r0, int c0, int r1, int c1) {
    for (int r = r0; r <= r1 && r < t->rows; r++)
        for (int c = c0; c < c1 && c < t->cols; c++)
            t->cells[r][c] = (Cell){ ' ', t->fg, t->bg };
}

static void tab_clear_all(Tab *t) { tab_clear_region(t, 0, 0, t->rows - 1, t->cols); }

static void tab_scroll_up(Tab *t) {
    memcpy(t->scrollback[t->sb_w], t->cells[0], sizeof(Cell) * MAX_COLS);
    t->sb_w = (t->sb_w + 1) % SCROLLBACK;
    if (t->sb_n < SCROLLBACK) t->sb_n++;
    memmove(&t->cells[0], &t->cells[1], sizeof(Cell) * MAX_COLS * (t->rows - 1));
    for (int c = 0; c < t->cols; c++)
        t->cells[t->rows - 1][c] = (Cell){ ' ', t->fg, t->bg };
}

static void tab_newline(Tab *t) {
    t->cx = 0; t->cy++;
    if (t->cy >= t->rows) { t->cy = t->rows - 1; tab_scroll_up(t); }
}

/* Get scrollback row (0=oldest visible, sb_n-1=newest) */
static Cell *tab_sb_row(Tab *t, int n, int col) {
    int ring = (t->sb_w - t->sb_n + n + SCROLLBACK * 2048) % SCROLLBACK;
    return &t->scrollback[ring][col];
}

/* ── Display width (East Asian Width) ───────────────────────────────────── */
static int cp_width(uint32_t cp) {
    if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF ||
        (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0489) ||
        (cp >= 0x0591 && cp <= 0x05BD) || (cp >= 0x0610 && cp <= 0x061A) ||
        (cp >= 0x064B && cp <= 0x065F) || (cp >= 0x06D6 && cp <= 0x06DC) ||
        (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
        (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F) ||
        (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF))
        return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) || cp == 0x2329 || cp == 0x232A ||
        (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF) ||
        (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
        (cp >= 0xA000 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE10 && cp <= 0xFE19) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F000 && cp <= 0x1FAFF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    /* BMP emoji with default emoji-presentation (width-2) */
    if (cp==0x231A||cp==0x231B||(cp>=0x23E9&&cp<=0x23EC)||cp==0x23F0||cp==0x23F3||
        cp==0x25FD||cp==0x25FE||cp==0x2614||cp==0x2615||(cp>=0x2648&&cp<=0x2653)||
        cp==0x267F||cp==0x2693||cp==0x26A1||cp==0x26AA||cp==0x26AB||cp==0x26BD||
        cp==0x26BE||cp==0x26C4||cp==0x26C5||cp==0x26CE||cp==0x26D4||cp==0x26EA||
        cp==0x26F2||cp==0x26F3||cp==0x26F5||cp==0x26FA||cp==0x26FD||cp==0x2705||
        cp==0x270A||cp==0x270B||cp==0x2728||cp==0x274C||cp==0x274E||
        (cp>=0x2753&&cp<=0x2755)||cp==0x2757||(cp>=0x2795&&cp<=0x2797)||
        cp==0x27B0||cp==0x27BF||cp==0x2B1B||cp==0x2B1C||cp==0x2B50||cp==0x2B55)
        return 2;
    return 1;
}

/* ── CSI / SGR parser ────────────────────────────────────────────────────── */


static void tab_handle_csi(Tab *t) {
    if (!t->in_csi || t->csilen == 0) goto done;
    char cmd = t->csibuf[t->csilen - 1];
    t->csibuf[t->csilen - 1] = '\0';
    const char *buf = t->csibuf;

    /* DEC private: ESC[?Nh / ESC[?Nl */
    if (buf[0] == '?') {
        int n = atoi(buf + 1);
        if (cmd == 'h') {
            if (n == 25) { t->cur_hide = false; t->cur_vis = true; }
            if (n == 1049) { tab_clear_all(t); t->cx = t->cy = 0; }
        } else if (cmd == 'l') {
            if (n == 25) { t->cur_hide = true; t->cur_vis = false; }
            if (n == 1049) { tab_clear_all(t); t->cx = t->cy = 0; }
        }
        goto done;
    }

    /* Cursor save/restore */
    if (cmd == 's') { t->svx = t->cx; t->svy = t->cy; goto done; }
    if (cmd == 'u') { t->cx = t->svx; t->cy = t->svy; goto done; }

    if (cmd == 'm') {
        /* SGR — parse all params, handle extended colors */
        const char *p = buf;
        while (*p || p == buf) {
            int n = (*p) ? atoi(p) : 0;
            while (*p && *p != ';') p++;
            if (*p == ';') p++;

            if (n == 0)  { t->fg = COL_FG_DEF; t->bg = COL_BG_DEF; t->bold = false; }
            else if (n == 1) { t->bold = true; }
            else if (n == 22) { t->bold = false; }
            else if (n >= 30 && n <= 37) t->fg = t->bold ? ANSI_BRIGHT[n-30] : ANSI_NORMAL[n-30];
            else if (n >= 90 && n <= 97) t->fg = ANSI_BRIGHT[n-90];
            else if (n >= 40 && n <= 47) t->bg = ANSI_NORMAL[n-40];
            else if (n >= 100 && n <= 107) t->bg = ANSI_BRIGHT[n-100];
            else if (n == 39) t->fg = COL_FG_DEF;
            else if (n == 49) t->bg = COL_BG_DEF;
            else if (n == 38 || n == 48) {
                int mode = atoi(p); while (*p && *p != ';') p++; if (*p == ';') p++;
                uint32_t col = (n == 38) ? COL_FG_DEF : COL_BG_DEF;
                if (mode == 5) {
                    uint8_t idx = (uint8_t)atoi(p); while (*p && *p != ';') p++; if (*p == ';') p++;
                    col = color256(idx);
                } else if (mode == 2) {
                    int r = atoi(p); while (*p && *p != ';') p++; if (*p == ';') p++;
                    int g2 = atoi(p); while (*p && *p != ';') p++; if (*p == ';') p++;
                    int b = atoi(p); while (*p && *p != ';') p++; if (*p == ';') p++;
                    col = 0xFF000000u | ((uint32_t)(r&255)<<16) | ((uint32_t)(g2&255)<<8) | (uint32_t)(b&255);
                }
                if (n == 38) t->fg = col; else t->bg = col;
            }
            if (!*p) break;
        }
    } else if (cmd == 'J') {
        int n = atoi(buf);
        if (n == 2 || n == 3) { tab_clear_all(t); t->cx = t->cy = 0; }
        else if (n == 0) {
            /* erase from cursor to end of screen: current row from cx, subsequent rows from col 0 */
            tab_clear_region(t, t->cy, t->cx, t->cy, t->cols);
            if (t->cy + 1 < t->rows) tab_clear_region(t, t->cy + 1, 0, t->rows - 1, t->cols);
        }
        else if (n == 1) { tab_clear_region(t, 0, 0, t->cy-1, t->cols); tab_clear_region(t, t->cy, 0, t->cy, t->cx+1); }
    } else if (cmd == 'K') {
        int n = atoi(buf);
        if (n == 0) for (int c=t->cx; c<t->cols; c++) t->cells[t->cy][c]=(Cell){' ',t->fg,t->bg};
        else if (n==1) for (int c=0; c<=t->cx; c++) t->cells[t->cy][c]=(Cell){' ',t->fg,t->bg};
        else for (int c=0; c<t->cols; c++) t->cells[t->cy][c]=(Cell){' ',t->fg,t->bg};
    } else if (cmd=='H' || cmd=='f') {
        int row=1, col=1;
        const char *sc = strchr(buf, ';');
        row = atoi(buf); if (sc) col = atoi(sc+1);
        t->cx = (col>1?col-1:0); if (t->cx>=t->cols) t->cx=t->cols-1;
        t->cy = (row>1?row-1:0); if (t->cy>=t->rows) t->cy=t->rows-1;
        t->wrap = false;
    } else if (cmd=='A') { int n=atoi(buf); t->cy -= n?n:1; if(t->cy<0) t->cy=0; t->wrap=false; }
      else if (cmd=='B') { int n=atoi(buf); t->cy += n?n:1; if(t->cy>=t->rows) t->cy=t->rows-1; t->wrap=false; }
      else if (cmd=='C') { int n=atoi(buf); t->cx += n?n:1; if(t->cx>=t->cols) t->cx=t->cols-1; t->wrap=false; }
      else if (cmd=='D') { int n=atoi(buf); t->cx -= n?n:1; if(t->cx<0) t->cx=0; t->wrap=false; }
      else if (cmd=='G') { int n=atoi(buf); t->cx=(n>1?n-1:0); if(t->cx>=t->cols) t->cx=t->cols-1; t->wrap=false; }
      else if (cmd=='d') { int n=atoi(buf); t->cy=(n>1?n-1:0); if(t->cy>=t->rows) t->cy=t->rows-1; t->wrap=false; }
      else if (cmd=='E') { int n=atoi(buf); t->cy += n?n:1; if(t->cy>=t->rows) t->cy=t->rows-1; t->cx=0; t->wrap=false; }
      else if (cmd=='F') { int n=atoi(buf); t->cy -= n?n:1; if(t->cy<0) t->cy=0; t->cx=0; t->wrap=false; }
    else if (cmd == 'P') {
        int n = atoi(buf); if (!n) n=1;
        int rem = t->cols - t->cx - n;
        if (rem>0) memmove(&t->cells[t->cy][t->cx], &t->cells[t->cy][t->cx+n], sizeof(Cell)*rem);
        for (int c=t->cols-n; c<t->cols; c++) t->cells[t->cy][c]=(Cell){' ',t->fg,t->bg};
    } else if (cmd == '@') {
        int n = atoi(buf); if (!n) n=1;
        int rem = t->cols - t->cx - n;
        if (rem>0) memmove(&t->cells[t->cy][t->cx+n], &t->cells[t->cy][t->cx], sizeof(Cell)*rem);
        for (int c=t->cx; c<t->cx+n && c<t->cols; c++) t->cells[t->cy][c]=(Cell){' ',t->fg,t->bg};
    } else if (cmd == 'X') {
        int n = atoi(buf); if (!n) n=1;
        for (int c=t->cx; c<t->cx+n && c<t->cols; c++) t->cells[t->cy][c]=(Cell){' ',t->fg,t->bg};
    } else if (cmd == 'L') {
        int n = atoi(buf); if (!n) n=1;
        for (int i=0; i<n; i++) {
            memmove(&t->cells[t->cy+1], &t->cells[t->cy], sizeof(Cell)*MAX_COLS*(t->rows-t->cy-1));
            tab_clear_region(t, t->cy, 0, t->cy, t->cols);
        }
    } else if (cmd == 'M') {
        int n = atoi(buf); if (!n) n=1;
        for (int i=0; i<n; i++) tab_scroll_up(t);
    } else if (cmd == 'r') {
        /* scroll region — accept but ignore for now */
    }
done:
    t->in_esc = t->in_csi = false; t->csilen = 0;
}

/* ── Main character receiver for one tab ────────────────────────────────── */
static void tab_putc(Tab *t, uint8_t c) {
    /* OSC: consume until BEL or ST */
    if (t->in_osc) {
        if (c == 0x07 || (c == 0x9C)) {
            /* BEL or ST terminates OSC — extract window title if OSC 0 or 2 */
            t->in_osc = false;
            char *semi = strchr(t->csibuf, ';');
            if (semi) {
                int osc_n = atoi(t->csibuf);
                if (osc_n == 0 || osc_n == 1 || osc_n == 2) {
                    strncpy(t->title, semi + 1, 63); t->title[63] = '\0';
                }
            }
            t->csilen = 0;
        } else if (c == 0x1B) {
            t->in_osc = false; t->in_esc = true; t->in_csi = false; t->csilen = 0;
        } else {
            if (t->csilen < 63) { t->csibuf[t->csilen++] = (char)c; t->csibuf[t->csilen] = '\0'; }
        }
        return;
    }
    if (t->in_esc) {
        if (!t->in_csi && c == '[') { t->in_csi = true; return; }
        if (!t->in_csi && c == ']') { t->in_osc = true; t->in_esc = false; t->csilen = 0; return; }
        if (!t->in_csi && c == 'M') {
            t->in_esc = false;
            if (t->cy > 0) { t->cy--; }
            else { memmove(&t->cells[1], &t->cells[0], sizeof(Cell)*MAX_COLS*(t->rows-1)); tab_clear_region(t,0,0,0,t->cols); }
            return;
        }
        if (!t->in_csi && (c=='7'||c=='s')) { t->svx=t->cx; t->svy=t->cy; t->in_esc=false; return; }
        if (!t->in_csi && (c=='8'||c=='u')) { t->cx=t->svx; t->cy=t->svy; t->in_esc=false; return; }
        if (!t->in_csi && c=='c') { tab_clear_all(t); t->cx=t->cy=0; t->fg=COL_FG_DEF; t->bg=COL_BG_DEF; t->in_esc=false; return; }
        if (!t->in_csi && c=='(') { t->in_esc=false; return; } /* charset designation — ignore */
        if (!t->in_csi && c==')') { t->in_esc=false; return; }
        if (t->in_csi) {
            /* -2: room for this byte + NUL (csilen-1 would write csibuf[64]) */
            if ((c>=0x40 && c<=0x7E) || t->csilen >= (int)sizeof(t->csibuf)-2) {
                t->csibuf[t->csilen++] = c;
                t->csibuf[t->csilen]   = '\0';
                tab_handle_csi(t);
            } else {
                t->csibuf[t->csilen++] = c;
            }
        } else {
            t->in_esc = false;
        }
        return;
    }

    if (c == 0x1B) { t->in_esc = true; t->in_csi = false; t->csilen = 0; return; }
    if (c == '\r') { t->cx = 0; t->wrap = false; return; }
    if (c == '\n') { t->wrap = false; tab_newline(t); return; }
    if (c == '\t') { t->wrap = false; t->cx = (t->cx + 8) & ~7; if (t->cx >= t->cols) t->cx = t->cols-1; return; }
    if (c == 0x08 || c == 0x7F) {
        t->wrap = false;
        if (t->cx > 0) { t->cx--; t->cells[t->cy][t->cx] = (Cell){' ', t->fg, t->bg}; }
        return;
    }
    if (c == 0x07) return; /* BEL */
    if (c < 0x20) return;

    /* UTF-8 multi-byte */
    if (c >= 0x80) {
        if (c >= 0xC0) {
            if      (c >= 0xF0) { t->utf8r = 3; t->utf8cp = c & 0x07u; }
            else if (c >= 0xE0) { t->utf8r = 2; t->utf8cp = c & 0x0Fu; }
            else                 { t->utf8r = 1; t->utf8cp = c & 0x1Fu; }
        } else if (t->utf8r > 0) {
            t->utf8cp = (t->utf8cp << 6) | (c & 0x3Fu);
            if (--t->utf8r == 0) {
                int w = cp_width(t->utf8cp);
                if (w != 0) {
                    if (t->wrap) {   /* deferred wrap from the previous glyph */
                        t->cx = 0; t->wrap = false;
                        if (++t->cy >= t->rows) { t->cy = t->rows-1; tab_scroll_up(t); }
                    }
                    if (w == 2 && t->cx + 1 >= t->cols) {   /* wide glyph won't fit last col */
                        t->cx = 0;
                        if (++t->cy >= t->rows) { t->cy = t->rows-1; tab_scroll_up(t); }
                    }
                    t->cells[t->cy][t->cx] = (Cell){ t->utf8cp, t->fg, t->bg };
                    if (w == 2) {
                        if (t->cx + 1 < t->cols) t->cells[t->cy][t->cx+1] = (Cell){ 0, t->fg, t->bg };
                        if (t->cx + 2 >= t->cols) { t->cx = t->cols - 1; t->wrap = true; }
                        else t->cx += 2;
                    } else {
                        if (t->cx + 1 >= t->cols) t->wrap = true;
                        else t->cx++;
                    }
                }
            }
        } else { t->utf8r = 0; }
        return;
    }

    /* Plain ASCII */
    if (t->wrap) {   /* deferred wrap from the previous glyph at the last column */
        t->cx = 0; t->wrap = false;
        if (++t->cy >= t->rows) { t->cy = t->rows-1; tab_scroll_up(t); }
    }
    t->cells[t->cy][t->cx] = (Cell){ c, t->fg, t->bg };
    if (t->cx + 1 >= t->cols) t->wrap = true;   /* stay in last col; wrap pending */
    else t->cx++;
}

/* ── Rendering ───────────────────────────────────────────────────────────── */

/* Tab bar colors */
#define COL_TABBAR_BG    0xFF0D1319u
#define COL_TAB_INACTIVE 0xFF14202Cu
#define COL_TAB_ACTIVE   0xFF1E3250u
#define COL_TAB_HOVER    0xFF203448u
#define COL_TAB_TEXT     0xFFb0c8e0u
#define COL_TAB_TEXTACT  0xFFe8f0f8u
#define COL_TAB_BORDER   0xFF2a4060u
#define COL_CLOSE_BTN    0xFF7c8ea0u
#define COL_CLOSE_HOVER  0xFFcc4444u
#define COL_BTN_BG       0xFF14202Cu
#define COL_BTN_TEXT     0xFF8090a0u
#define COL_ACCENT       0xFF409cffu   /* active-tab / hover accent */
#define COL_SEP          0xFF0a1018u

/* ── Tab-bar layout ──────────────────────────────────────────────────────────
 * Control geometry is computed in ONE place so rendering and hit-testing can
 * never drift apart. Widths chosen for comfortable, obvious click targets. */
#define TABBTN_PLUS_W  32   /* [+] new-tab button */
#define TABBTN_FONT_W  22   /* [A-] / [A+] each */
#define TAB_CLOSE_W    22   /* [×] hit region inside each tab (generous) */
#define TAB_MIN_W      48
#define TAB_MAX_W     180

typedef struct {
    int tw;         /* per-tab width */
    int close_w;    /* [×] hit width at the right of each tab */
    int plus_x;     /* left edge of [+] */
    int plus_w;     /* width of [+] */
    int fontdec_x;  /* left edge of [A-] */
    int fontinc_x;  /* left edge of [A+] */
    int fontbtn_w;  /* width of each font button */
} TabLayout;

static TabLayout tabbar_layout(void) {
    TabLayout L;
    L.plus_w    = TABBTN_PLUS_W;
    L.fontbtn_w = TABBTN_FONT_W;
    L.close_w   = TAB_CLOSE_W;
    int reserve = L.plus_w + 2 * L.fontbtn_w;
    int avail   = g_win_w - reserve;
    if (avail < 0) avail = 0;
    int tw = (g_n_tabs > 0) ? (avail / g_n_tabs) : TAB_MAX_W;
    if (tw > TAB_MAX_W) tw = TAB_MAX_W;
    if (tw < TAB_MIN_W) tw = TAB_MIN_W;
    L.tw        = tw;
    L.plus_x    = g_n_tabs * tw;
    L.fontinc_x = g_win_w - L.fontbtn_w;
    L.fontdec_x = g_win_w - 2 * L.fontbtn_w;
    return L;
}

/* Crisp 2px-thick × centered at (cx,cy) with arm radius r — drawn from lines so
 * it reads as a real close button regardless of the loaded bitmap font. */
static void draw_cross(int cx, int cy, int r, uint32_t col) {
    for (int i = -r; i <= r; i++) {
        fb_set(cx + i,     cy + i, col);
        fb_set(cx + i + 1, cy + i, col);
        fb_set(cx + i,     cy - i, col);
        fb_set(cx + i + 1, cy - i, col);
    }
}

/* Crisp 2px-thick + centered at (cx,cy) with arm radius r. */
static void draw_plus(int cx, int cy, int r, uint32_t col) {
    for (int i = -r; i <= r; i++) {
        fb_set(cx + i, cy,     col);
        fb_set(cx + i, cy + 1, col);
        fb_set(cx,     cy + i, col);
        fb_set(cx + 1, cy + i, col);
    }
}

static void render_tabbar(void) {
    /* Fill the whole top strip (reserved title area + tab bar). The compositor
     * paints its SSD titlebar over rows [0, TITLE_H); our tabs live below it. */
    fb_fill(0, 0, g_win_w, CONTENT_Y, COL_TABBAR_BG);

    TabLayout L = tabbar_layout();
    int tw      = L.tw;
    int close_w = L.close_w;
    bool bar_hot = (g_mouse_y >= TITLE_H && g_mouse_y < CONTENT_Y);
    int cy = TITLE_H + TAB_BAR_H / 2;

    int x = 0;
    for (int i = 0; i < g_n_tabs; i++) {
        bool tab_hover   = bar_hot && g_mouse_x >= x && g_mouse_x < x + tw;
        int  close_x     = x + tw - close_w;
        bool close_hover = tab_hover && g_mouse_x >= close_x;

        uint32_t tbg = (i == g_active) ? COL_TAB_ACTIVE
                     : tab_hover       ? COL_TAB_HOVER
                                       : COL_TAB_INACTIVE;
        uint32_t tfg = (i == g_active) ? COL_TAB_TEXTACT : COL_TAB_TEXT;
        fb_fill(x, TITLE_H, tw, TAB_BAR_H, tbg);
        /* Accent top bar for the active tab */
        if (i == g_active) fb_fill(x, TITLE_H, tw, 2, COL_ACCENT);
        /* Right separator */
        fb_vline(x + tw - 1, TITLE_H, TAB_BAR_H, COL_SEP);

        /* Title text — truncated to the space left of the close button */
        const char *title = g_tabs[i].title[0] ? g_tabs[i].title : "Terminal";
        int cw = g_font.w + 1;
        int text_w = tw - close_w - 4;
        int max_chars = text_w / cw;
        if (max_chars < 1) max_chars = 1;
        char label[65];
        strncpy(label, title, 64); label[64] = '\0';
        if ((int)strlen(label) > max_chars) { label[max_chars-1]='.'; label[max_chars]='\0'; }
        int ty = TITLE_H + (TAB_BAR_H - g_font.h) / 2;
        for (int ci = 0; label[ci] && x + 4 + ci * cw + cw <= close_x; ci++)
            fb_glyph(x + 4 + ci * cw, ty, (uint8_t)label[ci], tfg, tbg);

        /* [×] close button — generous hit area, red highlight on hover */
        if (close_hover) {
            fb_fill(close_x + 3, TITLE_H + 4, close_w - 6, TAB_BAR_H - 8, COL_CLOSE_HOVER);
            draw_cross(close_x + close_w / 2, cy, 4, 0xFFffffffu);
        } else {
            uint32_t xcol = (i == g_active) ? COL_TAB_TEXTACT : COL_CLOSE_BTN;
            draw_cross(close_x + close_w / 2, cy, 4, xcol);
        }

        x += tw;
    }

    /* [+] new-tab button */
    {
        bool hov = bar_hot && g_mouse_x >= L.plus_x && g_mouse_x < L.plus_x + L.plus_w;
        uint32_t bg  = hov ? COL_TAB_HOVER : COL_BTN_BG;
        uint32_t col = hov ? COL_ACCENT    : COL_BTN_TEXT;
        fb_fill(L.plus_x, TITLE_H, L.plus_w, TAB_BAR_H, bg);
        fb_vline(L.plus_x, TITLE_H, TAB_BAR_H, COL_SEP);
        draw_plus(L.plus_x + L.plus_w / 2, cy, 5, col);
    }

    /* Spacer between [+] and the font buttons */
    {
        int sp_x = L.plus_x + L.plus_w;
        if (L.fontdec_x > sp_x)
            fb_fill(sp_x, TITLE_H, L.fontdec_x - sp_x, TAB_BAR_H, COL_TABBAR_BG);
    }

    /* [A-] font size decrease */
    {
        bool hov = bar_hot && g_mouse_x >= L.fontdec_x && g_mouse_x < L.fontinc_x;
        uint32_t bg  = hov ? COL_TAB_HOVER : COL_BTN_BG;
        uint32_t col = hov ? COL_ACCENT    : COL_BTN_TEXT;
        fb_fill(L.fontdec_x, TITLE_H, L.fontbtn_w, TAB_BAR_H, bg);
        fb_vline(L.fontdec_x, TITLE_H, TAB_BAR_H, COL_SEP);
        fb_str_center(L.fontdec_x, TITLE_H, L.fontbtn_w, TAB_BAR_H, "A-", col, bg);
    }

    /* [A+] font size increase */
    {
        bool hov = bar_hot && g_mouse_x >= L.fontinc_x;
        uint32_t bg  = hov ? COL_TAB_HOVER : COL_BTN_BG;
        uint32_t col = hov ? COL_ACCENT    : COL_BTN_TEXT;
        fb_fill(L.fontinc_x, TITLE_H, L.fontbtn_w, TAB_BAR_H, bg);
        fb_vline(L.fontinc_x, TITLE_H, TAB_BAR_H, COL_SEP);
        fb_str_center(L.fontinc_x, TITLE_H, L.fontbtn_w, TAB_BAR_H, "A+", col, bg);
    }
}

static void render_content(Tab *t) {
    int cw = g_font.w + 1;
    int ch = g_font.h + 1;
    int ox = PAD;
    int oy = CONTENT_Y + PAD;
    int content_h = g_win_h - CONTENT_Y;

    fb_fill(0, CONTENT_Y, g_win_w, content_h, t->bg);

    for (int row = 0; row < t->rows; row++) {
        for (int col = 0; col < t->cols; col++) {
            Cell *ce;
            if (t->sb_off > 0 && row < t->sb_off) {
                int sn = t->sb_n - t->sb_off + row;
                if (sn < 0) { static Cell blank; blank = (Cell){' ', COL_FG_DEF, COL_BG_DEF}; ce = &blank; }
                else ce = tab_sb_row(t, sn, col < MAX_COLS ? col : MAX_COLS-1);
            } else {
                ce = &t->cells[row - t->sb_off][col];
            }
            int px = ox + col * cw;
            int py = oy + row * ch;
            if (px + cw > g_win_w - 14) break;  /* leave space for scrollbar */
            fb_fill(px, py, cw, ch, ce->bg);
            if (ce->cp && ce->cp != ' ')
                fb_glyph(px, py, ce->cp, ce->fg, ce->bg);
        }
    }

    /* Cursor: a full-height vertical bar (I-beam) at the LEFT edge of the cell.
     * Drawn solid (not gated on the blink flag) so it is always visible while
     * the prompt is on-screen — a blinking cursor is easy to miss and reads as
     * "no cursor". A bar rather than a filled block so it never covers the glyph
     * underneath (a block hid the character the cursor sat on, so arrowing left
     * over text looked like the characters were disappearing). */
    if (t->sb_off == 0 && !t->cur_hide &&
        t->cy >= 0 && t->cy < t->rows && t->cx >= 0 && t->cx < t->cols) {
        int px = ox + t->cx * cw;
        int py = oy + t->cy * ch;
        if (px + cw <= g_win_w - 14)
            fb_fill(px, py, 2, ch, t->fg);
    }

    /* Scrollbar */
    {
        int sb_x = g_win_w - 14;
        int sb_y = CONTENT_Y + 4;
        int sb_h = g_win_h - CONTENT_Y - 8;
        fb_fill(sb_x, sb_y, 14, sb_h, 0xFF1a2a3au);
        if (t->sb_n > 0) {
            int total = t->rows + t->sb_n;
            int thumb = sb_h * t->rows / total;
            if (thumb < 8) thumb = 8;
            int ty = sb_y + (sb_h - thumb) * (t->sb_n - t->sb_off) / t->sb_n;
            if (ty < sb_y) ty = sb_y;
            if (ty + thumb > sb_y + sb_h) ty = sb_y + sb_h - thumb;
            fb_fill(sb_x + 2, ty, 10, thumb, 0xFF4a6080u);
        }
    }
}

static void render(void) {
    if (!g_fb) return;
    render_tabbar();
    if (g_n_tabs > 0) render_content(&g_tabs[g_active]);
}

/* ── IPC helpers ─────────────────────────────────────────────────────────── */
static void ipc_send(int fd, uint32_t type, const void *data, uint32_t len) {
    (void)fifi_app_ipc_send(fd, type, data, len);
}

static void send_frame(int fd) {
    if (!g_fb) return;
    (void)fifi_app_ipc_send_frame(fd, (uint16_t)g_win_w, (uint16_t)g_win_h, g_fb);
}

/* ── Font size change ────────────────────────────────────────────────────── */
static void font_change(int delta);  /* forward decl */

/* ── Tab grid recalculation ──────────────────────────────────────────────── */
static void tab_recalc_grid(Tab *t) {
    int cw = g_font.w + 1, ch = g_font.h + 1;
    int new_cols = (g_win_w - 14 - 2*PAD) / cw;
    int new_rows = (g_win_h - CONTENT_Y - 2*PAD) / ch;
    if (new_cols < 10) new_cols = 10;
    if (new_rows <  3) new_rows  = 3;
    if (new_cols > MAX_COLS) new_cols = MAX_COLS;
    if (new_rows > MAX_ROWS) new_rows  = MAX_ROWS;
    if (new_cols == t->cols && new_rows == t->rows) return;

    if (t->cx >= new_cols) t->cx = new_cols - 1;
    if (t->cy >= new_rows) t->cy = new_rows - 1;
    /* Fill any newly exposed cells */
    int old_cols = t->cols, old_rows = t->rows;
    t->cols = new_cols; t->rows = new_rows;
    for (int r = 0; r < t->rows; r++) {
        int c0 = (r < old_rows) ? old_cols : 0;
        for (int c = c0; c < t->cols; c++)
            t->cells[r][c] = (Cell){ ' ', t->fg, t->bg };
    }
    /* Update PTY window size */
    if (t->pty >= 0) {
        struct winsize ws = { .ws_row=(uint16_t)t->rows, .ws_col=(uint16_t)t->cols };
        ioctl(t->pty, TIOCSWINSZ, &ws);
        if (t->pid > 0) kill(-(t->pid), SIGWINCH);
    }
}

/* ── New tab ─────────────────────────────────────────────────────────────── */
static int tab_spawn(Tab *t) {
    t->pty = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (t->pty < 0) return -1;
    if (grantpt(t->pty) < 0 || unlockpt(t->pty) < 0) { close(t->pty); t->pty=-1; return -1; }
    char *slave = ptsname(t->pty);
    if (!slave) { close(t->pty); t->pty=-1; return -1; }

    struct winsize ws = { .ws_row=(uint16_t)t->rows, .ws_col=(uint16_t)t->cols };
    ioctl(t->pty, TIOCSWINSZ, &ws);

    t->pid = fork();
    if (t->pid < 0) { close(t->pty); t->pty=-1; return -1; }
    if (t->pid == 0) {
        setsid();
        int sl = open(slave, O_RDWR);
        if (sl < 0) _exit(1);
        ioctl(sl, TIOCSCTTY, 0);
        dup2(sl, 0); dup2(sl, 1); dup2(sl, 2);
        if (sl > 2) close(sl);
        setenv("TERM",  "xterm-256color", 1);
        /* Use C.UTF-8 — always present in glibc, so bash never prints
         * "setlocale: LC_ALL: cannot change locale (en_US.UTF-8)". Override
         * (overwrite=1) any inherited en_US.UTF-8 and clear the per-category
         * LC_* / LANGUAGE vars so nothing stale re-triggers the warning. */
        unsetenv("LANGUAGE");
        unsetenv("LC_CTYPE");   unsetenv("LC_MESSAGES"); unsetenv("LC_COLLATE");
        unsetenv("LC_NUMERIC"); unsetenv("LC_TIME");     unsetenv("LC_MONETARY");
        setenv("LANG",  "C.UTF-8", 1);
        setenv("LC_ALL","C.UTF-8", 1);
        setenv("HOME",  "/fifi-data/home", 1);
        setenv("PATH",  "/usr/local/bin:/fifi-data/bin:/bin:/sbin:/usr/bin:/usr/sbin", 1);
        setenv("USER",  "fifi",           1);
        setenv("SHELL", "/bin/sh",        1);
        setenv("PS1",   "fifi@FiFiOS:\\w$ ", 1);
        execl("/bin/ush", "-ush", NULL);
        execl("/bin/bash", "-bash", NULL);
        execl("/bin/sh",  "-sh",   NULL);
        execl("/bin/busybox", "sh", NULL);
        _exit(1);
    }
    fcntl(t->pty, F_SETFL, O_NONBLOCK);
    return 0;
}

static void tab_new(void) {
    if (g_n_tabs >= MAX_TABS) return;
    Tab *t = &g_tabs[g_n_tabs];
    memset(t, 0, sizeof(*t));
    t->fg = COL_FG_DEF; t->bg = COL_BG_DEF;
    t->cur_vis = true; t->dirty = true; t->pty = -1;
    snprintf(t->title, sizeof(t->title), "Terminal %d", g_n_tabs + 1);

    /* Calculate grid */
    int cw = g_font.w + 1, ch = g_font.h + 1;
    t->cols = (g_win_w - 14 - 2*PAD) / cw;
    t->rows = (g_win_h - CONTENT_Y - 2*PAD) / ch;
    if (t->cols < 10) t->cols = 10;
    if (t->rows <  3) t->rows  = 3;
    if (t->cols > MAX_COLS) t->cols = MAX_COLS;
    if (t->rows > MAX_ROWS) t->rows  = MAX_ROWS;

    tab_clear_all(t);
    tab_spawn(t);

    g_n_tabs++;
    g_active = g_n_tabs - 1;
}

static void tab_close(int idx) {
    if (idx < 0 || idx >= g_n_tabs) return;
    Tab *t = &g_tabs[idx];
    if (t->pid > 0) kill(t->pid, SIGTERM);
    if (t->pty >= 0) close(t->pty);
    t->pty = -1; t->pid = -1;
    /* Shift remaining tabs */
    for (int i = idx; i < g_n_tabs - 1; i++)
        g_tabs[i] = g_tabs[i+1];
    g_n_tabs--;
    if (g_n_tabs == 0) {
        /* Last tab closed — send IPC_APP_CLOSE */
        ipc_send(g_ipc_fd, IPC_APP_CLOSE, NULL, 0);
        return;
    }
    if (g_active >= g_n_tabs) g_active = g_n_tabs - 1;
}

/* ── Mouse tab-bar hit detection ─────────────────────────────────────────── */
static void handle_tabbar_click(int rx, int ry, uint8_t btns) {
    bool click = (btns & 1) && !(g_prev_btns & 1);
    if (!click) return;
    if (ry < TITLE_H || ry >= CONTENT_Y) return;

    /* Same geometry the tab bar was drawn with — no drift possible. */
    TabLayout L = tabbar_layout();

    /* [A-] / [A+] font buttons (checked first: they sit at the far right) */
    if (rx >= L.fontdec_x && rx < L.fontinc_x) { font_change(-1); return; }
    if (rx >= L.fontinc_x)                      { font_change(+1); return; }

    /* [+] new-tab button */
    if (rx >= L.plus_x && rx < L.plus_x + L.plus_w) { tab_new(); return; }

    /* Tab labels and [×] close buttons */
    for (int i = 0; i < g_n_tabs; i++) {
        int tx0 = i * L.tw;
        if (rx >= tx0 && rx < tx0 + L.tw) {
            int close_x = tx0 + L.tw - L.close_w;
            if (rx >= close_x) {
                tab_close(i);
            } else {
                g_active = i;
                g_tabs[i].dirty = true;
            }
            return;
        }
    }
}

/* ── Font size change ────────────────────────────────────────────────────── */
static void font_change(int delta) {
    int new_idx = g_font_idx + delta;
    if (new_idx < 0) new_idx = 0;
    if (new_idx > 2) new_idx = 2;
    if (new_idx == g_font_idx) return;
    Font nf = {0};
    if (!font_load(&nf, FONT_PATHS[new_idx])) return;
    free(g_font.data); free(g_font.cps); free(g_font.gis);
    g_font = nf;
    g_font_idx = new_idx;
    /* Recalculate all tabs (fb size is unchanged; render repaints it fully) */
    for (int i = 0; i < g_n_tabs; i++) {
        tab_recalc_grid(&g_tabs[i]);
        g_tabs[i].dirty = true;
    }
}

/* ── Resize all tabs ─────────────────────────────────────────────────────── */
static void resize_to(int nw, int nh) {
    if (nw == g_win_w && nh == g_win_h) return;
    g_win_w = nw; g_win_h = nh;
    free(g_fb);
    g_fb = malloc((size_t)g_win_w * g_win_h * 4);
    for (int i = 0; i < g_n_tabs; i++) {
        tab_recalc_grid(&g_tabs[i]);
        g_tabs[i].dirty = true;
    }
}

/* ── Key → PTY translation ───────────────────────────────────────────────── */
static void key_to_pty(Tab *t, uint8_t k) {
    if (t->pty < 0) return;
    if (k >= 0x20 && k < 0x7F) { write(t->pty, &k, 1); return; }
    switch (k) {
    case 0x0D: case '\n': { uint8_t nl='\r'; write(t->pty, &nl, 1); break; }
    case 0x08: case 0x7F: { uint8_t bs=0x7F; write(t->pty, &bs, 1); break; }
    case 0x03: write(t->pty, "\x03", 1); break;
    case 0x04: write(t->pty, "\x04", 1); break;
    case 0x0C: write(t->pty, "\x0C", 1); break;
    case 0x1A: write(t->pty, "\x1A", 1); break;
    case 0x1B: write(t->pty, "\x1B", 1); break;
    case 0x09: write(t->pty, "\x09", 1); break;
    /* Arrow keys */
    case 0x80: write(t->pty, "\x1B[D", 3); break;
    case 0x81: write(t->pty, "\x1B[C", 3); break;
    case 0x82: write(t->pty, "\x1B[A", 3); break;
    case 0x83: write(t->pty, "\x1B[B", 3); break;
    /* Navigation */
    case 0x84: write(t->pty, "\x1B[3~", 4); break;  /* Delete */
    case 0x85: write(t->pty, "\x1B[H",  3); break;  /* Home */
    case 0x86: write(t->pty, "\x1B[F",  3); break;  /* End */
    case 0x87: write(t->pty, "\x1B[5~", 4); break;  /* PgUp */
    case 0x88: write(t->pty, "\x1B[6~", 4); break;  /* PgDn */
    case 0x89: write(t->pty, "\x1B\t",  2); break;  /* Alt+Tab */
    /* F1-F12 */
    case 0x8A: write(t->pty, "\x1BOP",    3); break;
    case 0x8B: write(t->pty, "\x1BOQ",    3); break;
    case 0x8C: write(t->pty, "\x1BOR",    3); break;
    case 0x8D: write(t->pty, "\x1BOS",    3); break;
    case 0x8E: write(t->pty, "\x1B[15~",  5); break;
    case 0x8F: write(t->pty, "\x1B[17~",  5); break;
    case 0x90: write(t->pty, "\x1B[18~",  5); break;
    case 0x91: write(t->pty, "\x1B[19~",  5); break;
    case 0x92: write(t->pty, "\x1B[20~",  5); break;
    case 0x93: write(t->pty, "\x1B[21~",  5); break;
    case 0x94: write(t->pty, "\x1B[23~",  5); break;
    case 0x95: write(t->pty, "\x1B[24~",  5); break;
    default:
        if (k < 0x20) write(t->pty, &k, 1);
        break;
    }
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void) {
    /* Load default font */
    if (!font_load(&g_font, FONT_PATHS[g_font_idx])) {
        /* Try falling back to PSF1 */
        g_font_idx = 0;
        if (!font_load(&g_font, FONT_PATHS[0])) return 1;
    }

    /* Allocate framebuffer */
    g_fb = malloc((size_t)DEF_WIN_W * DEF_WIN_H * 4);
    if (!g_fb) return 1;

    /* Connect to compositor */
    int sock = fifi_app_ipc_connect(DEF_WIN_W, DEF_WIN_H, "Terminal");
    if (sock < 0) return 1;
    g_ipc_fd = sock;

    {
        uint8_t hdr[8] = {0};
        if (read(sock, hdr, 8) < 8) { close(sock); return 1; }
        uint32_t tp, pl;
        memcpy(&tp, hdr, 4); memcpy(&pl, hdr+4, 4);
        if (tp == IPC_WIN_CREATED && pl >= 4) {
            uint8_t resp[20]; ssize_t rn = read(sock, resp, pl > 20 ? 20 : pl); (void)rn;
        }
    }

    signal(SIGPIPE, SIG_IGN);

    /* Open first tab */
    tab_new();

    render();
    send_frame(sock);

    /* ── Event loop ── */
    uint8_t  in_hdr[8];
    int      in_got  = 0;
    uint8_t *in_pld  = NULL;
    uint32_t in_plen = 0, in_pgot = 0, in_type = 0;
    bool     running = true;

    while (running) {
        /* Build poll fd set: compositor + all active PTY fds */
        struct pollfd pfds[MAX_TABS + 1];
        int nfds = 0;
        pfds[nfds].fd = sock; pfds[nfds].events = POLLIN; pfds[nfds].revents = 0; nfds++;
        int tab_pfd_base = nfds;
        for (int i = 0; i < g_n_tabs; i++) {
            pfds[nfds].fd = (g_tabs[i].pty >= 0) ? g_tabs[i].pty : -1;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        int pn = poll(pfds, nfds, 16);
        if (pn < 0 && errno == EINTR) continue;

        /* ── Compositor messages ── */
        if (pfds[0].revents & POLLIN) {
            uint8_t tbuf[4096];
            ssize_t nr = read(sock, tbuf, sizeof(tbuf));
            if (nr <= 0) { running = false; break; }
            ssize_t pos = 0;
            while (pos < nr) {
                if (in_got < 8) {
                    in_hdr[in_got++] = tbuf[pos++];
                    if (in_got == 8) {
                        memcpy(&in_type, in_hdr,     4);
                        memcpy(&in_plen, in_hdr + 4, 4);
                        if (in_plen > 262144) { in_got = 0; break; }
                        if (in_plen > 0) { free(in_pld); in_pld = malloc(in_plen); in_pgot = 0; }
                        else {
                            /* zero-payload message: handle now (the compositor
                             * sends INVALIDATE with no payload; it was dropped) */
                            if (in_type == IPC_INVALIDATE && g_n_tabs > 0)
                                g_tabs[g_active].dirty = true;
                            in_got = 0;
                        }
                    }
                } else if (in_plen > 0 && in_pgot < in_plen) {
                    uint32_t need = in_plen - in_pgot;
                    uint32_t have = (uint32_t)(nr - pos);
                    uint32_t take = need < have ? need : have;
                    if (in_pld) memcpy(in_pld + in_pgot, tbuf + pos, take);
                    in_pgot += take; pos += take;
                    if (in_pgot >= in_plen) {
                        Tab *at = (g_n_tabs > 0) ? &g_tabs[g_active] : NULL;

                        if (in_type == IPC_INPUT_KEY && in_plen >= 1 && at) {
                            uint8_t key = in_pld ? in_pld[0] : 0;

                            /* Terminal-level shortcuts — intercepted before PTY */
                            if (key == 0x14u) {          /* Ctrl+T: new tab */
                                tab_new();
                                for (int i = 0; i < g_n_tabs; i++) g_tabs[i].dirty = true;
                            } else if (key == 0x17u) {   /* Ctrl+W: close tab */
                                tab_close(g_active);
                                if (g_n_tabs > 0) g_tabs[g_active].dirty = true;
                            } else if (key == 0x91u) {   /* F8: previous tab */
                                g_active = (g_active > 0) ? g_active - 1 : g_n_tabs - 1;
                                g_tabs[g_active].dirty = true;
                            } else if (key == 0x92u) {   /* F9: next tab */
                                g_active = (g_active + 1) % g_n_tabs;
                                g_tabs[g_active].dirty = true;
                            } else if (key == 0x87u) {   /* PgUp: scroll back */
                                at->sb_off += at->rows / 2;
                                if (at->sb_off > at->sb_n) at->sb_off = at->sb_n;
                                at->dirty = true;
                            } else if (key == 0x88u) {   /* PgDn: scroll forward */
                                at->sb_off -= at->rows / 2;
                                if (at->sb_off < 0) at->sb_off = 0;
                                at->dirty = true;
                            } else {
                                if (at->sb_off > 0) { at->sb_off = 0; at->dirty = true; }
                                if (key == 0x16u) {      /* Ctrl+V: paste */
                                    ipc_send(sock, IPC_CLIP_GET, NULL, 0);
                                } else {
                                    key_to_pty(at, key);
                                }
                            }
                        } else if (in_type == IPC_INPUT_MOUSE && in_plen >= 10 && in_pld) {
                            int32_t rx, ry; uint8_t btns; int8_t wheel;
                            memcpy(&rx, in_pld, 4); memcpy(&ry, in_pld+4, 4);
                            btns  = in_pld[8];
                            wheel = (int8_t)in_pld[9];

                            /* Track cursor for tab-bar hover feedback */
                            g_mouse_x = rx; g_mouse_y = ry;

                            if (ry >= TITLE_H && ry < CONTENT_Y) {
                                handle_tabbar_click(rx, ry, btns);
                                /* Repaint so hover state on +/×/A-/A+ updates */
                                for (int i = 0; i < g_n_tabs; i++) g_tabs[i].dirty = true;
                                g_in_tabbar = true;
                            } else {
                                /* Just left the tab bar — repaint once to clear hover */
                                if (g_in_tabbar) {
                                    for (int i = 0; i < g_n_tabs; i++) g_tabs[i].dirty = true;
                                    g_in_tabbar = false;
                                }
                                if (wheel != 0 && at) {
                                    at->sb_off += wheel * 3;
                                    if (at->sb_off < 0) at->sb_off = 0;
                                    if (at->sb_off > at->sb_n) at->sb_off = at->sb_n;
                                    at->dirty = true;
                                }
                            }
                            g_prev_btns = btns;
                        } else if (in_type == IPC_CLIP_DATA && in_plen > 0 && at && at->pty >= 0) {
                            if (in_pld) write(at->pty, in_pld, in_plen);
                        } else if (in_type == IPC_WIN_RESIZE && in_plen >= 4 && in_pld) {
                            uint16_t nw, nh;
                            memcpy(&nw, in_pld, 2); memcpy(&nh, in_pld+2, 2);
                            if (nw <= 16384 && nh <= 16384)
                                resize_to((int)nw, (int)nh);
                        } else if (in_type == IPC_INVALIDATE) {
                            if (at) at->dirty = true;
                        }

                        free(in_pld); in_pld = NULL;
                        in_got = 0; in_plen = 0; in_pgot = 0;
                    }
                } else {
                    in_got = 0; in_plen = 0; in_pgot = 0;
                }
            }
        }
        if (pfds[0].revents & (POLLHUP | POLLERR)) { running = false; break; }

        /* ── PTY output for each tab ── */
        for (int i = 0; i < g_n_tabs; i++) {
            int fi = tab_pfd_base + i;
            if (fi >= nfds) break;
            if (g_tabs[i].pty >= 0 && (pfds[fi].revents & POLLIN)) {
                uint8_t buf[1024];
                ssize_t nr2;
                while ((nr2 = read(g_tabs[i].pty, buf, sizeof(buf))) > 0) {
                    for (ssize_t j = 0; j < nr2; j++) tab_putc(&g_tabs[i], buf[j]);
                    g_tabs[i].dirty = true;
                }
            }
            if (g_tabs[i].pid > 0) {
                int wstat = 0;
                if (waitpid(g_tabs[i].pid, &wstat, WNOHANG) > 0) {
                    g_tabs[i].pid = -1;
                    const char *ex = "\r\n[Process exited]\r\n";
                    for (const char *p = ex; *p; p++) tab_putc(&g_tabs[i], (uint8_t)*p);
                    g_tabs[i].dirty = true;
                }
            }
        }

        /* ── Redraw if any tab is dirty (titles show in the tab bar) ── */
        bool need_render = false;
        for (int i = 0; i < g_n_tabs; i++) if (g_tabs[i].dirty) { need_render = true; break; }

        if (need_render && g_fb) {
            render();
            send_frame(sock);
            for (int i = 0; i < g_n_tabs; i++) g_tabs[i].dirty = false;
        }
    }

    for (int i = 0; i < g_n_tabs; i++) {
        if (g_tabs[i].pid > 0) kill(g_tabs[i].pid, SIGTERM);
        if (g_tabs[i].pty >= 0) close(g_tabs[i].pty);
    }
    ipc_send(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock);
    free(g_fb);
    free(g_font.data); free(g_font.cps); free(g_font.gis);
    return 0;
}
