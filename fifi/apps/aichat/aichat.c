/* FiFi AI — windowed chat client for the local offline AI model.
 *
 * Connects to the hand-rolled Wayland compositor over the same IPC framebuffer
 * protocol used by fifi-terminal / fifi-editor (see /tmp/fifi-compositor.sock).
 * Shows a scrollable chat transcript with an input line at the bottom; Enter
 * sends. Talks to the resident local model server started by /usr/bin/
 * fifi-ai-serve (llama-server on http://127.0.0.1:8080) by POSTing JSON to
 * /completion via curl, run in a forked child so the UI stays responsive while
 * the model generates.
 *
 * Build: gcc -O2 -static -o fifi-aichat aichat.c
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
#include <sys/stat.h>
#include <poll.h>
#include <signal.h>
#include <time.h>

#include "../fifi_u8.h"

/* ── IPC protocol (subset shared with the other native apps) ─────────────── */
#include "../../shared/app_ipc.h"

/* ── Window geometry ─────────────────────────────────────────────────────── */
#define DEF_WIN_W  780
#define DEF_WIN_H  620
#define HEADER_H    46
#define INPUT_H     50
#define PAD         14
#define BUB_PAD      8
#define LINE_GAP     2
#define MSG_GAP     12

/* ── AI request plumbing ─────────────────────────────────────────────────── */
#define REQ_PATH   "/tmp/fifi-aichat-req.json"
#define NOSERVER_TAG "\x01NOSERVER"
#define SYS_PROMPT \
    "You are FiFi, a friendly and concise assistant running fully offline on " \
    "the user's own PC. Keep answers short and to the point. IMPORTANT: you " \
    "are a text-only chat model with NO ability to run commands, install " \
    "software, change settings, or access files. Never claim to have performed " \
    "any such action. If asked to do something on the computer, briefly " \
    "explain that you can only give step-by-step instructions the user can " \
    "follow themselves."

/* ── Font (PSF1/PSF2) — same loader as fifi-terminal ─────────────────────── */
static const char *FONT_PATHS[3] = {
    "/fifi-data/fonts/ter16b.psf",
    "/fifi-data/fonts/ter20b.psf",
    "/fifi-data/fonts/ter24b.psf",
};
static int g_font_idx = 1;

typedef struct {
    uint8_t  *data;
    int       n, sz, w, h;
    uint32_t *cps;
    uint16_t *gis;
    int       nc;
} Font;

static Font g_font;

static bool font_load(Font *f, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 4) { close(fd); return false; }
    uint8_t *raw = malloc((size_t)st.st_size);
    if (!raw) { close(fd); return false; }
    if (read(fd, raw, (size_t)st.st_size) < st.st_size) { free(raw); close(fd); return false; }
    close(fd);

    free(f->data); free(f->cps); free(f->gis);
    memset(f, 0, sizeof(*f));

    if (st.st_size >= 32 && raw[0]==0x72 && raw[1]==0xb5 && raw[2]==0x4a && raw[3]==0x86) {
        uint32_t hdr_size, flags, length, glyph_size, height, width;
        memcpy(&hdr_size,   raw+8,  4);
        memcpy(&flags,      raw+12, 4);
        memcpy(&length,     raw+16, 4);
        memcpy(&glyph_size, raw+20, 4);
        memcpy(&height,     raw+24, 4);
        memcpy(&width,      raw+28, 4);
        /* Reject malformed headers whose glyph table exceeds the file */
        if (length == 0 || glyph_size == 0 || hdr_size < 32 ||
            (uint64_t)hdr_size + (uint64_t)length * glyph_size > (uint64_t)st.st_size) {
            free(raw); return false;
        }
        f->n = (int)length; f->sz = (int)glyph_size; f->w = (int)width; f->h = (int)height;
        f->data = malloc((size_t)length * glyph_size);
        if (!f->data) { free(raw); return false; }
        memcpy(f->data, raw + hdr_size, (size_t)length * glyph_size);

        if ((flags & 1) && hdr_size + length * glyph_size < (uint32_t)st.st_size) {
            size_t tpos = hdr_size + length * glyph_size;
            int cap = 2048, cnt = 0;
            uint32_t *tcps = malloc((size_t)cap * sizeof(uint32_t));
            uint16_t *tgis = malloc((size_t)cap * sizeof(uint16_t));
            if (!tcps || !tgis) { free(tcps); free(tgis); free(raw); return false; }
            uint16_t gi = 0;
            while (tpos < (size_t)st.st_size && gi < (uint16_t)length) {
                uint8_t b = raw[tpos];
                if (b == 0xFF) { gi++; tpos++; continue; }
                if (b == 0xFE) { tpos++; continue; }
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
                    uint16_t *ngis = realloc(tgis, (size_t)cap * sizeof(uint16_t));
                    if (ncps) tcps = ncps;
                    if (ngis) tgis = ngis;
                    if (!ncps || !ngis) { free(tcps); free(tgis); tcps = NULL; tgis = NULL; cnt = 0; break; }
                }
                tcps[cnt] = cp; tgis[cnt] = gi; cnt++;
            }
            for (int i = 1; i < cnt; i++) {
                uint32_t kcp = tcps[i]; uint16_t kgi = tgis[i]; int j = i - 1;
                while (j >= 0 && tcps[j] > kcp) { tcps[j+1]=tcps[j]; tgis[j+1]=tgis[j]; j--; }
                tcps[j+1] = kcp; tgis[j+1] = kgi;
            }
            f->cps = tcps; f->gis = tgis; f->nc = cnt;
        }
    } else if (st.st_size >= 4 && raw[0]==0x36 && raw[1]==0x04) {
        uint8_t mode = raw[2], charsize = raw[3];
        f->n = (mode & 1) ? 512 : 256; f->sz = charsize; f->w = 8; f->h = charsize;
        if (f->sz == 0 || 4 + (size_t)f->n * f->sz > (size_t)st.st_size) { free(raw); return false; }
        f->data = malloc((size_t)f->n * f->sz);
        if (!f->data) { free(raw); return false; }
        memcpy(f->data, raw + 4, (size_t)f->n * f->sz);
    } else { free(raw); return false; }

    free(raw);
    return true;
}

