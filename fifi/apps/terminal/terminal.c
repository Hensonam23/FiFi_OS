/* FiFi Terminal — standalone IPC process with dynamic resize + scrollback.
 * Spawns a PTY shell (/bin/sh) and provides a VT100 terminal window.
 * Window adapts its COLS/ROWS when the compositor resizes or snaps it.
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
#define FIFI_SOCK        "/tmp/fifi-compositor.sock"
#define IPC_APP_CONNECT  0x01u
#define IPC_APP_FRAME    0x02u
#define IPC_APP_TITLE    0x03u
#define IPC_APP_CLOSE    0x04u
#define IPC_WIN_CREATED  0x10u
#define IPC_INPUT_KEY    0x11u
#define IPC_INPUT_MOUSE  0x12u
#define IPC_INVALIDATE   0x15u
#define IPC_CLIP_GET     0x18u
#define IPC_CLIP_DATA    0x19u
#define IPC_WIN_RESIZE   0x1Bu

/* ── Maximum grid bounds (static arrays) ────────────────────────────────── */
#define MAX_COLS   220
#define MAX_ROWS    60
#define SCROLLBACK 300

/* ── Default window size ─────────────────────────────────────────────────── */
#define DEF_WIN_W  640
#define DEF_WIN_H  400
#define TITLE_H     24
#define PAD          4

/* ── Font (PSF1) ─────────────────────────────────────────────────────────── */
#define PSF1_MAGIC 0x0436u
typedef struct { uint16_t magic; uint8_t mode; uint8_t charsize; } Psf1Hdr;

static uint8_t *g_glyph   = NULL;
static int      g_glyph_h = 16;
static int      g_glyph_w = 8;
static int      g_n_glyph = 256;

static bool font_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    Psf1Hdr hdr;
    if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) || hdr.magic != PSF1_MAGIC) {
        close(fd); return false;
    }
    g_glyph_h = hdr.charsize;
    g_n_glyph = (hdr.mode & 1) ? 512 : 256;
    int total  = g_n_glyph * g_glyph_h;
    g_glyph    = malloc((size_t)total);
    if (!g_glyph || read(fd, g_glyph, total) < total) {
        free(g_glyph); g_glyph = NULL; close(fd); return false;
    }
    close(fd);
    return true;
}

/* ── Terminal state ──────────────────────────────────────────────────────── */
typedef struct { uint8_t ch; uint32_t fg; uint32_t bg; } Cell;

static Cell     g_cells[MAX_ROWS][MAX_COLS];
static Cell     g_scrollback[SCROLLBACK][MAX_COLS]; /* ring buffer */
static int      g_sb_write     = 0;
static int      g_sb_count     = 0;
static int      g_scroll_offset = 0;   /* 0=live, N=scrolled back N lines */

static int      g_rows = 23, g_cols = 80;   /* active grid (≤ MAX) */
static int      g_win_w = DEF_WIN_W, g_win_h = DEF_WIN_H;
static int      g_cx = 0, g_cy = 0;
static uint32_t g_fg = 0xFFd8e8f8u;
static uint32_t g_bg = 0xFF0e1418u;
static bool     g_cursor_vis  = true;
static bool     g_cursor_hide = false;  /* ESC[?25l sets this; blink respects it */
static bool     g_dirty = true;
static int      g_pty_master = -1;
static pid_t    g_child_pid  = -1;

/* ESC sequence parser */
static bool  g_esc         = false;
static bool  g_esc_bracket  = false;
static bool  g_in_osc       = false;
static char  g_esc_buf[64];
static int   g_esc_len      = 0;

/* UTF-8 decoder */
static int      g_utf8_remain = 0;
static uint32_t g_utf8_cp     = 0;

/* Saved cursor */
static int   g_saved_cx = 0, g_saved_cy = 0;

/* Display width of a codepoint in terminal columns: 0, 1, or 2. The program
 * driving us (e.g. Claude Code) lays out its UI by counting Unicode
 * East-Asian-Width + emoji; if our cursor advance disagrees, every line that
 * follows drifts and text overlaps. So we must match it per codepoint:
 *   0 — combining marks, joiners, variation selectors (attach to prev cell)
 *   2 — East-Asian-Wide / fullwidth / emoji-presentation glyphs
 *   1 — everything else.
 * (A previous version only flagged a few wide ranges and advanced 1 for
 *  combining marks and BMP emoji — that under-advance is what overlapped.) */
