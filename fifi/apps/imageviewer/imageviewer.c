/* fifi-imageviewer — Image preview IPC app for FiFi OS linux-desktop.
 * Supports BMP (24-bit / 32-bit BI_RGB) and PPM P6 formats.
 * Zoom: scroll or +/-, F=fit, 0=100%, W=set as wallpaper, Esc/Q=close. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

/* ── IPC protocol ────────────────────────────────────────────────────────── */
#define FIFI_SOCK         "/tmp/fifi-compositor.sock"
#include "../../shared/ipc.h"

/* ── Window ──────────────────────────────────────────────────────────────── */
static int g_win_w = 720;
static int g_win_h = 520;
#define TITLE_H  24
#define INFO_H   22
#define PAD_X    10

/* ── Colors (shared FiFi design language) ────────────────────────────────── */
#define C_BG       0xFF0E1620u   /* window background */
#define C_CHECK_A  0xFF161E2Au   /* transparency checker (dark) */
#define C_CHECK_B  0xFF1E2836u   /* transparency checker (light) */
#define C_INFO_BG  0xFF1A2740u   /* info/toolbar bar */
#define C_INFO_FG  0xFFD8E8F8u   /* primary text */
#define C_INFO_SM  0xFF6A8098u   /* muted / hints */
#define C_ACCENT   0xFF409CFFu   /* accent */
#define C_ONACCENT 0xFFFFFFFFu   /* on-accent text */
#define C_BORDER   0xFF243448u   /* subtle divider */
#define C_HINT_FG  0xFF6A8098u
#define C_ERR_BG   0xFF1A1018u
#define C_ERR_FG   0xFFFF6A50u

/* ── Image state ─────────────────────────────────────────────────────────── */
static uint32_t *g_img   = NULL;
static int       g_img_w = 0;
static int       g_img_h = 0;
static char      g_img_fmt[16]  = "";
static char      g_img_err[128] = "";
static char      g_img_path[512] = "";
static char      g_img_name[64]  = "";

/* View state */
static int  g_zoom_pct = 100;
static int  g_pan_x    = 0;
static int  g_pan_y    = 0;
static bool g_fit_mode = true;

/* Mouse drag state */
static bool g_drag    = false;
static int  g_drag_sx = 0, g_drag_sy = 0;
static int  g_drag_px = 0, g_drag_py = 0;
static bool g_lbtn    = false;

/* ── PSF1 font ───────────────────────────────────────────────────────────── */
#define PSF1_MAGIC 0x0436u
typedef struct { uint16_t magic; uint8_t mode; uint8_t charsize; } Psf1Hdr;
static uint8_t *g_glyph   = NULL;
static int      g_glyph_h = 16;
#define GLYPH_W 8

static bool font_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    Psf1Hdr h;
    if (read(fd, &h, 4) != 4 || h.magic != PSF1_MAGIC) { close(fd); return false; }
    g_glyph_h = h.charsize;
    int sz = 256 * g_glyph_h;
    g_glyph = malloc((size_t)sz);
    if (!g_glyph) { close(fd); return false; }
    if (read(fd, g_glyph, sz) < sz) { free(g_glyph); g_glyph = NULL; close(fd); return false; }
    close(fd);
    return true;
}

/* ── Drawing helpers ─────────────────────────────────────────────────────── */
static void put_pixel(uint32_t *fb, int x, int y, uint32_t col) {
    if (x >= 0 && y >= 0 && x < g_win_w && y < g_win_h)
        fb[y * g_win_w + x] = col;
}

static void fill_rect(uint32_t *fb, int x, int y, int w, int h, uint32_t col) {
    for (int r = y; r < y + h; r++)
        for (int c = x; c < x + w; c++)
            put_pixel(fb, c, r, col);
}