static uint16_t font_glyph(const Font *f, uint32_t cp) {
    if (f->nc > 0) {
        int lo = 0, hi = f->nc - 1;
        while (lo <= hi) {
            int mid = (lo + hi) >> 1;
            if (f->cps[mid] == cp) return f->gis[mid];
            if (f->cps[mid]  < cp) lo = mid + 1; else hi = mid - 1;
        }
        return 0xFFFF;
    }
    if (cp < (uint32_t)f->n) return (uint16_t)cp;
    return 0xFFFF;
}

/* ── Colors ──────────────────────────────────────────────────────────────── */
#define COL_BG        0xFF0e1418u
#define COL_HEADER    0xFF14202Cu
#define COL_ACCENT    0xFF4488ccu
#define COL_BORDER    0xFF2a4060u
#define COL_TITLE     0xFFe8f0f8u
#define COL_SUBTITLE  0xFF8098b0u
#define COL_USER_BUB  0xFF244a72u
#define COL_USER_TXT  0xFFeef4fbu
#define COL_AI_BUB    0xFF182632u
#define COL_AI_TXT    0xFFd8e8f8u
#define COL_ROLE_TXT  0xFF88a4c0u
#define COL_INPUT_BAR 0xFF0b1117u
#define COL_INPUT_BOX 0xFF16232fu
#define COL_INPUT_TXT 0xFFe8f0f8u
#define COL_PLACEHLD  0xFF5a7290u

/* ── Framebuffer / window state ──────────────────────────────────────────── */
static int       g_win_w  = DEF_WIN_W;
static int       g_win_h  = DEF_WIN_H;
static uint32_t *g_fb     = NULL;
static uint32_t  g_tick   = 0;

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
static void fb_hline(int x, int y, int w, uint32_t c) { fb_fill(x, y, w, 1, c); }

/* Rounded-corner filled rect (subtle 2px radius) for chat bubbles. */
static void fb_round_rect(int x, int y, int w, int h, uint32_t c) {
    fb_fill(x, y, w, h, c);
    /* knock out the 4 corner pixels for a softer look */
    fb_set(x, y, COL_BG);           fb_set(x+w-1, y, COL_BG);
    fb_set(x, y+h-1, COL_BG);       fb_set(x+w-1, y+h-1, COL_BG);
}