static int cp_width(uint32_t cp) {
    /* Zero-width: combining marks, joiners, variation selectors, BOM/ZWSP */
    if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF ||
        (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0489) ||
        (cp >= 0x0591 && cp <= 0x05BD) || (cp >= 0x0610 && cp <= 0x061A) ||
        (cp >= 0x064B && cp <= 0x065F) || (cp >= 0x06D6 && cp <= 0x06DC) ||
        (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
        (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F) ||
        (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF))
        return 0;
    /* Wide: East-Asian Wide & Fullwidth + supplementary-plane emoji */
    if ((cp >= 0x1100 && cp <= 0x115F) ||   /* Hangul Jamo */
        cp == 0x2329 || cp == 0x232A ||     /* angle brackets 〈 〉 */
        (cp >= 0x2E80 && cp <= 0x303E) ||   /* CJK radicals .. Kangxi */
        (cp >= 0x3041 && cp <= 0x33FF) ||   /* Kana .. CJK symbols */
        (cp >= 0x3400 && cp <= 0x4DBF) ||   /* CJK Ext-A */
        (cp >= 0x4E00 && cp <= 0x9FFF) ||   /* CJK Unified */
        (cp >= 0xA000 && cp <= 0xA4CF) ||   /* Yi */
        (cp >= 0xAC00 && cp <= 0xD7A3) ||   /* Hangul syllables */
        (cp >= 0xF900 && cp <= 0xFAFF) ||   /* CJK compatibility */
        (cp >= 0xFE10 && cp <= 0xFE19) ||   /* vertical forms */
        (cp >= 0xFE30 && cp <= 0xFE4F) ||   /* CJK compat forms */
        (cp >= 0xFF00 && cp <= 0xFF60) ||   /* fullwidth forms */
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||   /* fullwidth signs */
        (cp >= 0x1F000 && cp <= 0x1F0FF) || /* mahjong/dominoes/cards */
        (cp >= 0x1F300 && cp <= 0x1FAFF) || /* emoji & pictographs */
        (cp >= 0x20000 && cp <= 0x3FFFD))   /* CJK Ext-B and beyond */
        return 2;
    /* BMP symbols with default *emoji* presentation are also width 2 (these are
     * the ones Claude Code emits standalone: ✅ ❌ ⭐ ✨ ❓ ❗ …). */
    if (cp == 0x231A || cp == 0x231B || (cp >= 0x23E9 && cp <= 0x23EC) ||
        cp == 0x23F0 || cp == 0x23F3 || cp == 0x25FD || cp == 0x25FE ||
        cp == 0x2614 || cp == 0x2615 || (cp >= 0x2648 && cp <= 0x2653) ||
        cp == 0x267F || cp == 0x2693 || cp == 0x26A1 ||
        cp == 0x26AA || cp == 0x26AB || cp == 0x26BD || cp == 0x26BE ||
        cp == 0x26C4 || cp == 0x26C5 || cp == 0x26CE || cp == 0x26D4 ||
        cp == 0x26EA || cp == 0x26F2 || cp == 0x26F3 || cp == 0x26F5 ||
        cp == 0x26FA || cp == 0x26FD || cp == 0x2705 ||
        cp == 0x270A || cp == 0x270B || cp == 0x2728 ||
        cp == 0x274C || cp == 0x274E || (cp >= 0x2753 && cp <= 0x2755) ||
        cp == 0x2757 || (cp >= 0x2795 && cp <= 0x2797) ||
        cp == 0x27B0 || cp == 0x27BF || cp == 0x2B1B || cp == 0x2B1C ||
        cp == 0x2B50 || cp == 0x2B55)
        return 2;
    return 1;
}

/* Map Unicode codepoints to ASCII fallbacks — no font encoding dependency */
static uint8_t unicode_to_ascii(uint32_t cp) {
    /* Box drawing — all corners/junctions → + */
    if (cp >= 0x250C && cp <= 0x254B) return '+';
    /* Double-line box drawing — corners/junctions → + */
    if (cp >= 0x2552 && cp <= 0x256C) return '+';
    /* Rounded box corners ╭ ╮ ╯ ╰ (Claude Code's UI boxes) → + */
    if (cp >= 0x256D && cp <= 0x2570) return '+';
    /* Diagonals ╱ ╲ ╳ */
    if (cp == 0x2571) return '/';
    if (cp == 0x2572) return '\\';
    if (cp == 0x2573) return 'X';
    /* Partial box lines ╴..╿ — horizontal (even) → -, vertical (odd) → | */
    if (cp >= 0x2574 && cp <= 0x257F) return (cp & 1u) ? '|' : '-';
    /* Braille patterns — used by progress spinners */
    if (cp >= 0x2800 && cp <= 0x28FF) return '*';
    /* Latin-1 accented letters → base ASCII (font has no glyphs for these) */
    if (cp == 0x00A0) return ' ';                       /* nbsp */
    if (cp >= 0x00C0 && cp <= 0x00C6) return 'A';
    if (cp == 0x00C7) return 'C';
    if (cp >= 0x00C8 && cp <= 0x00CB) return 'E';
    if (cp >= 0x00CC && cp <= 0x00CF) return 'I';
    if (cp == 0x00D0) return 'D';
    if (cp == 0x00D1) return 'N';
    if (cp >= 0x00D2 && cp <= 0x00D6) return 'O';
    if (cp == 0x00D7) return 'x';                       /* × */
    if (cp == 0x00D8) return 'O';
    if (cp >= 0x00D9 && cp <= 0x00DC) return 'U';
    if (cp == 0x00DD) return 'Y';
    if (cp == 0x00DF) return 's';                       /* ß */
    if (cp >= 0x00E0 && cp <= 0x00E6) return 'a';
    if (cp == 0x00E7) return 'c';
    if (cp >= 0x00E8 && cp <= 0x00EB) return 'e';
    if (cp >= 0x00EC && cp <= 0x00EF) return 'i';
    if (cp == 0x00F0) return 'd';
    if (cp == 0x00F1) return 'n';
    if (cp >= 0x00F2 && cp <= 0x00F6) return 'o';
    if (cp == 0x00F7) return '/';                       /* ÷ */
    if (cp == 0x00F8) return 'o';
    if (cp >= 0x00F9 && cp <= 0x00FC) return 'u';
    if (cp == 0x00FD || cp == 0x00FF) return 'y';
    switch (cp) {
    case 0x2500: case 0x2501:
    case 0x2504: case 0x2505: case 0x2508: case 0x2509:
    case 0x254C: case 0x254D:
    case 0x2550: return '-';   /* ─ ━ variants → - */
    case 0x2502: case 0x2503:
    case 0x2506: case 0x2507: case 0x250A: case 0x250B:
    case 0x254E: case 0x254F:
    case 0x2551: return '|';   /* │ ┃ variants → | */
    /* Block elements → # */
    case 0x2580: case 0x2584: case 0x2588:
    case 0x258C: case 0x2590:
    case 0x2591: case 0x2592: case 0x2593: case 0x2594: case 0x2595:
    case 0x25A0: case 0x25A1: return '#';
    /* Arrows */
    case 0x2190: case 0x21D0: case 0x27F5: return '<';
    case 0x2192: case 0x21D2: case 0x27F6: return '>';
    case 0x2191: case 0x21D1: return '^';
    case 0x2193: case 0x21D3: return 'v';
    case 0x21B5: case 0x23CE: return '<';   /* ↵ enter */
    /* Symbols Claude Code uses */
    case 0x25C6: case 0x25C7:
    case 0x25C8: case 0x25C9: return '*';   /* ◆ ◇ → * */
    case 0x25CF: return 'o';                /* ● → o */
    case 0x25CB: case 0x25CC: return 'o';   /* ○ → o */
    case 0x25B2: case 0x25B3: return '^';   /* ▲ → ^ */
    case 0x25BC: case 0x25BD: return 'v';   /* ▼ → v */
    case 0x25B6: return '>';                /* ▶ → > */
    case 0x25C0: return '<';                /* ◀ → < */
    case 0x2022: case 0x2023:
    case 0x00B7: case 0x2027:
    case 0x2219: return '.';   /* • · → . */
    case 0x2713: case 0x2714:
    case 0x2705: return '+';   /* ✓ ✔ → + */
    case 0x2717: case 0x2718:
    case 0x274C: return 'x';   /* ✗ ✘ → x */
    case 0x2026: return '.';   /* … → . */
    case 0x2014: return '-';   /* — → - */
    case 0x2013: return '-';   /* – → - */
    case 0x2018: case 0x2019:
    case 0x201C: case 0x201D: return '"';   /* curly quotes */
    default:     return ' ';
    }
}