/* Filled rect with softened (notched) corners — reads as a rounded pill. */
static void fill_round(uint32_t *fb, int x, int y, int w, int h,
                       uint32_t col, uint32_t bg) {
    fill_rect(fb, x, y, w, h, col);
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

static void draw_char(uint32_t *fb, int x, int y, unsigned char ch, uint32_t fg, uint32_t bg) {
    if (!g_glyph) return;
    const uint8_t *bits = g_glyph + (int)ch * g_glyph_h;
    for (int row = 0; row < g_glyph_h; row++) {
        uint8_t b = bits[row];
        for (int col = 0; col < GLYPH_W; col++)
            put_pixel(fb, x + col, y + row, (b & (0x80u >> col)) ? fg : bg);
    }
}

static void draw_str(uint32_t *fb, int x, int y, const char *s,
                     uint32_t fg, uint32_t bg) {
    for (; *s; s++, x += GLYPH_W + 1)
        draw_char(fb, x, y, (unsigned char)*s, fg, bg);
}

static int str_px(const char *s) { return (int)strlen(s) * (GLYPH_W + 1); }

/* ── BMP decoder ─────────────────────────────────────────────────────────── */
static uint32_t *decode_bmp(const uint8_t *d, size_t sz, int *ow, int *oh) {
    if (sz < 54) return NULL;
    if (d[0] != 'B' || d[1] != 'M') return NULL;

    uint32_t data_off; memcpy(&data_off, d + 10, 4);
    int32_t  width;    memcpy(&width,    d + 18, 4);
    int32_t  height;   memcpy(&height,   d + 22, 4);
    uint16_t bpp;      memcpy(&bpp,      d + 28, 2);
    uint32_t compr;    memcpy(&compr,    d + 30, 4);

    if (width <= 0 || width > 16384) return NULL;
    int h = height < 0 ? -height : height;
    if (h <= 0 || h > 16384) return NULL;
    if (bpp != 24 && bpp != 32) return NULL;
    if (compr != 0 && compr != 3) return NULL;

    int stride = (width * (bpp / 8) + 3) & ~3;
    if ((size_t)data_off + (size_t)stride * (size_t)h > sz) return NULL;

    uint32_t *px = malloc((size_t)width * (size_t)h * 4);
    if (!px) return NULL;

    int Bpp = bpp / 8;
    for (int y = 0; y < h; y++) {
        int src_row = (height > 0) ? (h - 1 - y) : y;
        const uint8_t *row = d + data_off + (size_t)src_row * (size_t)stride;
        uint32_t *dst = px + y * width;
        for (int x = 0; x < width; x++) {
            uint8_t b  = row[x * Bpp + 0];
            uint8_t g2 = row[x * Bpp + 1];
            uint8_t r  = row[x * Bpp + 2];
            dst[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g2 << 8) | b;
        }
    }
    *ow = width; *oh = h;
    return px;
}

/* ── PPM P6 decoder ──────────────────────────────────────────────────────── */
static int ppm_read_int(const uint8_t *d, size_t sz, size_t *p) {
    for (;;) {
        while (*p < sz && (d[*p]==' '||d[*p]=='\t'||d[*p]=='\r'||d[*p]=='\n')) (*p)++;
        if (*p >= sz) return -1;
        if (d[*p] != '#') break;
        while (*p < sz && d[*p] != '\n') (*p)++;
    }
    int v = 0;
    while (*p < sz && d[*p] >= '0' && d[*p] <= '9') {
        if (v > 214748363) return -1;   /* would overflow int */
        v = v * 10 + (int)(d[(*p)++] - '0');
    }
    return v;
}

static uint32_t *decode_ppm(const uint8_t *d, size_t sz, int *ow, int *oh) {
    if (sz < 7 || d[0] != 'P' || d[1] != '6') return NULL;
    size_t pos = 2;
    int w = ppm_read_int(d, sz, &pos);
    int h = ppm_read_int(d, sz, &pos);
    int mv = ppm_read_int(d, sz, &pos);
    if (w <= 0 || h <= 0 || mv != 255) return NULL;
    if (w > 16384 || h > 16384) return NULL;
    if (pos < sz) pos++; /* single whitespace after maxval */
    if (pos + (size_t)w * h * 3 > sz) return NULL;

    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) return NULL;
    const uint8_t *rgb = d + pos;
    for (int i = 0; i < w * h; i++) {
        uint8_t r = rgb[i*3+0], g2 = rgb[i*3+1], b = rgb[i*3+2];
        px[i] = 0xFF000000u | ((uint32_t)r<<16) | ((uint32_t)g2<<8) | b;
    }
    *ow = w; *oh = h;
    return px;
}

