/* FiFi Editor — standalone IPC text editor.
 * Receives the file path as argv[1] OR via IPC_OPEN_FILE from the compositor.
 * Supports: arrow keys, home/end, pgup/pgdn, insert, delete, backspace,
 *           Ctrl+S (save), Ctrl+Q (quit), Ctrl+G (go to line), Ctrl+F (find),
 *           Ctrl+Z (undo, single level), line numbers, status bar.
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
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>

#include "../fifi_u8.h"

/* ── IPC protocol ────────────────────────────────────────────────────────── */
#include "../../shared/app_ipc.h"
#include "../../shared/app_ui.h"

/* ── Window layout ───────────────────────────────────────────────────────── */
#define WIN_W   720
#define WIN_H   480
#define TITLE_H  24   /* compositor title bar */
#define HDR_H    20   /* file name bar */
#define FOOT_H   20   /* status bar */
#define LNUM_W   48   /* line number gutter width */
#define PAD_X     4

/* ── Colour palette (shared FiFi design language) ────────────────────────── */
#define C_BG        0x000E1620u   /* editing surface */
#define C_HDR_BG    0x001A2740u   /* header / toolbar bar */
#define C_FOOT_BG   0x001A2740u   /* status bar */
#define C_SEL       0x002F6BBFu   /* selection (accent-dim) */
#define C_LNUM_BG   0x0016202Eu   /* line-number gutter (card) */
#define C_LNUM_FG   0x006A8098u   /* gutter numerals (muted) */
#define C_CUR_LINE  0x00131D2Bu   /* current line highlight */
#define C_TEXT      0x00D8E8F8u   /* primary text */
#define C_ACCENT    0x00409CFFu   /* accent (cursor, active line #) */
#define C_MODIFIED  0x00E0A030u   /* modified indicator (amber) */
#define C_SAVED     0x0040CC80u   /* saved (green) */
#define C_BORDER    0x00243448u   /* subtle divider */
#define C_FIND_HL   0x00335F94u   /* find match highlight */
#define C_WHITE     0x00FFFFFFu   /* bright text */
#define C_GREY      0x006A8098u   /* muted / secondary */
#define C_WARN      0x00E05050u

/* ── Shared bitmap UI ────────────────────────────────────────────────────── */
static fifi_ui_font_t g_font;
#define g_glyph_h (g_font.height)

static bool font_load(const char *path) {
    return fifi_ui_font_load_psf1(&g_font, path);
}

static void fb_fill(uint32_t *fb, int x, int y, int w, int h, uint32_t col) {
    fifi_ui_fill((fifi_ui_canvas_t){fb, WIN_W, WIN_H}, x, y, w, h, col);
}

static void fb_glyph(uint32_t *fb, int px, int py, unsigned char ch, uint32_t fg, uint32_t bg) {
    fifi_ui_glyph((fifi_ui_canvas_t){fb, WIN_W, WIN_H}, &g_font,
                  px, py, ch, fg, bg);
}

/* UTF-8 aware string draw for chrome (filename header, footer, status, line
 * numbers): decode each codepoint and fold it to a byte the font can show, one
 * cell per codepoint. Used only for non-editable text, so column math is free
 * to advance per codepoint here. */
static void fb_str(uint32_t *fb, const char *s, int x, int y, uint32_t fg, uint32_t bg) {
    for (size_t i = 0; s[i] && x + 8 <= WIN_W; ) {
        uint32_t cp = fifi_u8_next(s, &i);
        int c = fifi_fold_ascii(cp);
        if (c == 0) continue;                 /* zero-width: skip */
        fb_glyph(fb, x, y, (unsigned char)c, fg, bg);
        x += 9;
    }
}


/* ── Document model ──────────────────────────────────────────────────────── */
#define MAX_LINES  8192
#define LINE_CAP   1024   /* max chars per line */