static void cell_clear_region(int r0, int c0, int r1, int c1) {
    for (int r = r0; r <= r1 && r < g_rows; r++)
        for (int c = c0; c < c1 && c < g_cols; c++)
            g_cells[r][c] = (Cell){ ' ', g_fg, g_bg };
}

static void cell_clear_all(void) { cell_clear_region(0, 0, g_rows - 1, g_cols); }

static void scroll_up(void) {
    /* Push top row into scrollback ring */
    memcpy(g_scrollback[g_sb_write], g_cells[0], sizeof(Cell) * MAX_COLS);
    g_sb_write = (g_sb_write + 1) % SCROLLBACK;
    if (g_sb_count < SCROLLBACK) g_sb_count++;
    /* Shift screen up */
    memmove(&g_cells[0], &g_cells[1], sizeof(Cell) * MAX_COLS * (g_rows - 1));
    for (int c = 0; c < g_cols; c++)
        g_cells[g_rows - 1][c] = (Cell){ ' ', g_fg, g_bg };
}

static void newline(void) {
    g_cx = 0; g_cy++;
    if (g_cy >= g_rows) { g_cy = g_rows - 1; scroll_up(); }
}

static uint32_t ansi_color(int idx, bool bright) {
    static const uint32_t pal[8] = {
        0xFF0e1418u, 0xFF993333u, 0xFF33993eu, 0xFF999933u,
        0xFF336699u, 0xFF993399u, 0xFF339999u, 0xFFd8e8f8u,
    };
    static const uint32_t bright_pal[8] = {
        0xFF506070u, 0xFFcc4444u, 0xFF44cc44u, 0xFFcccc44u,
        0xFF4488ccu, 0xFFcc44ccu, 0xFF44ccccu, 0xFFf8f8f8u,
    };
    if (idx < 0 || idx > 7) return 0xFFd8e8f8u;
    return bright ? bright_pal[idx] : pal[idx];
}