/* ── Image file loader ───────────────────────────────────────────────────── */
static void load_image(const char *path) {
    free(g_img); g_img = NULL; g_img_w = g_img_h = 0;
    g_img_err[0] = '\0'; g_img_fmt[0] = '\0';

    strncpy(g_img_path, path, sizeof(g_img_path) - 1);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strncpy(g_img_name, base, sizeof(g_img_name) - 1);

    /* Detect format by extension */
    const char *ext = strrchr(base, '.');
    bool is_bmp = ext && (strcasecmp(ext, ".bmp") == 0);
    bool is_ppm = ext && (strcasecmp(ext, ".ppm") == 0 || strcasecmp(ext, ".pgm") == 0);
    bool is_png = ext && (strcasecmp(ext, ".png") == 0);
    bool is_jpg = ext && (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0);

    if (is_png || is_jpg) {
        snprintf(g_img_err, sizeof(g_img_err),
                 "%s: no PNG/JPEG decoder. Convert to BMP or PPM.",
                 is_png ? "PNG" : "JPEG");
        return;
    }
    if (!is_bmp && !is_ppm) {
        snprintf(g_img_err, sizeof(g_img_err), "Unsupported format: %s",
                 ext ? ext : "(unknown)");
        return;
    }

    /* Read file into memory */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(g_img_err, sizeof(g_img_err), "Cannot open: %s", strerror(errno));
        return;
    }
    struct stat st;
    fstat(fd, &st);
    if (st.st_size <= 0 || st.st_size > 128 * 1024 * 1024) {
        close(fd);
        snprintf(g_img_err, sizeof(g_img_err), "File too large (>128MB)");
        return;
    }
    size_t fsz = (size_t)st.st_size;
    uint8_t *buf = malloc(fsz);
    if (!buf) { close(fd); snprintf(g_img_err, sizeof(g_img_err), "Out of memory"); return; }
    size_t got = 0;
    while (got < fsz) {
        ssize_t n = read(fd, buf + got, fsz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);

    if (is_bmp) {
        g_img = decode_bmp(buf, got, &g_img_w, &g_img_h);
        strncpy(g_img_fmt, "BMP", sizeof(g_img_fmt) - 1);
    } else {
        g_img = decode_ppm(buf, got, &g_img_w, &g_img_h);
        strncpy(g_img_fmt, "PPM", sizeof(g_img_fmt) - 1);
    }
    free(buf);

    if (!g_img)
        snprintf(g_img_err, sizeof(g_img_err), "Decode failed (unsupported variant)");

    g_fit_mode = true;
    g_pan_x = g_pan_y = 0;
    g_zoom_pct = 100;
}