static char  *g_lines[MAX_LINES];   /* each is malloc'd, NUL-terminated */
static int    g_nlines = 1;
static int    g_cx = 0;  /* cursor col (byte offset in line) */
static int    g_cy = 0;  /* cursor row */
static int    g_scroll = 0;  /* first visible row */
static int    g_col_scroll = 0;  /* first visible col */
static bool   g_modified = false;
static char   g_filepath[512] = {0};
static bool   g_readonly = false;

static char *line_dup(const char *s);   /* full-capacity line allocator (below) */

/* ── Undo (single level) ─────────────────────────────────────────────────── */
static char  *g_undo_lines[MAX_LINES];
static int    g_undo_nlines = 0;
static int    g_undo_cx = 0, g_undo_cy = 0;
static bool   g_undo_valid = false;

static void undo_save(void) {
    for (int i = 0; i < g_undo_nlines; i++) { free(g_undo_lines[i]); g_undo_lines[i] = NULL; }
    g_undo_nlines = g_nlines;
    for (int i = 0; i < g_nlines; i++) g_undo_lines[i] = strdup(g_lines[i] ? g_lines[i] : "");
    g_undo_cx = g_cx; g_undo_cy = g_cy;
    g_undo_valid = true;
}

static void undo_restore(void) {
    if (!g_undo_valid) return;
    for (int i = 0; i < g_nlines; i++) { free(g_lines[i]); g_lines[i] = NULL; }
    g_nlines = g_undo_nlines;
    for (int i = 0; i < g_nlines; i++) g_lines[i] = line_dup(g_undo_lines[i] ? g_undo_lines[i] : "");
    g_cx = g_undo_cx; g_cy = g_undo_cy;
    g_modified = true;
}

/* ── Find state ──────────────────────────────────────────────────────────── */
#define FIND_MAX 64
static char  g_find_buf[FIND_MAX] = {0};
static int   g_find_len = 0;
static bool  g_find_mode = false;
static int   g_find_line = -1, g_find_col = -1;  /* last match position */

/* ── Status message ──────────────────────────────────────────────────────── */
static char    g_status[128] = {0};
static time_t  g_status_until = 0;
static uint32_t g_status_col = 0x0040cc80u;

static void set_status(const char *msg, uint32_t col) {
    snprintf(g_status, sizeof(g_status), "%s", msg);
    g_status_col = col;
    g_status_until = time(NULL) + 3;
}

/* Allocate a line buffer at full LINE_CAP capacity (NOT the string's exact
 * length) and copy s into it. insert_char/split/join may grow a line up to
 * LINE_CAP, so every editable line must own that much space — strdup'd
 * exact-length buffers overflowed the heap on the first inserted character. */
static char *line_dup(const char *s) {
    char *p = malloc(LINE_CAP);
    if (!p) return NULL;
    size_t n = s ? strlen(s) : 0;
    if (n >= LINE_CAP) n = LINE_CAP - 1;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ── File I/O ────────────────────────────────────────────────────────────── */
static void line_ensure(int idx) {
    if (!g_lines[idx]) g_lines[idx] = line_dup("");
}

static bool file_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    for (int i = 0; i < g_nlines; i++) { free(g_lines[i]); g_lines[i] = NULL; }
    g_nlines = 0;
    /* buf must not exceed LINE_CAP: line_dup keeps at most LINE_CAP-1 chars,
     * and fgets reads at most sizeof(buf)-1 — anything longer would be
     * silently dropped between the two. Overlong lines wrap to the next row. */
    char buf[LINE_CAP];
    while (fgets(buf, sizeof(buf), f) && g_nlines < MAX_LINES) {
        int len = (int)strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[--len] = '\0';
        if (len > 0 && buf[len-1] == '\r') buf[--len] = '\0';
        g_lines[g_nlines++] = line_dup(buf);
    }
    fclose(f);
    if (g_nlines == 0) { g_lines[0] = line_dup(""); g_nlines = 1; }
    return true;
}

static bool file_save(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    for (int i = 0; i < g_nlines; i++) {
        fprintf(f, "%s\n", g_lines[i] ? g_lines[i] : "");
    }
    fclose(f);
    return true;
}