static void fb_glyph(int px, int py, uint32_t cp, uint32_t fg, uint32_t bg, bool draw_bg) {
    const Font *f = &g_font;
    uint16_t gi = font_glyph(f, cp);
    if (gi == 0xFFFF) { gi = font_glyph(f, '?'); if (gi == 0xFFFF) gi = font_glyph(f, ' '); }
    if (gi == 0xFFFF || gi >= (uint16_t)f->n) gi = 0;
    const uint8_t *bits = f->data + (size_t)gi * f->sz;
    int bpr = (f->w + 7) / 8;
    for (int row = 0; row < f->h; row++) {
        for (int col = 0; col < f->w; col++) {
            int on = bits[row * bpr + col / 8] & (0x80u >> (col & 7));
            if (on) fb_set(px + col, py + row, fg);
            else if (draw_bg) fb_set(px + col, py + row, bg);
        }
    }
}

/* Draw a byte range of a string as UTF-8: each codepoint is folded to a byte
 * the bitmap font can show (dashes/quotes/… → ASCII) and drawn in one cell.
 * Zero-width codepoints are skipped without advancing. Returns x after last. */
static int fb_text(int px, int py, const char *s, int len, uint32_t fg) {
    int cw = g_font.w + 1;
    size_t i = 0;
    while ((int)i < len) {
        uint32_t cp = fifi_u8_next(s, &i);
        int c = fifi_fold_ascii(cp);
        if (c == 0) continue;            /* zero-width: no glyph, no advance */
        if (c < 0x20) c = ' ';
        fb_glyph(px, py, (uint32_t)c, fg, 0, false);
        px += cw;
    }
    return px;
}
static int fb_str(int px, int py, const char *s, uint32_t fg) {
    return fb_text(px, py, s, (int)strlen(s), fg);
}

/* Display-cell count of a byte-bounded UTF-8 slice (matches fb_text's advance:
 * one cell per codepoint, zero-width skipped). */
static int slice_cols(const char *p, int len) {
    size_t i = 0; int cols = 0;
    while ((int)i < len) {
        uint32_t cp = fifi_u8_next(p, &i);
        if (fifi_fold_ascii(cp) != 0) cols++;
    }
    return cols;
}

/* ── Chat transcript model ───────────────────────────────────────────────── */
#define ROLE_USER 0
#define ROLE_AI   1
#define MAX_MSGS  256

typedef struct { int role; char *text; } Msg;
static Msg  g_msgs[MAX_MSGS];
static int  g_nmsgs = 0;

static void msgs_add(int role, const char *text) {
    char *dup = strdup(text ? text : "");
    if (!dup) return;
    if (g_nmsgs >= MAX_MSGS) {
        free(g_msgs[0].text);
        memmove(&g_msgs[0], &g_msgs[1], sizeof(Msg) * (MAX_MSGS - 1));
        g_nmsgs = MAX_MSGS - 1;
    }
    g_msgs[g_nmsgs].role = role;
    g_msgs[g_nmsgs].text = dup;
    g_nmsgs++;
}

/* ── Input line ──────────────────────────────────────────────────────────── */
static char g_input[2048];
static int  g_input_len = 0;

/* ── Scroll / async state ────────────────────────────────────────────────── */
static int   g_scroll      = 0;      /* px offset from top of content */
static bool  g_stick       = true;   /* auto-follow bottom */
static bool  g_thinking    = false;
static int   g_ai_pipe     = -1;
static pid_t g_ai_pid      = -1;
static char *g_resp        = NULL;   /* accumulated child output */
static size_t g_resp_len   = 0, g_resp_cap = 0;

/* ── Word wrap ───────────────────────────────────────────────────────────── */
typedef struct { const char *p; int len; } Line;

/* UTF-8 aware: columns are counted per codepoint (not per byte) so bubble
 * width lines up, and line boundaries never split a multi-byte sequence. */
static int wrap_text(const char *s, int maxc, Line *out, int maxl) {
    if (maxc < 1) maxc = 1;
    int n = 0;
    size_t i = 0, len = strlen(s);
    while (i < len && n < maxl) {
        size_t start = i;
        long last_space = -1;               /* byte index of last space */
        int col = 0;
        while (i < len) {
            if (s[i] == '\n') break;         /* i at newline (not consumed) */
            if (col >= maxc) break;          /* i at start of overflow codepoint */
            size_t prev = i;
            uint32_t cp = fifi_u8_next(s, &i);
            if (cp == ' ') last_space = (long)prev;
            col++;
        }
        size_t end = i;
        if (i < len && s[i] == '\n') {
            end = i; i++;                                  /* consume newline */
        } else if (i < len && last_space > (long)start) {
            end = (size_t)last_space; i = (size_t)last_space + 1; /* word break */
        }
        out[n].p = s + start; out[n].len = (int)(end - start); n++;
    }
    return n;
}