/* ── Render ──────────────────────────────────────────────────────────────── */
static void render(uint32_t *fb) {
    int content_y = TITLE_H;
    int content_h = g_win_h - TITLE_H - INFO_H;
    if (content_h < 1) content_h = 1;

    /* Info bar background + top divider */
    fill_rect(fb, 0, g_win_h - INFO_H, g_win_w, INFO_H, C_INFO_BG);
    fill_rect(fb, 0, g_win_h - INFO_H, g_win_w, 1, C_BORDER);

    /* Error state */
    if (g_img_err[0]) {
        fill_rect(fb, 0, content_y, g_win_w, content_h, C_ERR_BG);
        int tx = (g_win_w - str_px(g_img_err)) / 2;
        int ty = content_y + (content_h - g_glyph_h) / 2;
        draw_str(fb, tx, ty, g_img_err, C_ERR_FG, C_ERR_BG);

        const char *hint = "BMP and PPM P6 formats supported";
        int hx = (g_win_w - str_px(hint)) / 2;
        draw_str(fb, hx, ty + g_glyph_h + 4, hint, C_HINT_FG, C_ERR_BG);

        /* Info bar */
        draw_str(fb, PAD_X, g_win_h - INFO_H + (INFO_H - g_glyph_h) / 2,
                 g_img_name, C_INFO_FG, C_INFO_BG);
        return;
    }

    if (!g_img) {
        fill_rect(fb, 0, content_y, g_win_w, content_h, C_BG);
        const char *msg = "No image loaded";
        draw_str(fb, (g_win_w - str_px(msg)) / 2,
                 content_y + (content_h - g_glyph_h) / 2,
                 msg, C_INFO_FG, C_BG);
        return;
    }

    /* Compute effective zoom */
    int disp_w, disp_h;
    if (g_fit_mode) {
        /* Scale to fit content area while preserving aspect ratio */
        int fw = g_win_w;
        int fh = content_h;
        if (g_img_w * fh > fw * g_img_h) {
            disp_w = fw;
            disp_h = fw * g_img_h / g_img_w;
        } else {
            disp_h = fh;
            disp_w = fh * g_img_w / g_img_h;
        }
        if (disp_w < 1) disp_w = 1;
        if (disp_h < 1) disp_h = 1;
    } else {
        disp_w = g_img_w * g_zoom_pct / 100;
        disp_h = g_img_h * g_zoom_pct / 100;
        if (disp_w < 1) disp_w = 1;
        if (disp_h < 1) disp_h = 1;
    }

    /* Top-left of image in window coords (centered + pan) */
    int img_x = (g_win_w - disp_w) / 2 + (g_fit_mode ? 0 : g_pan_x);
    int img_y = content_y + (content_h - disp_h) / 2 + (g_fit_mode ? 0 : g_pan_y);

    /* Fill content area with checkerboard background */
    for (int y = content_y; y < content_y + content_h; y++) {
        for (int x = 0; x < g_win_w; x++) {
            uint32_t col = (((x >> 3) ^ (y >> 3)) & 1) ? C_CHECK_A : C_CHECK_B;
            put_pixel(fb, x, y, col);
        }
    }

    /* Blit image with nearest-neighbor scaling */
    int clip_x0 = img_x > 0 ? img_x : 0;
    int clip_y0 = img_y > content_y ? img_y : content_y;
    int clip_x1 = img_x + disp_w < g_win_w ? img_x + disp_w : g_win_w;
    int clip_y1 = img_y + disp_h < content_y + content_h ?
                  img_y + disp_h : content_y + content_h;

    for (int py = clip_y0; py < clip_y1; py++) {
        int sy = (py - img_y) * g_img_h / disp_h;
        if (sy < 0 || sy >= g_img_h) continue;
        const uint32_t *src_row = g_img + sy * g_img_w;
        uint32_t *dst = fb + py * g_win_w + clip_x0;
        for (int px2 = clip_x0; px2 < clip_x1; px2++) {
            int sx = (px2 - img_x) * g_img_w / disp_w;
            if (sx >= 0 && sx < g_img_w) *dst = src_row[sx];
            dst++;
        }
    }

    /* ── Info bar content ── */
    int bar_y = g_win_h - INFO_H;
    int ty    = bar_y + (INFO_H - g_glyph_h) / 2;
    int x     = PAD_X;

    /* Format badge (accent pill) */
    if (g_img_fmt[0]) {
        int bw = str_px(g_img_fmt) + 10;
        fill_round(fb, x, bar_y + 3, bw, INFO_H - 6, C_ACCENT, C_INFO_BG);
        draw_str(fb, x + 5, ty, g_img_fmt, C_ONACCENT, C_ACCENT);
        x += bw + 10;
    }

    /* Dimensions (primary text) */
    char dims[48];
    snprintf(dims, sizeof(dims), "%dx%d", g_img_w, g_img_h);
    draw_str(fb, x, ty, dims, C_INFO_FG, C_INFO_BG);
    x += str_px(dims) + 12;

    /* Zoom level (accent) */
    char zoom[24];
    if (g_fit_mode) snprintf(zoom, sizeof(zoom), "fit");
    else            snprintf(zoom, sizeof(zoom), "%d%%", g_zoom_pct);
    draw_str(fb, x, ty, zoom, C_ACCENT, C_INFO_BG);

    /* Hints (muted, right-aligned) */
    const char *hint = "W wallpaper   F fit   +/- zoom";
    int hx = g_win_w - PAD_X - str_px(hint);
    if (hx > x + 12)
        draw_str(fb, hx, ty, hint, C_INFO_SM, C_INFO_BG);
}

