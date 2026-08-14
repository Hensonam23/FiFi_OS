/* fifi-calc — Calculator IPC app for FiFi OS.
 * Standard 4-function calculator with memory and percentage.
 * 280×380 window, mouse + keyboard input. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

/* ── IPC ─────────────────────────────────────────────────────────────────── */
#include "../../shared/app_ipc.h"

/* ── Window ──────────────────────────────────────────────────────────────── */
#define WIN_W   280
#define WIN_H   380
#define TITLE_H  24

/* ── Colours (shared FiFi design language) ───────────────────────────────── */
#define C_BG         0xFF0E1620u   /* window background */
#define C_CARD       0xFF16202Eu   /* panels / cards */
#define C_DISP_BG    0xFF0B1220u   /* display well (recessed) */
#define C_DISP_FG    0xFFD8E8F8u   /* primary text */
#define C_DISP_SM    0xFF6A8098u   /* muted / secondary */
#define C_BTN_NUM    0xFF16202Eu   /* number keys (card) */
#define C_BTN_OPS    0xFF1A2740u   /* operator keys (toolbar tone) */
#define C_BTN_EQ     0xFF409CFFu   /* equals — primary accent */
#define C_BTN_CLR    0xFF33202Cu   /* clear (muted warm) */
#define C_BTN_HOV    0xFF243448u   /* number hover */
#define C_BTN_OPS_H  0xFF2F6BBFu   /* operator hover (accent-dim) */
#define C_BTN_EQ_H   0xFF5AABFFu   /* equals hover */
#define C_BTN_CLR_H  0xFF5A2838u   /* clear hover */
#define C_BTN_FG     0xFFD8E8F8u   /* number label */
#define C_BTN_OPS_FG 0xFF409CFFu   /* operator label (accent) */
#define C_BTN_EQ_FG  0xFFFFFFFFu   /* on-accent */
#define C_BTN_CLR_FG 0xFFFF8899u   /* clear label */
#define C_BORDER     0xFF243448u   /* subtle divider */

/* ── PSF1 font ───────────────────────────────────────────────────────────── */
#define PSF1_MAGIC 0x0436u
typedef struct { uint16_t magic; uint8_t mode; uint8_t charsize; } Psf1Hdr;
static uint8_t *g_glyph = NULL;
static int g_glyph_h = 16;

static bool font_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    Psf1Hdr h;
    if (read(fd, &h, 4) != 4 || h.magic != PSF1_MAGIC) { close(fd); return false; }
    g_glyph_h = h.charsize;
    int sz = 256 * g_glyph_h;
    g_glyph = malloc(sz);
    if (!g_glyph) { close(fd); return false; }
    ssize_t got = read(fd, g_glyph, sz);
    close(fd);
    if (got < sz) { free(g_glyph); g_glyph = NULL; return false; }
    return true;
}

static void put_pixel(uint32_t *fb, int x, int y, uint32_t col) {
    if (x >= 0 && y >= 0 && x < WIN_W && y < WIN_H)
        fb[y * WIN_W + x] = col;
}

static void fill(uint32_t *fb, int x, int y, int w, int h, uint32_t col) {
    for (int row = y; row < y + h; row++)
        for (int col2 = x; col2 < x + w; col2++)
            put_pixel(fb, col2, row, col);
}

/* Filled rect with softened (notched) corners — reads as a rounded card. */
static void fill_round(uint32_t *fb, int x, int y, int w, int h,
                       uint32_t col, uint32_t bg) {
    fill(fb, x, y, w, h, col);
    const int r = 3;
    for (int i = 0; i < r; i++)
        for (int j = 0; j < r; j++)
            if (i + j < r) {
                put_pixel(fb, x + i,         y + j,         bg);
                put_pixel(fb, x + w - 1 - i, y + j,         bg);
                put_pixel(fb, x + i,         y + h - 1 - j, bg);
                put_pixel(fb, x + w - 1 - i, y + h - 1 - j, bg);
            }
}