static void handle_esc_seq(void) {
    if (!g_esc_bracket || g_esc_len == 0) {
        g_esc = g_esc_bracket = false; g_esc_len = 0; return;
    }
    char cmd = g_esc_buf[g_esc_len - 1];
    g_esc_buf[g_esc_len - 1] = '\0';

    /* DEC private sequences: ESC[?Nh / ESC[?Nl */
    if (g_esc_buf[0] == '?') {
        int n = atoi(g_esc_buf + 1);
        if (cmd == 'h') {
            if (n == 25)   { g_cursor_hide = false; g_cursor_vis = true; }
            if (n == 1049) { cell_clear_all(); g_cx = 0; g_cy = 0; }
        } else if (cmd == 'l') {
            if (n == 25)   { g_cursor_hide = true; g_cursor_vis = false; }
            if (n == 1049) { cell_clear_all(); g_cx = 0; g_cy = 0; }
        }
        g_esc = g_esc_bracket = false; g_esc_len = 0;
        return;
    }

    /* Cursor save/restore */
    if (cmd == 's') { g_saved_cx = g_cx; g_saved_cy = g_cy; g_esc = g_esc_bracket = false; g_esc_len = 0; return; }
    if (cmd == 'u') { g_cx = g_saved_cx; g_cy = g_saved_cy; g_esc = g_esc_bracket = false; g_esc_len = 0; return; }

    if (cmd == 'm') {
        char *p = g_esc_buf;
        while (*p) {
            int n = atoi(p);
            while (*p && *p != ';') p++;   /* advance past this param */
            if (*p == ';') p++;
            if (n == 0)                  { g_fg = ansi_color(7, false); g_bg = ansi_color(0, false); }
            else if (n == 1)             { /* bold */ }
            else if (n >= 30 && n <= 37)   g_fg = ansi_color(n - 30, false);
            else if (n >= 90 && n <= 97)   g_fg = ansi_color(n - 90, true);
            else if (n >= 40 && n <= 47)   g_bg = ansi_color(n - 40, false);
            else if (n == 38 || n == 48) {
                /* Extended color — skip sub-params so they aren't misread as color indices */
                int mode = atoi(p);
                while (*p && *p != ';') p++; if (*p == ';') p++;  /* skip mode (5 or 2) */
                if (mode == 5) {
                    while (*p && *p != ';') p++; if (*p == ';') p++;  /* skip N */
                } else if (mode == 2) {
                    while (*p && *p != ';') p++; if (*p == ';') p++;  /* skip R */
                    while (*p && *p != ';') p++; if (*p == ';') p++;  /* skip G */
                    while (*p && *p != ';') p++; if (*p == ';') p++;  /* skip B */
                }
            }
        }
    } else if (cmd == 'J') {
        int n = atoi(g_esc_buf);
        if (n == 2) { cell_clear_all(); g_cx = 0; g_cy = 0; }
        else if (n == 0) cell_clear_region(g_cy, g_cx, g_rows - 1, g_cols);
    } else if (cmd == 'K') {
        int n = atoi(g_esc_buf);
        if (n == 0) for (int c = g_cx; c < g_cols; c++) g_cells[g_cy][c] = (Cell){ ' ', g_fg, g_bg };
        else if (n == 1) for (int c = 0; c <= g_cx; c++) g_cells[g_cy][c] = (Cell){ ' ', g_fg, g_bg };
        else if (n == 2) for (int c = 0; c < g_cols; c++) g_cells[g_cy][c] = (Cell){ ' ', g_fg, g_bg };
    } else if (cmd == 'H' || cmd == 'f') {
        int row = 0, col = 0;
        char *sc = strchr(g_esc_buf, ';');
        row = atoi(g_esc_buf);
        if (sc) col = atoi(sc + 1);
        g_cx = (col > 1 ? col - 1 : 0); if (g_cx >= g_cols) g_cx = g_cols - 1;
        g_cy = (row > 1 ? row - 1 : 0); if (g_cy >= g_rows) g_cy = g_rows - 1;
    } else if (cmd == 'A') { int n = atoi(g_esc_buf); g_cy -= n ? n : 1; if (g_cy < 0) g_cy = 0; }
      else if (cmd == 'B') { int n = atoi(g_esc_buf); g_cy += n ? n : 1; if (g_cy >= g_rows) g_cy = g_rows-1; }
      else if (cmd == 'C') { int n = atoi(g_esc_buf); g_cx += n ? n : 1; if (g_cx >= g_cols) g_cx = g_cols-1; }
      else if (cmd == 'D') { int n = atoi(g_esc_buf); g_cx -= n ? n : 1; if (g_cx < 0) g_cx = 0; }
    else if (cmd == 'P') {
        int n = atoi(g_esc_buf); if (!n) n = 1;
        int rem = g_cols - g_cx - n;
        if (rem > 0) memmove(&g_cells[g_cy][g_cx], &g_cells[g_cy][g_cx + n], sizeof(Cell) * rem);
        for (int c = g_cols - n; c < g_cols; c++) g_cells[g_cy][c] = (Cell){ ' ', g_fg, g_bg };
    }
    g_esc = g_esc_bracket = false; g_esc_len = 0;
}