/* ── IPC helpers ─────────────────────────────────────────────────────────── */
static void ipc_send_msg(int fd, uint32_t type, const void *data, uint32_t len) {
    uint8_t hdr[8];
    memcpy(hdr, &type, 4); memcpy(hdr + 4, &len, 4);
    write(fd, hdr, 8);
    if (len && data) write(fd, data, len);
}

static void send_frame(int sock, uint32_t *fb) {
    uint32_t hdr[4] = {0, 0, (uint32_t)g_win_w, (uint32_t)g_win_h};
    /* Compute in size_t: 16 + w*h*4 overflows uint32 for large windows */
    size_t px     = (size_t)g_win_w * (size_t)g_win_h * 4;
    size_t pld_sz = 16 + px;
    if (pld_sz > 0xFFFFFFFFu) return;   /* len field is 32-bit */
    uint8_t *msg = malloc(pld_sz);
    if (!msg) return;
    memcpy(msg, hdr, 16);
    memcpy(msg + 16, fb, px);
    ipc_send_msg(sock, IPC_APP_FRAME, msg, (uint32_t)pld_sz);
    free(msg);
}

static void send_title(int sock) {
    char title[128];
    if (g_img_err[0]) {
        snprintf(title, sizeof(title), "Image Viewer — %s (error)", g_img_name);
    } else if (g_img) {
        if (g_fit_mode)
            snprintf(title, sizeof(title), "Image Viewer — %s (fit)", g_img_name);
        else
            snprintf(title, sizeof(title), "Image Viewer — %s (%d%%)", g_img_name, g_zoom_pct);
    } else {
        snprintf(title, sizeof(title), "Image Viewer");
    }
    ipc_send_msg(sock, IPC_APP_TITLE, title, (uint32_t)strlen(title));
}