/* Draw a character at 8×g_glyph_h using font */
static void draw_char(uint32_t *fb, int x, int y, unsigned char c, uint32_t fg) {
    if (!g_glyph) return;
    const uint8_t *bits = g_glyph + c * g_glyph_h;
    for (int row = 0; row < g_glyph_h; row++) {
        uint8_t b = bits[row];
        for (int col = 0; col < 8; col++) {
            if (b & (0x80u >> col))
                put_pixel(fb, x + col, y + row, fg);
        }
    }
}

static void draw_str(uint32_t *fb, const char *s, int x, int y, uint32_t fg) {
    for (; *s; s++, x += 9) draw_char(fb, x, y, (unsigned char)*s, fg);
}

/* Right-aligned string */
static void draw_str_r(uint32_t *fb, const char *s, int right_x, int y, uint32_t fg) {
    int len = (int)strlen(s);
    draw_str(fb, s, right_x - len * 9, y, fg);
}

/* ── Calculator state ────────────────────────────────────────────────────── */
#define DISP_MAX 20   /* max digits in display */

static double g_acc    = 0.0;   /* accumulator */
static double g_mem    = 0.0;   /* memory */
static char   g_op     = 0;     /* pending operator: + - * / */
static bool   g_new_num = true; /* next digit starts a new number */
static bool   g_error  = false;
static char   g_disp[DISP_MAX + 4] = "0";   /* display string */
static char   g_expr[DISP_MAX + 8] = "";    /* small "expr" line above */

static void disp_set(double v) {
    if (!isfinite(v)) { snprintf(g_disp, sizeof(g_disp), "Error"); g_error = true; return; }
    /* Use snprintf then trim trailing zeros after decimal point */
    snprintf(g_disp, sizeof(g_disp), "%.10g", v);
    /* Clamp to DISP_MAX chars */
    if ((int)strlen(g_disp) > DISP_MAX) {
        snprintf(g_disp, sizeof(g_disp), "%.6g", v);
    }
}

static double disp_val(void) { return strtod(g_disp, NULL); }

/* ── Button layout ───────────────────────────────────────────────────────── */
#define COLS   4
#define ROWS   5
#define BTN_W  (WIN_W / COLS)
#define BTN_H  38
#define BTN_Y0 (TITLE_H + 80)   /* top of button grid */

static const char *g_btn_labels[ROWS][COLS] = {
    { "C",   "+/-", "%",  "/" },
    { "7",   "8",   "9",  "*" },
    { "4",   "5",   "6",  "-" },
    { "1",   "2",   "3",  "+" },
    { "0",   ".",   "=",  "=" },  /* "0" spans 2, "=" spans 2 */
};
/* 1=number, 2=operator, 3=equals, 4=clear/special */
static const int g_btn_type[ROWS][COLS] = {
    { 4, 4, 4, 2 },
    { 1, 1, 1, 2 },
    { 1, 1, 1, 2 },
    { 1, 1, 1, 2 },
    { 1, 1, 3, 3 },
};

static int g_hov_r = -1, g_hov_c = -1;  /* hover button */

/* Which (r,c) is the mouse over? -1 if not in grid */
static void btn_hit(int mx, int my, int *r, int *c) {
    *r = *c = -1;
    if (mx < 0 || mx >= WIN_W) return;
    if (my < BTN_Y0 || my >= BTN_Y0 + ROWS * BTN_H) return;
    int row = (my - BTN_Y0) / BTN_H;
    if (row < 0 || row >= ROWS) return;
    /* last row: col 0 spans 2, col 2+3 = "=" */
    if (row == ROWS - 1) {
        if (mx < BTN_W * 2) { *r = row; *c = 0; }
        else                 { *r = row; *c = 2; }
        return;
    }
    int col = mx / BTN_W;
    if (col < 0 || col >= COLS) return;
    *r = row; *c = col;
}