static void term_putc(uint8_t c) {
    /* OSC: consume until BEL or ESC */
    if (g_in_osc) {
        if (c == 0x07) { g_in_osc = false; }
        else if (c == 0x1B) { g_in_osc = false; g_esc = true; g_esc_bracket = false; g_esc_len = 0; }
        return;
    }
    if (g_esc) {
        if (!g_esc_bracket && c == '[') { g_esc_bracket = true; return; }
        if (!g_esc_bracket && c == ']') { g_in_osc = true; g_esc = false; return; }
        if (!g_esc_bracket && c == 'M') { /* reverse index */
            g_esc = false;
            if (g_cy > 0) { g_cy--; } else { memmove(&g_cells[1], &g_cells[0], sizeof(Cell)*MAX_COLS*(g_rows-1)); cell_clear_region(0,0,0,g_cols); }
            return;
        }
        if (g_esc_bracket) {
            if ((c >= 0x40 && c <= 0x7E) || g_esc_len >= (int)sizeof(g_esc_buf) - 1) {
                g_esc_buf[g_esc_len++] = c;
                g_esc_buf[g_esc_len]   = '\0';
                handle_esc_seq();
            } else {
                g_esc_buf[g_esc_len++] = c;
            }
        } else {
            g_esc = false;
        }
        return;
    }
    if (c == 0x1B) { g_esc = true; g_esc_bracket = false; g_esc_len = 0; return; }
    if (c == '\r')  { g_cx = 0; return; }
    if (c == '\n')  { newline(); return; }
    if (c == '\t')  { g_cx = (g_cx + 8) & ~7; if (g_cx >= g_cols) g_cx = g_cols - 1; return; }
    if (c == 0x08 || c == 0x7F) {
        if (g_cx > 0) { g_cx--; g_cells[g_cy][g_cx] = (Cell){ ' ', g_fg, g_bg }; } return;
    }
    if (c < 0x20) return;

    /* UTF-8 multi-byte handling */
    if (c >= 0x80) {
        if (c >= 0xC0) {
            if      (c >= 0xF0) { g_utf8_remain = 3; g_utf8_cp = c & 0x07u; }
            else if (c >= 0xE0) { g_utf8_remain = 2; g_utf8_cp = c & 0x0Fu; }
            else                 { g_utf8_remain = 1; g_utf8_cp = c & 0x1Fu; }
        } else if (g_utf8_remain > 0) {
            g_utf8_cp = (g_utf8_cp << 6) | (c & 0x3Fu);
            if (--g_utf8_remain == 0) {
                int w = cp_width(g_utf8_cp);
                if (w == 0) {
                    /* combining mark / joiner / variation selector: occupies no
                     * column and has no standalone glyph — the base char already
                     * advanced the cursor, so we just drop it. */
                } else {
                    uint8_t ch;
                    /* Only ASCII maps 1:1 onto a PSF font slot. For >= 0x80 the
                     * font's slot index is NOT the Unicode value (this build
                     * ignores the font's unicode table), so go through the ASCII
                     * fallback instead of indexing a wrong/blank glyph. */
                    if (g_utf8_cp < 0x80u)
                        ch = (uint8_t)g_utf8_cp;
                    else
                        ch = unicode_to_ascii(g_utf8_cp);
                    /* a double-width cell that would straddle the right edge wraps first */
                    if (w == 2 && g_cx + 1 >= g_cols) {
                        g_cx = 0;
                        if (++g_cy >= g_rows) { g_cy = g_rows-1; scroll_up(); }
                    }
                    g_cells[g_cy][g_cx] = (Cell){ ch, g_fg, g_bg };
                    if (++g_cx >= g_cols) { g_cx = 0; if (++g_cy >= g_rows) { g_cy = g_rows-1; scroll_up(); } }
                    if (w == 2) {   /* blank second column keeps following text aligned */
                        g_cells[g_cy][g_cx] = (Cell){ ' ', g_fg, g_bg };
                        if (++g_cx >= g_cols) { g_cx = 0; if (++g_cy >= g_rows) { g_cy = g_rows-1; scroll_up(); } }
                    }
                }
            }
        } else { g_utf8_remain = 0; }
        return;
    }

    g_cells[g_cy][g_cx] = (Cell){ c, g_fg, g_bg };
    g_cx++;
    if (g_cx >= g_cols) { g_cx = 0; g_cy++; if (g_cy >= g_rows) { g_cy = g_rows - 1; scroll_up(); } }
}

/* ── Rendering ───────────────────────────────────────────────────────────── */
static uint32_t *g_fb = NULL;

static void fb_set(int x, int y, uint32_t col) {
    if (x >= 0 && x < g_win_w && y >= 0 && y < g_win_h) g_fb[y * g_win_w + x] = col;
}

static void fb_fill(int x, int y, int w, int h, uint32_t col) {
    for (int row = y; row < y + h; row++)
        for (int c = x; c < x + w; c++)
            fb_set(c, row, col);
}

static void fb_glyph(int px, int py, uint8_t ch, uint32_t fg, uint32_t bg) {
    if (!g_glyph || ch >= (unsigned)g_n_glyph) return;
    const uint8_t *bits = g_glyph + ch * g_glyph_h;
    for (int row = 0; row < g_glyph_h; row++) {
        uint8_t b = bits[row];
        for (int col = 0; col < g_glyph_w; col++) {
            uint32_t c = (b & (0x80u >> col)) ? fg : bg;
            fb_set(px + col, py + row, c);
        }
    }
}

/* Get scrollback line n (0=oldest) */
static Cell *sb_row_col(int n, int col) {
    int ring = (g_sb_write - g_sb_count + n + SCROLLBACK * 1024) % SCROLLBACK;
    return &g_scrollback[ring][col];
}