/* ── IPC message reader ──────────────────────────────────────────────────── */
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
                if (m->plen > 128 * 1024 * 1024u) { m->hdr_got = 0; return false; }
                m->pgot = 0;
                free(m->pld); m->pld = NULL;
                /* On malloc failure pld stays NULL: payload is skipped in sync */
                if (m->plen > 0) m->pld = malloc(m->plen);
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
int main(int argc, char **argv) {
    if (!font_load("/fifi-data/fonts/ter16b.psf"))
        font_load("/fifi-data/fonts/default.psf");

    const char *path = (argc > 1) ? argv[1] : NULL;
    if (path) load_image(path);

    uint32_t *fb = calloc((size_t)g_win_w * g_win_h, 4);
    if (!fb) return 1;

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { free(fb); return 1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIFI_SOCK, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock); free(fb); return 1;
    }

    /* IPC_APP_CONNECT */
    uint8_t conn[64] = {0};
    uint16_t cw = (uint16_t)g_win_w, ch = (uint16_t)g_win_h;
    memcpy(conn, &cw, 2); memcpy(conn + 2, &ch, 2);
    snprintf((char *)(conn + 4), 60, "Image Viewer");
    ipc_send_msg(sock, IPC_APP_CONNECT, conn, sizeof(conn));

    /* Read IPC_WIN_CREATED */
    {
        uint8_t rbuf[28] = {0}; int got = 0;
        while (got < 28) {
            ssize_t n = read(sock, rbuf + got, (size_t)(28 - got));
            if (n <= 0) break;
            got += (int)n;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    render(fb);
    send_frame(sock, fb);
    send_title(sock);

    MsgState ms = {0};
    bool running = true;
    bool dirty   = false;
    bool title_dirty = false;

    while (running) {
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        struct timeval tv = {0, 50000};
        int sel = select(sock + 1, &fds, NULL, NULL, &tv);
        if (sel < 0) break;

        if (sel > 0) {
            uint8_t tbuf[8192];
            ssize_t n = read(sock, tbuf, sizeof(tbuf));
            if (n <= 0) break;
            int pos = 0;
            while (pos < (int)n) {
                /* false = message incomplete; keep state and wait for more data
                 * (resetting here would discard partially-received messages) */
                if (!msg_feed(&ms, tbuf, (int)n, &pos)) break;

                switch (ms.type) {

                case IPC_INPUT_KEY:
                    if (ms.plen >= 1 && ms.pld) {
                        uint8_t key = ms.pld[0];

                        if (key == 0x1Bu || key == 'q' || key == 'Q') {
                            running = false; break;
                        }
                        if (key == 'f' || key == 'F') {
                            g_fit_mode = true; g_pan_x = g_pan_y = 0;
                            dirty = true; title_dirty = true;
                        } else if (key == '0') {
                            g_fit_mode = false; g_zoom_pct = 100;
                            g_pan_x = g_pan_y = 0;
                            dirty = true; title_dirty = true;
                        } else if (key == '+' || key == '=') {
                            g_fit_mode = false;
                            g_zoom_pct += 10;
                            if (g_zoom_pct > 800) g_zoom_pct = 800;
                            dirty = true; title_dirty = true;
                        } else if (key == '-') {
                            g_fit_mode = false;
                            g_zoom_pct -= 10;
                            if (g_zoom_pct < 10) g_zoom_pct = 10;
                            dirty = true; title_dirty = true;
                        } else if (key == 'w' || key == 'W') {
                            /* Set as wallpaper */
                            if (g_img_path[0])
                                ipc_send_msg(sock, IPC_SET_WALLPAPER,
                                             g_img_path, (uint32_t)strlen(g_img_path));
                        } else if (key == 0x52 || key == 0x51 ||  /* arrow keys (up/down) */
                                   key == 0x50 || key == 0x4F) {   /* left/right */
                            if (!g_fit_mode) {
                                int step = 20;
                                if      (key == 0x52) g_pan_y += step; /* down */
                                else if (key == 0x51) g_pan_y -= step; /* up */
                                else if (key == 0x50) g_pan_x -= step; /* left */
                                else                  g_pan_x += step; /* right */
                                dirty = true;
                            }
                        }
                    }
                    break;

                case IPC_INPUT_MOUSE:
                    if (ms.plen >= 10 && ms.pld) {
                        int32_t rx, ry;
                        memcpy(&rx, ms.pld, 4);
                        memcpy(&ry, ms.pld + 4, 4);
                        uint8_t btns   = ms.pld[8];
                        int8_t  scroll = (int8_t)ms.pld[9];
                        bool    lbtn   = !!(btns & 1);

                        /* Scroll wheel zoom */
                        if (scroll != 0 && g_img) {
                            g_fit_mode = false;
                            g_zoom_pct += scroll * 10;
                            if (g_zoom_pct < 10)  g_zoom_pct = 10;
                            if (g_zoom_pct > 800) g_zoom_pct = 800;
                            dirty = true; title_dirty = true;
                        }

                        /* Mouse drag to pan */
                        if (lbtn && !g_lbtn) {
                            /* Button down — start drag */
                            g_drag = true;
                            g_drag_sx = rx; g_drag_sy = ry;
                            g_drag_px = g_pan_x; g_drag_py = g_pan_y;
                        } else if (!lbtn && g_lbtn) {
                            /* Button up — end drag */
                            g_drag = false;
                        }
                        if (g_drag && lbtn && g_img && !g_fit_mode) {
                            g_pan_x = g_drag_px + (rx - g_drag_sx);
                            g_pan_y = g_drag_py + (ry - g_drag_sy);
                            dirty = true;
                        }
                        g_lbtn = lbtn;
                    }
                    break;

                case IPC_WIN_RESIZE:
                    if (ms.plen >= 4 && ms.pld) {
                        uint16_t nw, nh;
                        memcpy(&nw, ms.pld, 2);
                        memcpy(&nh, ms.pld + 2, 2);
                        if (nw >= 200 && nh >= 150 && nw <= 8192 && nh <= 8192 &&
                            ((int)nw != g_win_w || (int)nh != g_win_h)) {
                            free(fb);
                            g_win_w = (int)nw; g_win_h = (int)nh;
                            fb = calloc((size_t)g_win_w * g_win_h, 4);
                            if (!fb) { running = false; break; }
                            dirty = true;
                        }
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

        if (dirty && fb) {
            render(fb);
            send_frame(sock, fb);
            dirty = false;
        }
        if (title_dirty) {
            send_title(sock);
            title_dirty = false;
        }
    }

    ipc_send_msg(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock);
    free(fb);
    free(g_img);
    free(g_glyph);
    return 0;
}