/* ── Render ──────────────────────────────────────────────────────────────── */
#define GAP 4   /* gap between keys / around cards */

static void render(uint32_t *fb) {
    fill(fb, 0, 0, WIN_W, WIN_H, C_BG);

    /* ── Display area (recessed card) ── */
    int disp_y = TITLE_H + GAP + 2;
    int disp_h = BTN_Y0 - disp_y - GAP;
    fill_round(fb, GAP, disp_y, WIN_W - 2 * GAP, disp_h, C_DISP_BG, C_BG);

    /* expression hint (small, above main display) */
    if (g_expr[0]) {
        draw_str_r(fb, g_expr, WIN_W - GAP - 8, disp_y + 8, C_DISP_SM);
    }

    /* main display — right-aligned, vertically centred */
    int disp_val_y = disp_y + disp_h - g_glyph_h - 8;
    draw_str_r(fb, g_disp, WIN_W - GAP - 8, disp_val_y, g_error ? 0xFFFF6040u : C_DISP_FG);

    /* Memory indicator (accent pill top-left of display) */
    if (g_mem != 0.0) {
        fill_round(fb, GAP + 6, disp_y + 6, 16, g_glyph_h + 2, C_BTN_OPS, C_DISP_BG);
        draw_str(fb, "M", GAP + 10, disp_y + 7, C_BTN_OPS_FG);
    }

    /* ── Buttons ── */
    for (int row = 0; row < ROWS; row++) {
        for (int col = 0; col < COLS; col++) {
            /* last row: col 0 wide, col 3 skip */
            int bx, bw;
            if (row == ROWS - 1) {
                if (col == 0) { bx = 0; bw = BTN_W * 2; }
                else if (col == 1) continue;
                else if (col == 2) { bx = BTN_W * 2; bw = BTN_W * 2; }
                else continue;
            } else {
                bx = col * BTN_W;
                bw = BTN_W;
            }
            int by = BTN_Y0 + row * BTN_H;
            bool hov = (g_hov_r == row && g_hov_c == col);

            uint32_t bg, fg;
            int typ = g_btn_type[row][col];
            if (typ == 1)      { bg = hov ? C_BTN_HOV  : C_BTN_NUM; fg = C_BTN_FG; }
            else if (typ == 2) { bg = hov ? C_BTN_OPS_H: C_BTN_OPS; fg = hov ? C_BTN_EQ_FG : C_BTN_OPS_FG; }
            else if (typ == 3) { bg = hov ? C_BTN_EQ_H : C_BTN_EQ;  fg = C_BTN_EQ_FG; }
            else               { bg = hov ? C_BTN_CLR_H: C_BTN_CLR; fg = C_BTN_CLR_FG; }

            /* inset each key by GAP so the keys read as separated rounded tiles */
            int kx = bx + GAP, ky = by + GAP;
            int kw = bw - 2 * GAP, kh = BTN_H - 2 * GAP;
            fill_round(fb, kx, ky, kw, kh, bg, C_BG);

            const char *lbl = g_btn_labels[row][col];
            if (row == ROWS - 1 && col == 2) lbl = "=";
            int lx = bx + (bw - (int)strlen(lbl) * 9) / 2;
            int ly = by + (BTN_H - g_glyph_h) / 2;
            draw_str(fb, lbl, lx, ly, fg);
        }
    }
}

/* ── Calculator logic ────────────────────────────────────────────────────── */
static void apply_op(void) {
    if (!g_op) return;
    double b = disp_val();
    double result;
    switch (g_op) {
    case '+': result = g_acc + b; break;
    case '-': result = g_acc - b; break;
    case '*': result = g_acc * b; break;
    case '/':
        if (b == 0.0) { snprintf(g_disp, sizeof(g_disp), "Div/0"); g_error = true; g_op = 0; return; }
        result = g_acc / b;
        break;
    default: return;
    }
    g_acc = result;
    disp_set(result);
    g_op = 0;
}