static void render(void) {
    fb_fill(0, 0, g_win_w, g_win_h, g_bg);
    fb_fill(0, 0, g_win_w, TITLE_H, 0xFF0e1620u);

    int cell_w = g_glyph_w + 1;
    int cell_h = g_glyph_h + 1;
    int ox = PAD, oy = TITLE_H + PAD;

    for (int row = 0; row < g_rows; row++) {
        for (int col = 0; col < g_cols; col++) {
            Cell *ce;
            if (g_scroll_offset > 0 && row < g_scroll_offset) {
                int sb_n = g_sb_count - g_scroll_offset + row;
                if (sb_n < 0) {
                    static Cell blank = { ' ', 0xFFd8e8f8u, 0xFF0e1418u };
                    ce = &blank;
                } else {
                    ce = sb_row_col(sb_n, col < MAX_COLS ? col : MAX_COLS - 1);
                }
            } else {
                ce = &g_cells[row - g_scroll_offset][col];
            }
            int px = ox + col * cell_w;
            int py = oy + row * cell_h;
            if (px + cell_w > g_win_w) break;
            fb_fill(px, py, cell_w, cell_h, ce->bg);
            fb_glyph(px, py, ce->ch, ce->fg, ce->bg);
        }
    }

    /* Cursor — hidden while scrolled back */
    if (g_cursor_vis && g_scroll_offset == 0) {
        int px = ox + g_cx * cell_w;
        int py = oy + g_cy * cell_h;
        fb_fill(px, py + cell_h - 2, cell_w, 2, g_fg);
    }

    /* Scrollbar on right edge */
    {
        int sb_x = g_win_w - 14;
        int sb_y = TITLE_H + 4;
        int sb_h = g_win_h - TITLE_H - 8;
        fb_fill(sb_x, sb_y, 14, sb_h, 0xFF1e3048u);
        int sb_total = (g_sb_count > 0) ? g_sb_count : 1;
        int total_lines = g_rows + sb_total;
        int thumb_h = sb_h * g_rows / total_lines;
        if (thumb_h < 8) thumb_h = 8;
        int thumb_y = sb_y + (sb_h - thumb_h) * (sb_total - g_scroll_offset) / sb_total;
        if (thumb_y < sb_y) thumb_y = sb_y;
        if (thumb_y + thumb_h > sb_y + sb_h) thumb_y = sb_y + sb_h - thumb_h;
        fb_fill(sb_x + 2, thumb_y, 10, thumb_h, 0xFFd0d8e0u);
    }
}

/* ── IPC helpers ─────────────────────────────────────────────────────────── */
static void write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n > 0) { p += n; len -= (size_t)n; }
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        } else if (n < 0 && errno == EINTR) {
            /* retry */
        } else { break; }
    }
}

static void ipc_send(int fd, uint32_t type, const void *data, uint32_t len) {
    uint8_t hdr[8];
    memcpy(hdr,     &type, 4);
    memcpy(hdr + 4, &len,  4);
    write_all(fd, hdr, 8);
    if (len > 0 && data) write_all(fd, data, len);
}

static void send_frame(int fd) {
    uint32_t frm[4] = { 0, 0, (uint32_t)g_win_w, (uint32_t)g_win_h };
    uint32_t total  = 16 + (uint32_t)g_win_w * (uint32_t)g_win_h * 4;
    uint8_t *msg    = malloc(total);
    if (!msg) return;
    memcpy(msg,      frm,  16);
    memcpy(msg + 16, g_fb, (size_t)g_win_w * g_win_h * 4);
    ipc_send(fd, IPC_APP_FRAME, msg, total);
    free(msg);
}

/* ── PTY setup ───────────────────────────────────────────────────────────── */
static int pty_spawn(void) {
    g_pty_master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (g_pty_master < 0) return -1;
    if (grantpt(g_pty_master) < 0 || unlockpt(g_pty_master) < 0) {
        close(g_pty_master); return -1;
    }
    char *slave_name = ptsname(g_pty_master);
    if (!slave_name) { close(g_pty_master); return -1; }

    struct winsize ws = { .ws_row = (uint16_t)g_rows, .ws_col = (uint16_t)g_cols };
    ioctl(g_pty_master, TIOCSWINSZ, &ws);

    g_child_pid = fork();
    if (g_child_pid < 0) { close(g_pty_master); return -1; }
    if (g_child_pid == 0) {
        setsid();
        int slave = open(slave_name, O_RDWR);
        if (slave < 0) _exit(1);
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        if (slave > 2) close(slave);
        /* Match the built-in terminal (platform/linux/pty.c) so both shells look identical:
         * same TERM, environment, "\w # " prompt, and ush-then-sh login shell. */
        setenv("TERM",     "xterm-256color", 1);
        setenv("LANG",     "en_US.UTF-8",   1);
        setenv("LC_ALL",   "en_US.UTF-8",   1);
        setenv("HOME",     "/root",          1);
        if (!getenv("PATH") || getenv("PATH")[0] == '\0')
            setenv("PATH", "/usr/local/bin:/bin:/sbin:/usr/bin:/usr/sbin", 1);
        setenv("USER",  "fifi",  1);
        setenv("SHELL", "/bin/sh", 1);
        /* Familiar Linux-style prompt, identical to the built-in terminal. */
        setenv("PS1",   "fifi@FiFiOS:\\w$ ", 1);
        execl("/bin/ush", "-ush", NULL);
        execl("/bin/sh",  "-sh",  NULL);
        execl("/bin/busybox", "sh", NULL);
        _exit(1);
    }
    fcntl(g_pty_master, F_SETFL, O_NONBLOCK);
    return 0;
}