/* ── Layout helpers ──────────────────────────────────────────────────────── */
static int visible_rows(void) { return (WIN_H - TITLE_H - HDR_H - FOOT_H) / (g_glyph_h + 1); }
static int visible_cols(void) { return (WIN_W - LNUM_W - PAD_X) / 9; }

static void clamp_scroll(void) {
    int vr = visible_rows(), vc = visible_cols();
    if (g_cy < g_scroll) g_scroll = g_cy;
    if (g_cy >= g_scroll + vr) g_scroll = g_cy - vr + 1;
    if (g_scroll < 0) g_scroll = 0;
    if (g_cx < g_col_scroll) g_col_scroll = g_cx;
    if (g_cx >= g_col_scroll + vc) g_col_scroll = g_cx - vc + 1;
    if (g_col_scroll < 0) g_col_scroll = 0;
}

/* ── Rendering ───────────────────────────────────────────────────────────── */
static void render(uint32_t *fb) {
    fb_fill(fb, 0, 0, WIN_W, WIN_H, C_BG);
    fb_fill(fb, 0, 0, WIN_W, TITLE_H, C_HDR_BG);

    /* File name header */
    fb_fill(fb, 0, TITLE_H, WIN_W, HDR_H, C_HDR_BG);
    {
        char hdr[128];
        const char *name = strrchr(g_filepath, '/');
        name = name ? name + 1 : g_filepath;
        if (!*name) name = "[new file]";
        snprintf(hdr, sizeof(hdr), " %.90s%s", name, g_modified ? "  [modified]" : "");
        fb_str(fb, hdr, 4, TITLE_H + (HDR_H - g_glyph_h) / 2,
               g_modified ? C_MODIFIED : C_WHITE, C_HDR_BG);
    }
    /* Separator */
    fb_fill(fb, 0, TITLE_H + HDR_H - 1, WIN_W, 1, C_BORDER);

    int list_top = TITLE_H + HDR_H;
    int cell_h   = g_glyph_h + 1;
    int vr = visible_rows();

    for (int row = 0; row < vr; row++) {
        int line_idx = g_scroll + row;
        int py = list_top + row * cell_h;
        bool is_cur = (line_idx == g_cy);

        /* Current line highlight */
        if (is_cur) fb_fill(fb, 0, py, WIN_W, cell_h, C_CUR_LINE);

        /* Line number gutter */
        fb_fill(fb, 0, py, LNUM_W, cell_h, C_LNUM_BG);
        if (line_idx < g_nlines) {
            char lnum[16];
            snprintf(lnum, sizeof(lnum), "%4d", (line_idx + 1) % 10000);
            fb_str(fb, lnum, 4, py + (cell_h - g_glyph_h) / 2,
                   is_cur ? C_ACCENT : C_LNUM_FG, C_LNUM_BG);
        }
        fb_fill(fb, LNUM_W - 1, py, 1, cell_h, C_BORDER);

        if (line_idx >= g_nlines) continue;
        line_ensure(line_idx);
        const char *line = g_lines[line_idx];
        int len = (int)strlen(line);
        int vc = visible_cols();
        int draw_from = g_col_scroll;
        int draw_n = len - draw_from;
        if (draw_n > vc) draw_n = vc;
        if (draw_n < 0) draw_n = 0;

        /* Precompute find-match highlight mask for the visible span */
        static bool find_mask[LINE_CAP];
        bool find_active = (g_find_len > 0 && !g_find_mode && draw_n > 0);
        if (find_active) {
            memset(find_mask, 0, (size_t)draw_n);
            const char *scan = line;
            while ((scan = strstr(scan, g_find_buf)) != NULL) {
                int mstart = (int)(scan - line) - draw_from;
                if (mstart >= draw_n) break;      /* past the visible span */
                int mend = mstart + g_find_len;
                if (mstart < 0) mstart = 0;
                if (mend > draw_n) mend = draw_n;
                for (int mi = mstart; mi < mend; mi++) find_mask[mi] = true;
                scan++;
            }
        }

        /* Fold each source byte to a display glyph WITHOUT changing the
         * byte↔cell mapping the cursor/scroll/find/click logic relies on:
         * a multi-byte codepoint shows its folded ASCII glyph in the lead
         * byte's cell and blank cells for its continuation bytes. This kills
         * garbage boxes while keeping editing perfectly byte-aligned. */
        static unsigned char disp[LINE_CAP];
        {
            size_t bi = 0;
            while (line[bi] && bi < LINE_CAP) {
                size_t start = bi;
                uint32_t cp = fifi_u8_next(line, &bi);
                if (bi > LINE_CAP) bi = LINE_CAP;
                int fold = fifi_fold_ascii(cp);
                disp[start] = (fold > 0) ? (unsigned char)fold : ' ';
                for (size_t k = start + 1; k < bi; k++) disp[k] = ' ';
            }
        }

        int tx = LNUM_W + PAD_X;
        for (int ci = 0; ci < draw_n; ci++) {
            unsigned char ch = disp[draw_from + ci];
            uint32_t bg = is_cur ? C_CUR_LINE : C_BG;
            if (find_active && find_mask[ci]) bg = C_FIND_HL;
            fb_glyph(fb, tx + ci * 9, py + (cell_h - g_glyph_h) / 2, ch, C_TEXT, bg);
        }

        /* Cursor */
        if (is_cur) {
            int cur_col = g_cx - g_col_scroll;
            if (cur_col >= 0 && cur_col <= vc) {
                int cpx = tx + cur_col * 9;
                fb_fill(fb, cpx, py + cell_h - 2, 9, 2, C_ACCENT);
            }
        }
    }

    /* Scrollbar on right edge */
    {
        int sb_x = WIN_W - 6;
        int sb_y = TITLE_H + HDR_H;
        int sb_h = WIN_H - TITLE_H - HDR_H - FOOT_H;
        int vr = visible_rows();
        if (g_nlines > vr) {
            fb_fill(fb, sb_x, sb_y, 6, sb_h, C_LNUM_BG);
            int thumb_h = sb_h * vr / g_nlines;
            if (thumb_h < 8) thumb_h = 8;
            int max_s = g_nlines - vr;
            int thumb_y = sb_y + (sb_h - thumb_h) * g_scroll / (max_s > 0 ? max_s : 1);
            if (thumb_y + thumb_h > sb_y + sb_h) thumb_y = sb_y + sb_h - thumb_h;
            fb_fill(fb, sb_x + 1, thumb_y + 1, 4, thumb_h - 2, C_GREY);
        }
    }

    /* Footer status bar */
    int foot_y = WIN_H - FOOT_H;
    fb_fill(fb, 0, foot_y, WIN_W, FOOT_H, C_FOOT_BG);
    fb_fill(fb, 0, foot_y, WIN_W, 1, C_BORDER);
    int fy = foot_y + (FOOT_H - g_glyph_h) / 2;

    if (g_find_mode) {
        char fbuf[128];
        snprintf(fbuf, sizeof(fbuf), "  Find: %.*s_  (Enter=next  Esc=cancel)",
                 g_find_len, g_find_buf);
        fb_str(fb, fbuf, 0, fy, C_WHITE, C_FOOT_BG);
    } else if (g_status[0] && time(NULL) <= g_status_until) {
        fb_str(fb, g_status, 4, fy, g_status_col, C_FOOT_BG);
    } else {
        char foot[160];
        snprintf(foot, sizeof(foot),
                 "  Ln %d/%d  Col %d  |  Ctrl+S:save  Ctrl+Z:undo  Ctrl+F:find  Ctrl+Q:quit",
                 g_cy + 1, g_nlines, g_cx + 1);
        fb_str(fb, foot, 0, fy, C_GREY, C_FOOT_BG);
    }
}