static void calc_key(char key) {
    if (g_error && key != 'C') return;

    if (key >= '0' && key <= '9') {
        if (g_new_num) {
            snprintf(g_disp, sizeof(g_disp), "%c", key);
            g_new_num = false;
        } else {
            int cur_len = (int)strlen(g_disp);
            if (cur_len < DISP_MAX) {
                g_disp[cur_len] = key;
                g_disp[cur_len + 1] = '\0';
            }
        }
    } else if (key == '.') {
        if (g_new_num) { snprintf(g_disp, sizeof(g_disp), "0."); g_new_num = false; }
        else if (!strchr(g_disp, '.')) {
            int cur_len = (int)strlen(g_disp);
            if (cur_len < DISP_MAX - 1) {
                g_disp[cur_len] = '.';
                g_disp[cur_len + 1] = '\0';
            }
        }
    } else if (key == 'C') {
        snprintf(g_disp, sizeof(g_disp), "0");
        g_acc = 0.0; g_op = 0; g_new_num = true;
        g_expr[0] = '\0'; g_error = false;
    } else if (key == 'N') {  /* +/- negate */
        double v = disp_val();
        disp_set(-v);
    } else if (key == '%') {
        double v = disp_val();
        if (g_op && g_acc != 0.0) disp_set(g_acc * v / 100.0);
        else                       disp_set(v / 100.0);
        g_new_num = true;
    } else if (key == '+' || key == '-' || key == '*' || key == '/') {
        if (g_op && !g_new_num) apply_op();
        else                    g_acc = disp_val();
        g_op = key;
        snprintf(g_expr, sizeof(g_expr), "%.10g %c", g_acc, key);
        g_new_num = true;
    } else if (key == '=') {
        if (g_op) {
            apply_op();
            g_expr[0] = '\0';
            g_new_num = true;
        }
    } else if (key == 'B') {  /* backspace */
        int len = (int)strlen(g_disp);
        if (!g_new_num && len > 1) {
            g_disp[len - 1] = '\0';
        } else {
            snprintf(g_disp, sizeof(g_disp), "0");
            g_new_num = true;
        }
    }
}

static void btn_click(int row, int col) {
    if (row < 0 || col < 0) return;
    if (row == ROWS - 1 && col == 2) { calc_key('='); return; }
    const char *lbl = g_btn_labels[row][col];
    if (strcmp(lbl, "C")   == 0) { calc_key('C'); return; }
    if (strcmp(lbl, "+/-") == 0) { calc_key('N'); return; }
    if (strcmp(lbl, "%")   == 0) { calc_key('%'); return; }
    if (lbl[0] && lbl[1] == '\0') calc_key(lbl[0]);
}

/* ── IPC message reader ───────────────────────────────────────────────────── */
static void send_frame(int sock, uint32_t *fb) {
    (void)fifi_app_ipc_send_frame(sock, WIN_W, WIN_H, fb);
}

typedef struct {
    uint8_t  hdr[8];
    int      hdr_got;
    uint32_t type, plen, pgot;
    uint8_t *pld;
} MsgState;

static bool msg_feed(MsgState *m, const uint8_t *buf, int n, int *pos) {
    while (*pos < n) {
        if (m->hdr_got < 8) {
            m->hdr[m->hdr_got++] = buf[(*pos)++];
            if (m->hdr_got == 8) {
                memcpy(&m->type, m->hdr, 4);
                memcpy(&m->plen, m->hdr + 4, 4);
                if (m->plen > 4 * 1024 * 1024u) { m->hdr_got = 0; return false; }
                m->pgot = 0;
                free(m->pld); m->pld = NULL;
                if (m->plen > 0) { m->pld = malloc(m->plen); if (!m->pld) return false; }
                if (m->plen == 0) return true;
            }
        } else {
            uint32_t need = m->plen - m->pgot;
            uint32_t have = (uint32_t)(n - *pos);
            uint32_t take = need < have ? need : have;
            if (m->pld) memcpy(m->pld + m->pgot, buf + *pos, take);
            m->pgot += take; *pos += (int)take;
            if (m->pgot >= m->plen) return true;
        }
    }
    return false;
}