/* ── Key translation ─────────────────────────────────────────────────────── */
static void key_to_pty(uint8_t k) {
    if (k >= 0x20 && k < 0x7F) { write(g_pty_master, &k, 1); return; }
    switch (k) {
    case 0x0D: case '\n': { uint8_t nl = '\r'; write(g_pty_master, &nl, 1); break; }
    case 0x08: case 0x7F: { uint8_t bs = 0x7F; write(g_pty_master, &bs, 1); break; }
    case 0x03: write(g_pty_master, "\x03", 1); break;
    case 0x04: write(g_pty_master, "\x04", 1); break;
    case 0x0C: write(g_pty_master, "\x0C", 1); break;
    case 0x1A: write(g_pty_master, "\x1A", 1); break;
    case 0x1B: write(g_pty_master, "\x1B", 1); break;
    case 0x09: write(g_pty_master, "\x09", 1); break;
    case 0x80: write(g_pty_master, "\x1B[D", 3); break;
    case 0x81: write(g_pty_master, "\x1B[C", 3); break;
    case 0x82: write(g_pty_master, "\x1B[A", 3); break;
    case 0x83: write(g_pty_master, "\x1B[B", 3); break;
    case 0x84: write(g_pty_master, "\x1B[3~", 4); break;
    case 0x85: write(g_pty_master, "\x1B[H", 3); break;
    case 0x86: write(g_pty_master, "\x1B[F", 3); break;
    default:
        if (k < 0x20) write(g_pty_master, &k, 1);
        break;
    }
}