/* ── Layout metrics ──────────────────────────────────────────────────────── */
static int lh(void)      { return g_font.h + LINE_GAP; }
static int chat_top(void){ return HEADER_H + PAD; }
static int chat_bot(void){ return g_win_h - INPUT_H - PAD; }
static int bubble_maxc(void) {
    int cw = g_font.w + 1;
    int content_w = g_win_w - 2 * PAD;
    int bw = content_w * 78 / 100 - 2 * BUB_PAD;
    int mc = bw / cw;
    if (mc < 4) mc = 4;
    return mc;
}

static Line g_lines[512];

/* Height in px of one message (before gap). */
static int msg_height(const Msg *m) {
    int nl = wrap_text(m->text, bubble_maxc(), g_lines, 512);
    if (nl < 1) nl = 1;
    return lh() /* role label */ + nl * lh() + 2 * BUB_PAD;
}

static int content_height(void) {
    int h = 0;
    for (int i = 0; i < g_nmsgs; i++) h += msg_height(&g_msgs[i]) + MSG_GAP;
    if (g_thinking) h += lh() * 2 + 2 * BUB_PAD + MSG_GAP;
    return h;
}

/* ── Render ──────────────────────────────────────────────────────────────── */
static void draw_bubble(int y, const Msg *m) {
    int cw = g_font.w + 1;
    int content_w = g_win_w - 2 * PAD;
    int maxc = bubble_maxc();
    int nl = wrap_text(m->text, maxc, g_lines, 512);
    if (nl < 1) { g_lines[0].p = ""; g_lines[0].len = 0; nl = 1; }

    /* widest line → bubble width (measured in display cells, not bytes) */
    int widest = 0;
    for (int i = 0; i < nl; i++) {
        int lc = slice_cols(g_lines[i].p, g_lines[i].len);
        if (lc > widest) widest = lc;
    }
    int bw = widest * cw + 2 * BUB_PAD;
    if (bw > content_w) bw = content_w;
    int bh = nl * lh() + 2 * BUB_PAD;

    bool user = (m->role == ROLE_USER);
    int bx = user ? (PAD + content_w - bw) : PAD;
    uint32_t bub = user ? COL_USER_BUB : COL_AI_BUB;
    uint32_t txt = user ? COL_USER_TXT : COL_AI_TXT;

    /* role label above the bubble */
    const char *role = user ? "You" : "FiFi";
    int rlw = (int)strlen(role) * cw;
    int rlx = user ? (PAD + content_w - rlw) : PAD;
    fb_text(rlx, y, role, (int)strlen(role), COL_ROLE_TXT);
    y += lh();

    fb_round_rect(bx, y, bw, bh, bub);
    if (user) fb_fill(bx, y + 2, 2, bh - 4, COL_ACCENT);      /* accent edge */
    else      fb_fill(bx + bw - 2, y + 2, 2, bh - 4, COL_BORDER);

    int tx = bx + BUB_PAD;
    int ty = y + BUB_PAD;
    for (int i = 0; i < nl; i++) {
        fb_text(tx, ty, g_lines[i].p, g_lines[i].len, txt);
        ty += lh();
    }
}