static void msg_reset(MsgState *m) {
    free(m->pld); m->pld = NULL;
    m->hdr_got = 0; m->plen = 0; m->pgot = 0;
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void) {
    if (!font_load("/fifi-data/fonts/ter16b.psf"))
        font_load("/fifi-data/fonts/default.psf");

    uint32_t *fb = calloc(WIN_W * WIN_H, 4);
    if (!fb) return 1;

    int sock = fifi_app_ipc_connect(WIN_W, WIN_H, "Calculator");
    if (sock < 0) { free(fb); return 1; }

    /* Read IPC_WIN_CREATED */
    {
        uint8_t rbuf[28] = {0}; int got = 0;
        while (got < 28) { ssize_t n = read(sock, rbuf + got, 28 - got); if (n <= 0) break; got += (int)n; }
    }

    signal(SIGPIPE, SIG_IGN);  /* don't crash if compositor closes the socket */
    render(fb);
    send_frame(sock, fb);

    MsgState ms = {0};
    bool running = true;
    bool dirty   = false;
    bool lbtn_prev = false;

    while (running) {
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        struct timeval tv = {0, 50000};
        int sel = select(sock + 1, &fds, NULL, NULL, &tv);
        if (sel < 0) break;

        if (sel > 0) {
            uint8_t tbuf[4096];
            ssize_t n = read(sock, tbuf, sizeof(tbuf));
            if (n <= 0) break;
            {
                int pos = 0;
                while (pos < (int)n) {
                    if (!msg_feed(&ms, tbuf, (int)n, &pos)) continue;

                    switch (ms.type) {
                    case IPC_INPUT_KEY:
                        if (ms.plen >= 1 && ms.pld) {
                            uint8_t key = ms.pld[0];
                            if (key == 0x1Bu || key == 'q' || key == 'Q') { running = false; break; }
                            if (key >= '0' && key <= '9') calc_key((char)key);
                            else if (key == '+' || key == '-' || key == '*' || key == '/') calc_key((char)key);
                            else if (key == '=' || key == '\r' || key == '\n') calc_key('=');
                            else if (key == '.' || key == ',') calc_key('.');
                            else if (key == 0x08u || key == 0x7Fu) calc_key('B');  /* backspace/del */
                            else if (key == '%') calc_key('%');
                            else if (key == 'c' || key == 'C') calc_key('C');
                            dirty = true;
                        }
                        break;
                    case IPC_INPUT_MOUSE:
                        if (ms.plen >= 9 && ms.pld) {
                            int32_t rx, ry;
                            memcpy(&rx, ms.pld, 4); memcpy(&ry, ms.pld + 4, 4);
                            uint8_t btns = ms.pld[8];
                            bool lbtn = !!(btns & 1);

                            int new_r, new_c;
                            btn_hit((int)rx, (int)ry, &new_r, &new_c);
                            if (new_r != g_hov_r || new_c != g_hov_c) {
                                g_hov_r = new_r; g_hov_c = new_c;
                                dirty = true;
                            }
                            /* click on button-up */
                            if (!lbtn && lbtn_prev && new_r >= 0) {
                                btn_click(new_r, new_c);
                                dirty = true;
                            }
                            lbtn_prev = lbtn;
                        }
                        break;
                    case IPC_INVALIDATE:
                        dirty = true;
                        break;
                    case IPC_APP_CLOSE:
                        running = false;
                        break;
                    }
                    msg_reset(&ms);
                }
            }
        }

        if (dirty) {
            render(fb);
            send_frame(sock, fb);
            dirty = false;
        }
    }

    (void)fifi_app_ipc_send(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock);
    free(fb);
    free(g_glyph);
    return 0;
}