/* Recalculate g_rows/g_cols from current window size, reallocate fb */
static void resize_to(int new_w, int new_h) {
    int cell_w = g_glyph_w + 1;
    int cell_h = g_glyph_h + 1;
    int new_cols = (new_w - 2 * PAD) / cell_w;
    int new_rows = (new_h - TITLE_H - 2 * PAD) / cell_h;
    if (new_cols < 10) new_cols = 10;
    if (new_rows <  3) new_rows = 3;
    if (new_cols > MAX_COLS) new_cols = MAX_COLS;
    if (new_rows > MAX_ROWS) new_rows = MAX_ROWS;

    bool size_changed = (new_cols != g_cols || new_rows != g_rows ||
                         new_w != g_win_w || new_h != g_win_h);
    if (!size_changed) return;

    /* Clamp cursor to new grid */
    if (g_cx >= new_cols) g_cx = new_cols - 1;
    if (g_cy >= new_rows) g_cy = new_rows - 1;
    int old_cols = g_cols, old_rows = g_rows;
    g_cols = new_cols; g_rows = new_rows;
    g_win_w = new_w;  g_win_h = new_h;

    /* Initialise cells newly exposed by a grow to the current bg. They were never written
     * (static zero-init → bg=0), so render() would otherwise paint them as pure-black
     * boxes. New rows are cleared fully; previously-existing rows get their new trailing
     * columns cleared. (Shrink: c0 >= g_cols, loops are no-ops.) */
    for (int r = 0; r < g_rows; r++) {
        int c0 = (r < old_rows) ? old_cols : 0;
        for (int c = c0; c < g_cols; c++)
            g_cells[r][c] = (Cell){ ' ', g_fg, g_bg };
    }

    /* Reallocate framebuffer */
    free(g_fb);
    g_fb = malloc((size_t)g_win_w * g_win_h * 4);
    if (!g_fb) { g_fb = NULL; return; }

    /* Deliberately do NOT push the new size to the pty on resize. The built-in terminal
     * does the same — it sets the pty winsize once at spawn (pty_set_initial_winsize) and
     * never again — because busybox sh reprints its prompt on every SIGWINCH, so each
     * resize would stack another prompt line. The shell keeps its spawn-time grid; only the
     * visible grid reflows. (A full-screen TUI launched after a resize would see the old
     * size, same limitation as the built-in terminal.) */
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void) {
    font_load("/fifi-data/fonts/ter16b.psf");
    /* After font load, recalculate default grid */
    {
        int cw = g_glyph_w + 1, ch = g_glyph_h + 1;
        g_cols = (DEF_WIN_W - 2 * PAD) / cw;
        g_rows = (DEF_WIN_H - TITLE_H - 2 * PAD) / ch;
        if (g_cols > MAX_COLS) g_cols = MAX_COLS;
        if (g_rows > MAX_ROWS) g_rows = MAX_ROWS;
    }
    cell_clear_all();

    g_fb = malloc((size_t)DEF_WIN_W * DEF_WIN_H * 4);
    if (!g_fb) return 1;

    if (pty_spawn() < 0) {
        const char *msg = "PTY spawn failed";
        for (int i = 0; msg[i]; i++) term_putc((uint8_t)msg[i]);
    }

    /* Connect to compositor */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return 1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIFI_SOCK, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) return 1;

    uint8_t conn[68] = {0};
    uint16_t w = (uint16_t)g_win_w, h = (uint16_t)g_win_h;
    memcpy(conn,     &w, 2);
    memcpy(conn + 2, &h, 2);
    snprintf((char *)(conn + 4), 64, "Terminal");
    ipc_send(sock, IPC_APP_CONNECT, conn, sizeof(conn));

    {
        uint8_t hdr[8] = {0};
        if (read(sock, hdr, 8) < 8) return 1;
        uint32_t tp, pl;
        memcpy(&tp, hdr, 4); memcpy(&pl, hdr + 4, 4);
        if (tp == IPC_WIN_CREATED && pl >= 20) {
            uint8_t resp[20]; read(sock, resp, pl > 20 ? 20 : pl);
        }
    }

    signal(SIGPIPE, SIG_IGN);
    render();
    send_frame(sock);

    uint8_t  in_hdr[8];
    int      in_got = 0;
    uint8_t *in_pld = NULL;
    uint32_t in_plen = 0, in_pgot = 0;
    uint32_t in_type = 0;
    bool     running = true;
    uint32_t tick = 0;

    while (running) {
        struct pollfd pfds[2];
        pfds[0].fd = sock;     pfds[0].events = POLLIN;
        pfds[1].fd = (g_pty_master >= 0) ? g_pty_master : -1; pfds[1].events = POLLIN;

        int pn = poll(pfds, 2, 16);
        if (pn < 0 && errno == EINTR) continue;

        /* ── Compositor messages ── */
        if (pfds[0].revents & POLLIN) {
            uint8_t tbuf[512];
            ssize_t nr = read(sock, tbuf, sizeof(tbuf));
            if (nr <= 0) { running = false; break; }
            ssize_t pos = 0;
            while (pos < nr) {
                if (in_got < 8) {
                    in_hdr[in_got++] = tbuf[pos++];
                    if (in_got == 8) {
                        memcpy(&in_type, in_hdr,     4);
                        memcpy(&in_plen, in_hdr + 4, 4);
                        if (in_plen > 131072) { in_got = 0; break; }
                        if (in_plen > 0) { in_pld = malloc(in_plen); in_pgot = 0; }
                    }
                } else if (in_plen > 0 && in_pgot < in_plen) {
                    uint32_t need = in_plen - in_pgot;
                    uint32_t have = (uint32_t)(nr - pos);
                    uint32_t take = need < have ? need : have;
                    if (in_pld) memcpy(in_pld + in_pgot, tbuf + pos, take);
                    in_pgot += take; pos += take;
                    if (in_pgot >= in_plen) {
                        if (in_type == IPC_INPUT_KEY && in_plen >= 1) {
                            uint8_t key = in_pld ? in_pld[0] : 0;
                            if (key == 0x87u) {  /* PgUp: scroll back */
                                int step = g_rows / 2;
                                g_scroll_offset += step;
                                if (g_scroll_offset > g_sb_count) g_scroll_offset = g_sb_count;
                                g_dirty = true;
                            } else if (key == 0x88u) {  /* PgDn: scroll forward */
                                int step = g_rows / 2;
                                g_scroll_offset -= step;
                                if (g_scroll_offset < 0) g_scroll_offset = 0;
                                g_dirty = true;
                            } else {
                                if (g_scroll_offset > 0) { g_scroll_offset = 0; g_dirty = true; }
                                if (key == 0x16u) {
                                    ipc_send(sock, IPC_CLIP_GET, NULL, 0);
                                } else if (g_pty_master >= 0) {
                                    key_to_pty(key);
                                }
                            }
                        } else if (in_type == IPC_INPUT_MOUSE && in_plen >= 10) {
                            int8_t wheel = (int8_t)(in_pld ? in_pld[9] : 0);
                            if (wheel != 0) {
                                g_scroll_offset += wheel * 3;
                                if (g_scroll_offset < 0) g_scroll_offset = 0;
                                if (g_scroll_offset > g_sb_count) g_scroll_offset = g_sb_count;
                                g_dirty = true;
                            }
                        } else if (in_type == IPC_CLIP_DATA && in_plen > 0 && g_pty_master >= 0) {
                            if (in_pld) write(g_pty_master, in_pld, in_plen);
                        } else if (in_type == IPC_WIN_RESIZE && in_plen >= 4) {
                            uint16_t nw, nh;
                            memcpy(&nw, in_pld,     2);
                            memcpy(&nh, in_pld + 2, 2);
                            resize_to((int)nw, (int)nh);
                            g_dirty = true;
                        }
                        free(in_pld); in_pld = NULL;
                        in_got = 0; in_plen = 0; in_pgot = 0;
                    }
                } else {
                    if (in_type == IPC_INVALIDATE) g_dirty = true;
                    in_got = 0; in_plen = 0; in_pgot = 0;
                }
            }
        }
        if (pfds[0].revents & (POLLHUP | POLLERR)) { running = false; break; }

        /* ── PTY output ── */
        if (g_pty_master >= 0 && (pfds[1].revents & POLLIN)) {
            uint8_t buf[512];
            ssize_t nr2;
            while ((nr2 = read(g_pty_master, buf, sizeof(buf))) > 0) {
                for (ssize_t i = 0; i < nr2; i++) term_putc(buf[i]);
                g_dirty = true;
            }
        }
        if (g_child_pid > 0) {
            int wstat = 0;
            if (waitpid(g_child_pid, &wstat, WNOHANG) > 0) {
                g_child_pid = -1;
                const char *ex = "\r\n[Process exited]\r\n";
                for (const char *p = ex; *p; p++) term_putc((uint8_t)*p);
                g_dirty = true;
            }
        }

        /* ── Cursor blink ── */
        tick++;
        if ((tick % 30) == 0 && !g_cursor_hide) { g_cursor_vis = !g_cursor_vis; g_dirty = true; }

        if (g_dirty && g_fb) {
            render();
            send_frame(sock);
            g_dirty = false;
        }
    }

    if (g_child_pid > 0) kill(g_child_pid, SIGTERM);
    if (g_pty_master >= 0) close(g_pty_master);
    ipc_send(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock);
    free(g_fb);
    free(g_glyph);
    return 0;
}