static void render(void) {
    if (!g_fb) return;
    fb_fill(0, 0, g_win_w, g_win_h, COL_BG);

    /* ── Header bar ── */
    fb_fill(0, 0, g_win_w, HEADER_H, COL_HEADER);
    fb_hline(0, HEADER_H - 1, g_win_w, COL_BORDER);
    /* accent dot */
    fb_fill(PAD, HEADER_H/2 - 4, 8, 8, COL_ACCENT);
    int hx = PAD + 8 + 8;
    int hy = (HEADER_H - g_font.h) / 2;
    hx = fb_str(hx, hy, "FiFi AI", COL_TITLE);
    fb_str(hx + 12, hy, "offline assistant", COL_SUBTITLE);

    /* ── Chat area (clipped to [chat_top, chat_bot]) ── */
    int top = chat_top(), bot = chat_bot();
    int total = content_height();
    int view = bot - top;
    int maxscroll = total - view; if (maxscroll < 0) maxscroll = 0;
    if (g_stick) g_scroll = maxscroll;
    if (g_scroll > maxscroll) g_scroll = maxscroll;
    if (g_scroll < 0) g_scroll = 0;

    int y = top - g_scroll;
    for (int i = 0; i < g_nmsgs; i++) {
        int mh = msg_height(&g_msgs[i]);
        if (y + mh >= top && y <= bot) draw_bubble(y, &g_msgs[i]);
        y += mh + MSG_GAP;
    }
    if (g_thinking) {
        int dots = (g_tick / 20) % 4;
        char buf[8] = "   ";
        for (int i = 0; i < dots && i < 3; i++) buf[i] = '.';
        Msg tm = { ROLE_AI, buf };
        if (y <= bot) draw_bubble(y, &tm);
    }

    /* mask anything drawn outside the chat viewport (header/input overlap) */
    fb_fill(0, HEADER_H, g_win_w, top - HEADER_H, COL_BG);
    fb_fill(0, bot, g_win_w, g_win_h - INPUT_H - bot, COL_BG);

    /* scrollbar */
    if (total > view) {
        int sbx = g_win_w - 6;
        fb_fill(sbx, top, 4, view, COL_HEADER);
        int thumb = view * view / total; if (thumb < 16) thumb = 16;
        int ty = top + (view - thumb) * g_scroll / maxscroll;
        if (ty + thumb > top + view) ty = top + view - thumb;
        fb_fill(sbx, ty, 4, thumb, COL_BORDER);
    }

    /* ── Input bar ── */
    int iy = g_win_h - INPUT_H;
    fb_fill(0, iy, g_win_w, INPUT_H, COL_INPUT_BAR);
    fb_hline(0, iy, g_win_w, COL_BORDER);
    int box_x = PAD, box_y = iy + 8, box_w = g_win_w - 2 * PAD, box_h = INPUT_H - 16;
    fb_round_rect(box_x, box_y, box_w, box_h, COL_INPUT_BOX);
    fb_hline(box_x, box_y, box_w, COL_BORDER);

    int cw = g_font.w + 1;
    int txt_x = box_x + 8;
    int txt_y = box_y + (box_h - g_font.h) / 2;
    int avail_chars = (box_w - 16 - cw) / cw;   /* leave room for cursor */
    if (avail_chars < 1) avail_chars = 1;

    if (g_input_len == 0 && !g_thinking) {
        fb_str(txt_x, txt_y, "Type a message and press Enter", COL_PLACEHLD);
    } else if (g_thinking && g_input_len == 0) {
        fb_str(txt_x, txt_y, "FiFi is thinking...", COL_PLACEHLD);
    } else {
        int start = 0;
        if (g_input_len > avail_chars) start = g_input_len - avail_chars;
        int shown = g_input_len - start;
        int ex = fb_text(txt_x, txt_y, g_input + start, shown, COL_INPUT_TXT);
        /* blinking caret */
        if ((g_tick / 25) % 2 == 0)
            fb_fill(ex + 1, box_y + 4, 2, box_h - 8, COL_ACCENT);
    }
}

/* ── IPC helpers ─────────────────────────────────────────────────────────── */
static void ipc_send(int fd, uint32_t type, const void *data, uint32_t len) {
    (void)fifi_app_ipc_send(fd, type, data, len);
}
static void send_frame(int fd) {
    if (!g_fb) return;
    (void)fifi_app_ipc_send_frame(fd, (uint16_t)g_win_w, (uint16_t)g_win_h, g_fb);
}

/* ── JSON helpers ────────────────────────────────────────────────────────── */
static void json_escape_to(FILE *fp, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\n': fputs("\\n", fp);  break;
        case '\r': fputs("\\r", fp);  break;
        case '\t': fputs("\\t", fp);  break;
        default:
            if (c < 0x20) fprintf(fp, "\\u%04x", c);
            else          fputc(c, fp);
        }
    }
}

/* Extract the "content" string from a llama-server /completion JSON reply and
 * return a newly malloc'd, unescaped copy. NULL if not found. */