/* ── IPC helpers ─────────────────────────────────────────────────────────── */
static void ipc_send(int fd, uint32_t type, const void *data, uint32_t len) {
    (void)fifi_app_ipc_send(fd, type, data, len);
}

static void send_frame(int fd, uint32_t *fb) {
    (void)fifi_app_ipc_send_frame(fd, WIN_W, WIN_H, fb);
}

static void update_title(int fd) {
    const char *name = strrchr(g_filepath, '/');
    name = name ? name + 1 : g_filepath;
    if (!*name) name = "[new file]";
    char title[80];
    snprintf(title, sizeof(title), "Editor: %.50s%s", name, g_modified ? " *" : "");
    ipc_send(fd, IPC_APP_TITLE, title, (uint32_t)strlen(title));
}

/* ── Document editing ────────────────────────────────────────────────────── */
static int line_len(int row) {
    if (row < 0 || row >= g_nlines || !g_lines[row]) return 0;
    return (int)strlen(g_lines[row]);
}

static void insert_char(int row, int col, char ch) {
    line_ensure(row);
    int len = line_len(row);
    if (len + 1 >= LINE_CAP) return;
    char *l = g_lines[row];
    memmove(l + col + 1, l + col, (size_t)(len - col + 1));
    l[col] = ch;
    g_modified = true;
}