static char *json_get_content(const char *json) {
    const char *k = strstr(json, "\"content\"");
    if (!k) return NULL;
    k += 9;
    while (*k && *k != ':') k++;
    if (*k != ':') return NULL;
    k++;
    while (*k == ' ' || *k == '\t' || *k == '\n' || *k == '\r') k++;
    if (*k != '"') return NULL;
    k++;
    size_t cap = 256, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    while (*k && *k != '"') {
        char ch;
        if (*k == '\\') {
            k++;
            switch (*k) {
            case 'n': ch = '\n'; break;
            case 't': ch = '\t'; break;
            case 'r': ch = '\r'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case '/': ch = '/';  break;
            case '"': ch = '"';  break;
            case '\\': ch = '\\'; break;
            case 'u': {
                /* decode \uXXXX → UTF-8 (BMP only, good enough) */
                if (!k[1] || !k[2] || !k[3] || !k[4]) { ch = '?'; break; }
                char hex[5] = { k[1], k[2], k[3], k[4], 0 };
                unsigned cp = (unsigned)strtol(hex, NULL, 16);
                k += 4;
                if (len + 4 >= cap) {
                    cap *= 2;
                    char *no = realloc(out, cap);
                    if (!no) { free(out); return NULL; }
                    out = no;
                }
                if (cp < 0x80) { out[len++] = (char)cp; }
                else if (cp < 0x800) {
                    out[len++] = (char)(0xC0 | (cp >> 6));
                    out[len++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    out[len++] = (char)(0xE0 | (cp >> 12));
                    out[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[len++] = (char)(0x80 | (cp & 0x3F));
                }
                k++;
                continue;
            }
            default: ch = *k; break;
            }
            if (!*k) break;
            k++;
        } else {
            ch = *k++;
        }
        if (len + 1 >= cap) {
            cap *= 2;
            char *no = realloc(out, cap);
            if (!no) { free(out); return NULL; }
            out = no;
        }
        out[len++] = ch;
    }
    out[len] = '\0';
    /* trim leading whitespace/newlines the model may emit */
    char *b = out;
    while (*b == ' ' || *b == '\n' || *b == '\r' || *b == '\t') b++;
    if (b != out) memmove(out, b, strlen(b) + 1);
    /* trim trailing whitespace */
    size_t l = strlen(out);
    while (l > 0 && (out[l-1] == ' ' || out[l-1] == '\n' || out[l-1] == '\r' || out[l-1] == '\t'))
        out[--l] = '\0';
    return out;
}

/* ── Build the request body file from conversation history ───────────────── */
static void write_request_file(void) {
    FILE *fp = fopen(REQ_PATH, "w");
    if (!fp) return;
    fputs("{\"prompt\":\"", fp);
    json_escape_to(fp, SYS_PROMPT);
    /* include a trailing window of history to stay within context */
    int start = 0;
    if (g_nmsgs > 16) start = g_nmsgs - 16;
    for (int i = start; i < g_nmsgs; i++) {
        json_escape_to(fp, g_msgs[i].role == ROLE_USER ? "\nUser: " : "\nAssistant: ");
        json_escape_to(fp, g_msgs[i].text);
    }
    json_escape_to(fp, "\nAssistant:");
    fputs("\",\"n_predict\":400,\"temperature\":0.4,\"stop\":[\"\\nUser:\"]}", fp);
    fclose(fp);
}

/* ── Launch an async AI request in a child process ───────────────────────── */
static void write_pipe_all(int fd, const void *data, size_t length) {
    const uint8_t *bytes = data;
    while (length > 0) {
        ssize_t written = write(fd, bytes, length);
        if (written > 0) {
            bytes += written;
            length -= (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

static void ai_start(void) {
    write_request_file();
    int pfd[2];
    if (pipe(pfd) < 0) { msgs_add(ROLE_AI, "(internal error: pipe failed)"); return; }

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); msgs_add(ROLE_AI, "(internal error: fork failed)"); return; }
    if (pid == 0) {
        /* child */
        close(pfd[0]);
        /* make our write end stdout so popen'd curl streams straight through */
        dup2(pfd[1], 1);
        if (pfd[1] != 1) close(pfd[1]);
        /* ensure the resident server is up (warms/reuses the model) */
        int rc = system("fifi-ai-serve >/dev/null 2>&1");
        if (rc != 0) {
            /* server unavailable / no model installed */
            const char *tag = NOSERVER_TAG;
            write_pipe_all(1, tag, strlen(tag));
            _exit(0);
        }
        FILE *fp = popen("curl -s -m 600 -X POST "
                         "http://127.0.0.1:8080/completion "
                         "-H 'Content-Type: application/json' "
                         "--data-binary @" REQ_PATH, "r");
        if (fp) {
            char buf[4096]; size_t n;
            while ((n = fread(buf, 1, sizeof buf, fp)) > 0) write_pipe_all(1, buf, n);
            pclose(fp);
        }
        _exit(0);
    }
    /* parent */
    close(pfd[1]);
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    g_ai_pipe = pfd[0];
    g_ai_pid  = pid;
    g_thinking = true;
    free(g_resp); g_resp = NULL; g_resp_len = g_resp_cap = 0;
}

static void ai_append(const char *buf, size_t n) {
    if (g_resp_len + n + 1 > g_resp_cap) {
        size_t nc = g_resp_cap ? g_resp_cap * 2 : 4096;
        while (nc < g_resp_len + n + 1) nc *= 2;
        char *nr = realloc(g_resp, nc);
        if (!nr) return;
        g_resp = nr; g_resp_cap = nc;
    }
    memcpy(g_resp + g_resp_len, buf, n);
    g_resp_len += n;
    g_resp[g_resp_len] = '\0';
}

static void ai_finish(void) {
    if (g_ai_pid > 0) { int st; waitpid(g_ai_pid, &st, 0); }
    if (g_ai_pipe >= 0) { close(g_ai_pipe); g_ai_pipe = -1; }
    g_ai_pid = -1;
    g_thinking = false;

    if (g_resp && strncmp(g_resp, NOSERVER_TAG, strlen(NOSERVER_TAG)) == 0) {
        msgs_add(ROLE_AI,
            "No local AI model is available yet. Open a terminal and run "
            "'fifi-ai-install' to download one, then try again.");
    } else if (g_resp && g_resp_len > 0) {
        char *content = json_get_content(g_resp);
        if (content && content[0]) { msgs_add(ROLE_AI, content); }
        else msgs_add(ROLE_AI, "(no response from the model)");
        free(content);
    } else {
        msgs_add(ROLE_AI, "(no response - the model server may not be running)");
    }
    free(g_resp); g_resp = NULL; g_resp_len = g_resp_cap = 0;
    g_stick = true;
}

/* ── Send the current input line ─────────────────────────────────────────── */
static void submit_input(void) {
    if (g_thinking || g_input_len == 0) return;
    g_input[g_input_len] = '\0';
    msgs_add(ROLE_USER, g_input);
    g_input_len = 0; g_input[0] = '\0';
    g_stick = true;
    ai_start();
}

/* ── Resize ──────────────────────────────────────────────────────────────── */
static void resize_to(int nw, int nh) {
    if (nw < 320) nw = 320;
    if (nh < 240) nh = 240;
    if (nw > 8192) nw = 8192;
    if (nh > 8192) nh = 8192;
    if (nw == g_win_w && nh == g_win_h) return;
    uint32_t *nf = malloc((size_t)nw * (size_t)nh * 4);
    if (!nf) return;   /* keep old fb AND old dims consistent */
    free(g_fb); g_fb = nf;
    g_win_w = nw; g_win_h = nh;
}

/* ── Key handling ────────────────────────────────────────────────────────── */
static void handle_key(uint8_t k) {
    if (k == 0x0D || k == '\n') { submit_input(); return; }
    if (k == 0x08 || k == 0x7F) {            /* backspace */
        if (g_input_len > 0) g_input[--g_input_len] = '\0';
        return;
    }
    if (k == 0x87) { g_scroll -= (chat_bot()-chat_top())/2; g_stick=false; return; } /* PgUp */
    if (k == 0x88) { g_scroll += (chat_bot()-chat_top())/2; g_stick=false; return; } /* PgDn */
    if (k == 0x82) { g_scroll -= lh() * 3; g_stick=false; return; }                  /* Up */
    if (k == 0x83) { g_scroll += lh() * 3; g_stick=false; return; }                  /* Down */
    if (k >= 0x20 && k < 0x7F) {             /* printable */
        if (g_input_len < (int)sizeof(g_input) - 1) {
            g_input[g_input_len++] = (char)k;
            g_input[g_input_len] = '\0';
        }
    }
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void) {
    if (!font_load(&g_font, FONT_PATHS[g_font_idx])) {
        g_font_idx = 0;
        if (!font_load(&g_font, FONT_PATHS[0])) return 1;
    }

    g_fb = malloc((size_t)DEF_WIN_W * DEF_WIN_H * 4);
    if (!g_fb) return 1;

    int sock = fifi_app_ipc_connect(DEF_WIN_W, DEF_WIN_H, "FiFi AI");
    if (sock < 0) return 1;

    {
        uint8_t hdr[8] = {0};
        if (read(sock, hdr, 8) < 8) { close(sock); return 1; }
        uint32_t tp, pl;
        memcpy(&tp, hdr, 4); memcpy(&pl, hdr+4, 4);
        if (tp == IPC_WIN_CREATED && pl > 0) {
            uint8_t resp[64]; ssize_t rn = read(sock, resp, pl > 64 ? 64 : pl); (void)rn;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);

    {
        char title[16] = "FiFi AI";
        ipc_send(sock, IPC_APP_TITLE, title, (uint32_t)strlen(title));
    }

    msgs_add(ROLE_AI,
        "Hi! I'm FiFi, your offline assistant running right here on this PC. "
        "Ask me anything and I'll do my best to help.");

    render();
    send_frame(sock);

    uint8_t  in_hdr[8];
    int      in_got  = 0;
    uint8_t *in_pld  = NULL;
    uint32_t in_plen = 0, in_pgot = 0, in_type = 0;
    bool     running = true;
    bool     dirty   = false;

    while (running) {
        struct pollfd pfds[2];
        int nfds = 0;
        pfds[nfds].fd = sock; pfds[nfds].events = POLLIN; pfds[nfds].revents = 0; nfds++;
        int ai_idx = -1;
        if (g_ai_pipe >= 0) { ai_idx = nfds; pfds[nfds].fd = g_ai_pipe; pfds[nfds].events = POLLIN; pfds[nfds].revents = 0; nfds++; }

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
                        memcpy(&in_type, in_hdr, 4); memcpy(&in_plen, in_hdr + 4, 4);
                        if (in_plen > 262144) { in_got = 0; break; }
                        if (in_plen > 0) { free(in_pld); in_pld = malloc(in_plen); in_pgot = 0; }
                        else {
                            /* zero-payload message: dispatch immediately */
                            if (in_type == IPC_INVALIDATE) dirty = true;
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
                        if (in_type == IPC_INPUT_KEY && in_plen >= 1) {
                            handle_key(in_pld ? in_pld[0] : 0);
                            dirty = true;
                        } else if (in_type == IPC_INPUT_MOUSE && in_plen >= 10) {
                            int8_t wheel = (int8_t)in_pld[9];
                            if (wheel != 0) {
                                g_scroll -= wheel * lh() * 3;
                                g_stick = false;
                                dirty = true;
                            }
                        } else if (in_type == IPC_WIN_RESIZE && in_plen >= 4) {
                            uint16_t nw, nh;
                            memcpy(&nw, in_pld, 2); memcpy(&nh, in_pld+2, 2);
                            resize_to((int)nw, (int)nh);
                            dirty = true;
                        } else if (in_type == IPC_INVALIDATE) {
                            dirty = true;
                        }
                        free(in_pld); in_pld = NULL;
                        in_got = 0; in_plen = 0; in_pgot = 0;
                    }
                } else { in_got = 0; in_plen = 0; in_pgot = 0; }
            }
        }
        if (pfds[0].revents & (POLLHUP | POLLERR)) { running = false; break; }

        /* ── AI child output ── */
        if (ai_idx >= 0 && (pfds[ai_idx].revents & (POLLIN | POLLHUP))) {
            char buf[4096];
            ssize_t n;
            bool eof = false;
            while ((n = read(g_ai_pipe, buf, sizeof buf)) > 0) ai_append(buf, (size_t)n);
            if (n == 0) eof = true;
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) eof = true;
            if (eof) { ai_finish(); dirty = true; }
        }

        /* ── Animate while thinking / caret blink ── */
        g_tick++;
        if (g_thinking && (g_tick % 20) == 0) dirty = true;
        if ((g_tick % 25) == 0) dirty = true;   /* caret blink */

        if (dirty && g_fb) {
            render();
            send_frame(sock);
            dirty = false;
        }
    }

    if (g_ai_pid > 0) kill(g_ai_pid, SIGTERM);
    if (g_ai_pipe >= 0) close(g_ai_pipe);
    ipc_send(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock);
    free(g_fb);
    free(g_font.data); free(g_font.cps); free(g_font.gis);
    for (int i = 0; i < g_nmsgs; i++) free(g_msgs[i].text);
    return 0;
}