static void delete_char(int row, int col) {
    if (col >= line_len(row)) return;
    char *l = g_lines[row];
    int len = (int)strlen(l);
    memmove(l + col, l + col + 1, (size_t)(len - col));
    g_modified = true;
}

static void join_with_next(int row) {
    if (row + 1 >= g_nlines) return;
    line_ensure(row); line_ensure(row + 1);
    int lena = line_len(row), lenb = line_len(row + 1);
    if (lena + lenb >= LINE_CAP) return;
    char *joined = malloc(LINE_CAP);
    if (!joined) return;
    memcpy(joined, g_lines[row], (size_t)lena);
    memcpy(joined + lena, g_lines[row + 1], (size_t)(lenb + 1));
    free(g_lines[row]);
    g_lines[row] = joined;
    free(g_lines[row + 1]);
    memmove(&g_lines[row + 1], &g_lines[row + 2],
            (size_t)(g_nlines - row - 2) * sizeof(char *));
    g_nlines--;
    g_lines[g_nlines] = NULL;
    g_modified = true;
}

static void split_line(int row, int col) {
    if (g_nlines >= MAX_LINES) return;
    line_ensure(row);
    char *rest = line_dup(g_lines[row] + col);
    g_lines[row][col] = '\0';
    memmove(&g_lines[row + 2], &g_lines[row + 1],
            (size_t)(g_nlines - row - 1) * sizeof(char *));
    g_lines[row + 1] = rest;
    g_nlines++;
    g_modified = true;
}

/* ── Find ────────────────────────────────────────────────────────────────── */
static void find_next(void) {
    if (g_find_len == 0) return;
    int start_row = g_cy, start_col = g_cx + 1;
    for (int pass = 0; pass < 2; pass++) {
        for (int r = (pass == 0 ? start_row : 0); r < g_nlines; r++) {
            line_ensure(r);
            const char *line = g_lines[r];
            int from = (r == start_row && pass == 0) ? start_col : 0;
            const char *p = strstr(line + from, g_find_buf);
            if (p) {
                g_cy = r;
                g_cx = (int)(p - line);
                g_find_line = g_cy; g_find_col = g_cx;
                clamp_scroll();
                return;
            }
        }
        start_row = 0; start_col = 0;
    }
    set_status("Not found", C_WARN);
}

/* ── Key handling ────────────────────────────────────────────────────────── */
static bool handle_key(uint8_t key, int sock, uint32_t *fb) {
    /* ── Find mode ── */
    if (g_find_mode) {
        if (key == 0x1B) { /* Esc */
            g_find_mode = false;
        } else if (key == 0x0D || key == '\n') { /* Enter */
            g_find_mode = false;
            find_next();
        } else if ((key == 0x08 || key == 0x7F) && g_find_len > 0) {
            g_find_buf[--g_find_len] = '\0';
        } else if (key >= 0x20 && key < 0x7F && g_find_len < FIND_MAX - 1) {
            g_find_buf[g_find_len++] = (char)key;
            g_find_buf[g_find_len]   = '\0';
        }
        return true;
    }

    /* ── Ctrl keys ── */
    if (key == 0x13) { /* Ctrl+S: save */
        if (g_readonly) { set_status("Read-only", C_WARN); return true; }
        if (g_filepath[0]) {
            if (file_save(g_filepath)) {
                g_modified = false;
                update_title(sock);
                set_status("Saved", C_SAVED);
            } else {
                set_status("Save failed!", C_WARN);
            }
        } else {
            set_status("No filename", C_WARN);
        }
        return true;
    }
    if (key == 0x11) { /* Ctrl+Q */
        return false; /* signal quit */
    }
    if (key == 0x06) { /* Ctrl+F: find */
        g_find_mode = true;
        g_find_len  = 0;
        g_find_buf[0] = '\0';
        return true;
    }
    if (key == 0x1A) { /* Ctrl+Z: undo */
        undo_restore();
        clamp_scroll();
        update_title(sock);
        return true;
    }
    if (key == 0x07) { /* Ctrl+G: go to line */
        /* Simple: just show prompt in status — for now snap to beginning */
        set_status("Ctrl+G: type line# then Enter (not yet implemented)", C_GREY);
        return true;
    }
    if (key == 0x03) { /* Ctrl+C: copy current line to clipboard */
        line_ensure(g_cy);
        ipc_send(sock, IPC_CLIP_SET, g_lines[g_cy], (uint32_t)line_len(g_cy));
        set_status("Line copied to clipboard", C_SAVED);
        return true;
    }
    if (key == 0x16) { /* Ctrl+V: paste from clipboard */
        ipc_send(sock, IPC_CLIP_GET, NULL, 0);
        return true;
    }
    if (key == 0x0E) { /* Ctrl+N: find next */
        find_next();
        return true;
    }

    /* ── Navigation ── */
    if (key == 0x80) { /* Left */
        if (g_cx > 0) g_cx--;
        else if (g_cy > 0) { g_cy--; g_cx = line_len(g_cy); }
        clamp_scroll(); return true;
    }
    if (key == 0x81) { /* Right */
        int len = line_len(g_cy);
        if (g_cx < len) g_cx++;
        else if (g_cy < g_nlines - 1) { g_cy++; g_cx = 0; }
        clamp_scroll(); return true;
    }
    if (key == 0x82) { /* Up */
        if (g_cy > 0) {
            g_cy--;
            int len = line_len(g_cy);
            if (g_cx > len) g_cx = len;
        }
        clamp_scroll(); return true;
    }
    if (key == 0x83) { /* Down */
        if (g_cy < g_nlines - 1) {
            g_cy++;
            int len = line_len(g_cy);
            if (g_cx > len) g_cx = len;
        }
        clamp_scroll(); return true;
    }
    if (key == 0x85) { /* Home */
        g_cx = 0; clamp_scroll(); return true;
    }
    if (key == 0x86) { /* End */
        g_cx = line_len(g_cy); clamp_scroll(); return true;
    }
    if (key == 0x87) { /* PgUp */
        int vr = visible_rows();
        g_cy -= vr; if (g_cy < 0) g_cy = 0;
        int len = line_len(g_cy); if (g_cx > len) g_cx = len;
        clamp_scroll(); return true;
    }
    if (key == 0x88) { /* PgDn */
        int vr = visible_rows();
        g_cy += vr; if (g_cy >= g_nlines) g_cy = g_nlines - 1;
        int len = line_len(g_cy); if (g_cx > len) g_cx = len;
        clamp_scroll(); return true;
    }

    if (g_readonly) { set_status("Read-only (press Ctrl+Q to quit)", C_GREY); return true; }

    /* ── Editing ── */
    if (key == 0x0D || key == '\n') { /* Enter */
        undo_save();
        split_line(g_cy, g_cx);
        g_cy++; g_cx = 0;
        clamp_scroll();
        update_title(sock);
        return true;
    }
    if (key == 0x08 || key == 0x7F) { /* Backspace */
        undo_save();
        if (g_cx > 0) {
            g_cx--;
            delete_char(g_cy, g_cx);
        } else if (g_cy > 0) {
            int prev_len = line_len(g_cy - 1);
            join_with_next(g_cy - 1);
            g_cy--;
            g_cx = prev_len;
        }
        clamp_scroll();
        update_title(sock);
        return true;
    }
    if (key == 0x84) { /* Delete */
        undo_save();
        int len = line_len(g_cy);
        if (g_cx < len) {
            delete_char(g_cy, g_cx);
        } else if (g_cy < g_nlines - 1) {
            join_with_next(g_cy);
        }
        update_title(sock);
        return true;
    }
    if (key == 0x09) { /* Tab → 4 spaces */
        undo_save();
        int spaces = 4 - (g_cx % 4);
        for (int i = 0; i < spaces; i++) { insert_char(g_cy, g_cx, ' '); g_cx++; }
        clamp_scroll();
        update_title(sock);
        return true;
    }
    if (key >= 0x20 && key < 0x7F) { /* Printable */
        undo_save();
        insert_char(g_cy, g_cx, (char)key);
        g_cx++;
        clamp_scroll();
        update_title(sock);
        return true;
    }
    return true;
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (!font_load("/fifi-data/fonts/ter16b.psf"))
        fifi_ui_font_init_blank(&g_font, 256, 8, 16);

    /* Init with empty document (must be full-capacity: insert_char grows in place) */
    g_lines[0] = line_dup(""); g_nlines = 1;

    /* Load file if given on command line */
    if (argc >= 2) {
        snprintf(g_filepath, sizeof(g_filepath), "%s", argv[1]);
        file_load(g_filepath);
        g_modified = false;
    }

    uint32_t *fb = malloc((size_t)WIN_W * WIN_H * 4);
    if (!fb) return 1;

    /* Connect to compositor */
    int sock = fifi_app_ipc_connect(WIN_W, WIN_H, "Editor");
    if (sock < 0) return 1;

    /* Wait for WIN_CREATED */
    {
        uint8_t hdr8[8] = {0};
        if (read(sock, hdr8, 8) < 8) return 1;
        uint32_t tp, pl;
        memcpy(&tp, hdr8, 4); memcpy(&pl, hdr8 + 4, 4);
        if (tp == IPC_WIN_CREATED && pl >= 20) {
            uint8_t resp[20]; read(sock, resp, pl > 20 ? 20 : pl);
        }
    }

    update_title(sock);
    render(fb);
    send_frame(sock, fb);
    fcntl(sock, F_SETFL, O_NONBLOCK);

    /* Event loop */
    uint8_t in_hdr[8];
    int     in_got = 0;
    uint8_t *in_pld = NULL;
    uint32_t in_plen = 0, in_pgot = 0;
    uint32_t in_type = 0;
    bool     running = true;
    bool     dirty = false;

    while (running) {
        uint8_t tbuf[512];
        ssize_t n = read(sock, tbuf, sizeof(tbuf));
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
        if (n == 0) break;

        if (n > 0) {
            ssize_t pos = 0;
            while (pos < n) {
                if (in_got < 8) {
                    in_hdr[in_got++] = tbuf[pos++];
                    if (in_got == 8) {
                        memcpy(&in_type,  in_hdr,     4);
                        memcpy(&in_plen,  in_hdr + 4, 4);
                        if (in_plen > 131072) { in_got = 0; break; }
                        if (in_plen > 0) { free(in_pld); in_pld = malloc(in_plen); in_pgot = 0; }
                        else {
                            /* zero-payload message: dispatch immediately */
                            if (in_type == IPC_INVALIDATE) dirty = true;
                            in_got = 0;
                        }
                    }
                } else if (in_plen > 0 && in_pgot < in_plen) {
                    uint32_t need = in_plen - in_pgot;
                    uint32_t have = (uint32_t)(n - pos);
                    uint32_t take = need < have ? need : have;
                    if (in_pld) memcpy(in_pld + in_pgot, tbuf + pos, take);
                    in_pgot += take; pos += take;
                    if (in_pgot >= in_plen) {
                        if (in_type == IPC_INPUT_KEY && in_plen >= 1) {
                            uint8_t key = in_pld ? in_pld[0] : 0;
                            running = handle_key(key, sock, fb);
                            dirty = true;
                        } else if (in_type == IPC_OPEN_FILE && in_pld && in_plen < (uint32_t)sizeof(g_filepath)) {
                            /* Compositor routing another file to us */
                            memcpy(g_filepath, in_pld, in_plen);
                            g_filepath[in_plen] = '\0';
                            file_load(g_filepath);
                            g_modified = false;
                            g_cy = 0; g_cx = 0; g_scroll = 0; g_col_scroll = 0;
                            update_title(sock);
                            dirty = true;
                        } else if (in_type == IPC_CLIP_DATA && in_plen > 0) {
                            /* Paste clipboard into document */
                            if (in_pld) {
                                undo_save();
                                for (uint32_t ci = 0; ci < in_plen; ci++) {
                                    uint8_t ch = in_pld[ci];
                                    if (ch == '\n' || ch == '\r') {
                                        split_line(g_cy, g_cx);
                                        g_cy++; g_cx = 0;
                                    } else if (ch >= 0x20 && ch < 0x7F) {
                                        insert_char(g_cy, g_cx, (char)ch);
                                        g_cx++;
                                    }
                                }
                                clamp_scroll();
                                update_title(sock);
                                dirty = true;
                            }
                        } else if (in_type == IPC_INPUT_MOUSE && in_plen >= 9) {
                            int32_t mx, my; uint8_t btns;
                            memcpy(&mx, in_pld, 4); memcpy(&my, in_pld + 4, 4);
                            btns = in_pld[8];
                            /* Left click: move cursor to clicked position */
                            if (btns & 1) {
                                int cell_h = g_glyph_h + 1;
                                int list_top = TITLE_H + HDR_H;
                                int list_bot = WIN_H - FOOT_H;
                                if (my >= list_top && my < list_bot) {
                                    int row = (my - list_top) / cell_h;
                                    int line_idx = g_scroll + row;
                                    if (line_idx >= 0 && line_idx < g_nlines) {
                                        int tx = LNUM_W + PAD_X;
                                        int col = (mx - tx) / 9 + g_col_scroll;
                                        g_cy = line_idx;
                                        int len = line_len(g_cy);
                                        g_cx = col < 0 ? 0 : (col > len ? len : col);
                                        clamp_scroll();
                                        dirty = true;
                                    }
                                }
                            }
                            /* Scroll wheel */
                            if (in_plen >= 10) {
                                int8_t scroll = (int8_t)in_pld[9];
                                if (scroll != 0) {
                                    g_scroll -= scroll;
                                    if (g_scroll < 0) g_scroll = 0;
                                    if (g_scroll > g_nlines - 1) g_scroll = g_nlines - 1;
                                    dirty = true;
                                }
                            }
                        } else if (in_type == IPC_WIN_RESIZE) {
                            dirty = true;
                        }
                        free(in_pld); in_pld = NULL;
                        in_got = 0; in_plen = 0; in_pgot = 0;
                    }
                } else {
                    if (in_type == IPC_INVALIDATE) dirty = true;
                    in_got = 0; in_plen = 0; in_pgot = 0;
                }
            }
        }

        if (dirty) {
            render(fb);
            send_frame(sock, fb);
            dirty = false;
        }

        struct timespec ts = { 0, 8000000 };
        nanosleep(&ts, NULL);
    }

    ipc_send(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock);
    free(fb);
    fifi_ui_font_destroy(&g_font);
    for (int i = 0; i < g_nlines; i++) free(g_lines[i]);
    for (int i = 0; i < g_undo_nlines; i++) free(g_undo_lines[i]);
    return 0;
}
