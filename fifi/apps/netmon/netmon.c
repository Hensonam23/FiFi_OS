/* fifi-netmon — Network Monitor IPC app for FiFi OS.
 * Shows interface stats, IP addresses, RX/TX rates.
 * 480×340 window, updates every second. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

/* ── IPC protocol ────────────────────────────────────────────────────────── */
#define FIFI_SOCK        "/tmp/fifi-compositor.sock"
#include "../../shared/ipc.h"

#define WIN_W   480
#define WIN_H   340
#define TITLE_H 24

/* ── PSF1 font ───────────────────────────────────────────────────────────── */
#define PSF1_MAGIC 0x0436
typedef struct { uint16_t magic; uint8_t mode; uint8_t charsize; } Psf1Hdr;
static uint8_t *g_glyph = NULL;
static uint32_t g_fw = 8, g_fh = 16;

static bool font_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    Psf1Hdr h;
    if (read(fd, &h, sizeof(h)) < (ssize_t)sizeof(h) || h.magic != PSF1_MAGIC) {
        close(fd); return false;
    }
    g_fh = h.charsize; g_fw = 8;
    size_t sz = 256 * g_fh;
    g_glyph = malloc(sz);
    if (!g_glyph) { close(fd); return false; }
    ssize_t got = read(fd, g_glyph, sz);
    close(fd);
    if (got < (ssize_t)sz) { free(g_glyph); g_glyph = NULL; return false; }
    return true;
}

static void draw_char(uint32_t *fb, int win_w, int win_h, int cx, int cy,
                      unsigned char c, uint32_t fg, uint32_t bg) {
    if (!g_glyph) return;
    uint8_t *row = g_glyph + (unsigned)c * g_fh;
    for (uint32_t y = 0; y < g_fh; y++) {
        for (uint32_t x = 0; x < g_fw; x++) {
            int px = cx + (int)x, py = cy + (int)y;
            if (px < 0 || py < 0 || px >= win_w || py >= win_h) continue;
            fb[py * win_w + px] = (row[y] & (0x80u >> x)) ? fg : bg;
        }
    }
}

static void draw_str(uint32_t *fb, int win_w, int win_h, int x, int y,
                     const char *s, uint32_t fg, uint32_t bg) {
    for (int i = 0; s[i]; i++)
        draw_char(fb, win_w, win_h, x + i * (int)g_fw, y, (unsigned char)s[i], fg, bg);
}

static void fill_rect(uint32_t *fb, int win_w, int win_h,
                      int x, int y, int w, int h, uint32_t col) {
    for (int row = 0; row < h; row++)
        for (int c = 0; c < w; c++) {
            int px = x + c, py = y + row;
            if (px >= 0 && py >= 0 && px < win_w && py < win_h)
                fb[py * win_w + px] = col;
        }
}

/* Filled rectangle with softened (rounded) corners — radius r. */
static void fill_round(uint32_t *fb, int win_w, int win_h,
                       int x, int y, int w, int h, uint32_t col, int r) {
    if (r < 1) { fill_rect(fb, win_w, win_h, x, y, w, h, col); return; }
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;
    for (int row = 0; row < h; row++) {
        int dy = (row < r) ? (r - 1 - row)
               : (row >= h - r) ? (row - (h - r)) : -1;
        int inset = 0;
        if (dy >= 0) {
            while (inset < r) {
                int dx = r - 1 - inset;
                if (dx*dx + dy*dy <= (r-1)*(r-1)) break;
                inset++;
            }
        }
        fill_rect(fb, win_w, win_h, x + inset, y + row, w - 2*inset, 1, col);
    }
}

/* Card panel (rounded). */
static void draw_card(uint32_t *fb, int win_w, int win_h,
                      int x, int y, int w, int h) {
    fill_round(fb, win_w, win_h, x, y, w, h, 0xFF16202Eu, 4);
}

/* ── IPC helpers ─────────────────────────────────────────────────────────── */
static void write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    while (n > 0) { ssize_t w = write(fd, p, n); if (w <= 0) break; p += w; n -= (size_t)w; }
}

static void ipc_send_msg(int fd, uint32_t type, const void *data, uint32_t len) {
    uint8_t hdr[8];
    memcpy(hdr, &type, 4); memcpy(hdr + 4, &len, 4);
    write_all(fd, hdr, 8);
    if (len && data) write_all(fd, data, len);
}

static int g_win_w = WIN_W, g_win_h = WIN_H;

static void send_frame(int sock, uint32_t *fb) {
    uint32_t w = (uint32_t)g_win_w, h = (uint32_t)g_win_h;
    size_t pix = (size_t)w * h * 4;   /* size_t: avoid 32-bit overflow */
    size_t pld_sz = 16 + pix;
    if (pld_sz > 0xFFFFFFFFu) return;
    uint8_t *msg = malloc(pld_sz);
    if (!msg) return;
    uint32_t hdr[4] = {0, 0, w, h};
    memcpy(msg, hdr, 16);
    memcpy(msg + 16, fb, pix);
    ipc_send_msg(sock, IPC_APP_FRAME, msg, (uint32_t)pld_sz);
    free(msg);
}

/* ── Network stats ───────────────────────────────────────────────────────── */
#define MAX_IFACES 8
typedef struct {
    char     name[16];
    uint64_t rx_bytes, tx_bytes;
    uint64_t rx_rate,  tx_rate;  /* bytes/sec */
    char     ip4[20];
    bool     up;
} iface_t;

static iface_t g_ifaces[MAX_IFACES];
static int     g_nifaces = 0;

static void update_ip(iface_t *ifc) {
    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk < 0) return;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifc->name, IFNAMSIZ - 1);
    if (ioctl(sk, SIOCGIFADDR, &ifr) == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        inet_ntop(AF_INET, &sin->sin_addr, ifc->ip4, sizeof(ifc->ip4));
    } else {
        strncpy(ifc->ip4, "no IP", sizeof(ifc->ip4));
    }
    if (ioctl(sk, SIOCGIFFLAGS, &ifr) == 0)
        ifc->up = !!(ifr.ifr_flags & IFF_UP) && !!(ifr.ifr_flags & IFF_RUNNING);
    close(sk);
}

static void update_stats(void) {
    int fd = open("/proc/net/dev", O_RDONLY);
    if (fd < 0) return;
    char buf[4096] = {0};
    read(fd, buf, sizeof(buf) - 1);
    close(fd);

    /* skip 2 header lines */
    char *line = buf;
    int skip = 2;
    while (skip-- > 0) { line = strchr(line, '\n'); if (!line) return; line++; }

    /* Process each interface line */
    iface_t new_ifaces[MAX_IFACES];
    int new_nifaces = 0;

    while (*line && new_nifaces < MAX_IFACES) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char name[16] = {0};
        unsigned long long rx=0, tx=0, tmp=0;
        int n = sscanf(line, " %15[^:]: %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                       name, &rx, &tmp, &tmp, &tmp, &tmp, &tmp, &tmp, &tmp, &tx);
        /* Skip loopback and virtual tunnel interfaces; only show real Ethernet (type=1) */
        bool is_real = false;
        if (n >= 10 && strcmp(name, "lo") != 0) {
            char type_path[64];
            snprintf(type_path, sizeof(type_path), "/sys/class/net/%s/type", name);
            int tfd = open(type_path, O_RDONLY);
            if (tfd >= 0) {
                char tbuf[8] = {0}; read(tfd, tbuf, sizeof(tbuf) - 1); close(tfd);
                is_real = (tbuf[0] == '1' && (tbuf[1] == '\n' || tbuf[1] == '\0'));
            }
        }
        if (is_real) {
            iface_t *ifc = &new_ifaces[new_nifaces++];
            memset(ifc, 0, sizeof(*ifc));
            memcpy(ifc->name, name, sizeof(ifc->name));
            ifc->rx_bytes = rx;
            ifc->tx_bytes = tx;
            /* carry over rates from previous measurement */
            for (int i = 0; i < g_nifaces; i++) {
                if (strcmp(g_ifaces[i].name, name) == 0) {
                    ifc->rx_rate = rx > g_ifaces[i].rx_bytes ? rx - g_ifaces[i].rx_bytes : 0;
                    ifc->tx_rate = tx > g_ifaces[i].tx_bytes ? tx - g_ifaces[i].tx_bytes : 0;
                    break;
                }
            }
            update_ip(ifc);
        }
        if (!nl) break;
        line = nl + 1;
    }
    memcpy(g_ifaces, new_ifaces, new_nifaces * sizeof(iface_t));
    g_nifaces = new_nifaces;
}

/* Format bytes/sec as human-readable */
static void fmt_rate(uint64_t bps, char *buf, int bufsz) {
    if (bps >= 1024*1024)
        snprintf(buf, bufsz, "%4.1f MB/s", bps / (1024.0 * 1024.0));
    else if (bps >= 1024)
        snprintf(buf, bufsz, "%4.1f KB/s", bps / 1024.0);
    else
        snprintf(buf, bufsz, "%4llu  B/s", (unsigned long long)bps);
}

static void fmt_bytes(uint64_t b, char *buf, int bufsz) {
    if (b >= 1024ULL*1024*1024)
        snprintf(buf, bufsz, "%6.2f GB", b / (1024.0*1024.0*1024.0));
    else if (b >= 1024*1024)
        snprintf(buf, bufsz, "%6.2f MB", b / (1024.0*1024.0));
    else if (b >= 1024)
        snprintf(buf, bufsz, "%6.2f KB", b / 1024.0);
    else
        snprintf(buf, bufsz, "%6llu  B", (unsigned long long)b);
}

/* ── Render (shared FiFi design language) ────────────────────────────────── */
#define C_BG      0xFF0E1620u   /* window background */
#define C_CARD    0xFF16202Eu   /* interface card    */
#define C_HEADER  0xFF1A2740u   /* header / footer   */
#define C_SEP     0xFF243448u   /* subtle divider    */
#define C_ACCENT  0xFF409CFFu   /* primary / RX meter*/
#define C_ACCENT2 0xFF2F6BBFu   /* dim accent / TX   */
#define C_FG      0xFFD8E8F8u   /* primary text      */
#define C_MUTED   0xFF6A8098u   /* secondary text    */
#define C_TRACK   0xFF0C141Eu   /* meter track       */
#define C_UP      0xFF40CC80u   /* link up           */
#define C_DOWN    0xFFE0A030u   /* link down (warn)  */

/* Small throughput meter: track + accent fill for `frac` (0..1). */
static void meter(uint32_t *fb, int ww, int wh, int x, int y, int w, int h,
                  double frac, uint32_t col) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int r = h >= 6 ? 2 : 1;
    fill_round(fb, ww, wh, x, y, w, h, C_TRACK, r);
    int fw = (int)(w * frac);
    if (fw < r*2 && fw > 0) fw = r*2;
    if (fw > w) fw = w;
    if (fw > 0) fill_round(fb, ww, wh, x, y, fw, h, col, r);
}

static void render(uint32_t *fb) {
    int ww = g_win_w, wh = g_win_h;
    fill_rect(fb, ww, wh, 0, 0, ww, wh, C_BG);
    int fh = (int)g_fh, fw = (int)g_fw;

    /* Header bar */
    int hdr_h = fh + 12;
    fill_rect(fb, ww, wh, 0, TITLE_H, ww, hdr_h, C_HEADER);
    fill_rect(fb, ww, wh, 0, TITLE_H + hdr_h, ww, 1, C_SEP);
    fill_rect(fb, ww, wh, 12, TITLE_H + 6, 3, fh, C_ACCENT);
    draw_str(fb, ww, wh, 20, TITLE_H + 6, "Network Interfaces", C_FG, C_HEADER);

    int y = TITLE_H + hdr_h + 10;
    int cx = 10, cw = ww - 20;

    if (g_nifaces == 0) {
        draw_str(fb, ww, wh, cx + 4, y, "No interfaces detected", C_MUTED, C_BG);
    } else {
        /* Rolling scale for the throughput meters (floor 128 KB/s). */
        uint64_t peak = 128 * 1024;
        for (int i = 0; i < g_nifaces; i++) {
            if (g_ifaces[i].rx_rate > peak) peak = g_ifaces[i].rx_rate;
            if (g_ifaces[i].tx_rate > peak) peak = g_ifaces[i].tx_rate;
        }

        int card_h = fh*3 + 26;
        int foot_reserve = fh + 12;
        for (int i = 0; i < g_nifaces && i < 4; i++) {
            if (y + card_h > wh - foot_reserve) break;
            iface_t *ifc = &g_ifaces[i];

            draw_card(fb, ww, wh, cx, y, cw, card_h);
            int ix = cx + 12;
            int row = y + 8;

            /* Name + IP + status pill */
            draw_str(fb, ww, wh, ix, row, ifc->name, C_FG, C_CARD);
            char ipbuf[24];
            snprintf(ipbuf, sizeof(ipbuf), "%s", ifc->ip4[0] ? ifc->ip4 : "---");
            draw_str(fb, ww, wh, ix + 9*fw, row, ipbuf, C_MUTED, C_CARD);

            const char *st = ifc->up ? "UP" : "DOWN";
            uint32_t stc = ifc->up ? C_UP : C_DOWN;
            int pill_w = (int)strlen(st) * fw + 14;
            int pill_x = cx + cw - pill_w - 10;
            fill_round(fb, ww, wh, pill_x, row - 2, pill_w, fh + 4, C_HEADER, 3);
            fill_round(fb, ww, wh, pill_x + 5, row + fh/2 - 2, 4, 4, stc, 2);
            draw_str(fb, ww, wh, pill_x + 12, row, st, stc, C_HEADER);
            row += fh + 6;

            /* RX / TX meters */
            int lbl_w = 3 * fw;
            int val_w = 11 * fw;
            int mx = ix + lbl_w + 4;
            int mw = cw - (lbl_w + 4) - val_w - 24;
            if (mw < 20) mw = 20;

            char rx_rate[16], tx_rate[16];
            fmt_rate(ifc->rx_rate, rx_rate, sizeof(rx_rate));
            fmt_rate(ifc->tx_rate, tx_rate, sizeof(tx_rate));

            draw_str(fb, ww, wh, ix, row, "RX", C_MUTED, C_CARD);
            meter(fb, ww, wh, mx, row + 2, mw, fh - 3,
                  (double)ifc->rx_rate / (double)peak, C_ACCENT);
            draw_str(fb, ww, wh, mx + mw + 8, row, rx_rate, C_FG, C_CARD);
            row += fh + 2;

            draw_str(fb, ww, wh, ix, row, "TX", C_MUTED, C_CARD);
            meter(fb, ww, wh, mx, row + 2, mw, fh - 3,
                  (double)ifc->tx_rate / (double)peak, C_ACCENT2);
            draw_str(fb, ww, wh, mx + mw + 8, row, tx_rate, C_FG, C_CARD);

            /* Totals (muted, right under the name row area) */
            char rx_tot[16], tx_tot[16];
            fmt_bytes(ifc->rx_bytes, rx_tot, sizeof(rx_tot));
            fmt_bytes(ifc->tx_bytes, tx_tot, sizeof(tx_tot));
            char totbuf[64];
            snprintf(totbuf, sizeof(totbuf), "%s / %s total", rx_tot, tx_tot);
            int tw = (int)strlen(totbuf) * fw;
            draw_str(fb, ww, wh, cx + cw - tw - 12, y + 8, totbuf, C_MUTED, C_CARD);

            y += card_h + 8;
        }
    }

    /* Bottom status bar */
    int bar_h = fh + 12;
    int bar_y = wh - bar_h;
    fill_rect(fb, ww, wh, 0, bar_y, ww, bar_h, C_HEADER);
    fill_rect(fb, ww, wh, 0, bar_y, ww, 1, C_SEP);
    draw_str(fb, ww, wh, 12, bar_y + 6, "Updating every second", C_MUTED, C_HEADER);
    char time_buf[16];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) snprintf(time_buf, sizeof(time_buf),
                     "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    else time_buf[0] = '\0';
    int txp = ww - (int)strlen(time_buf) * fw - 12;
    draw_str(fb, ww, wh, txp, bar_y + 6, time_buf, C_ACCENT, C_HEADER);
}

/* ── IPC message state ────────────────────────────────────────────────────── */
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

    uint32_t *fb = calloc((size_t)WIN_W * WIN_H, 4);
    if (!fb) return 1;

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { free(fb); return 1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIFI_SOCK, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock); free(fb); return 1;
    }

    /* IPC_APP_CONNECT: {uint16_t w, h; char title[60]} */
    uint8_t conn[64] = {0};
    uint16_t cw = WIN_W, ch = WIN_H;
    memcpy(conn, &cw, 2); memcpy(conn + 2, &ch, 2);
    snprintf((char *)(conn + 4), 60, "Network Monitor");
    ipc_send_msg(sock, IPC_APP_CONNECT, conn, sizeof(conn));

    /* Read IPC_WIN_CREATED */
    {
        uint8_t rbuf[28] = {0};
        int got = 0;
        while (got < 28) {
            ssize_t n = read(sock, rbuf + got, 28 - got);
            if (n <= 0) break;
            got += (int)n;
        }
    }

    /* Initial stats */
    signal(SIGPIPE, SIG_IGN);
    update_stats();
    render(fb);
    send_frame(sock, fb);

    MsgState ms = {0};
    bool running = true;
    struct timespec last_update;
    clock_gettime(CLOCK_MONOTONIC, &last_update);

    while (running) {
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        struct timeval tv = {0, 200000};
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
                    /* full message received */
                    switch (ms.type) {
                    case IPC_INPUT_KEY:
                        if (ms.plen >= 1 && ms.pld) {
                            uint8_t key = ms.pld[0];
                            if (key == 'q' || key == 'Q' || key == 0x1Bu) running = false;
                        }
                        break;
                    case IPC_WIN_RESIZE:
                        if (ms.plen >= 4 && ms.pld) {
                            uint16_t nw, nh;
                            memcpy(&nw, ms.pld, 2); memcpy(&nh, ms.pld + 2, 2);
                            if (nw >= 200 && nh >= 100 && nw <= 8192 && nh <= 8192) {
                                uint32_t *nb = realloc(fb, (size_t)nw * nh * 4);
                                if (nb) { fb = nb; g_win_w = nw; g_win_h = nh; }
                            }
                        }
                        break;
                    case IPC_INVALIDATE:
                        render(fb); send_frame(sock, fb);
                        break;
                    case IPC_APP_CLOSE:
                        running = false;
                        break;
                    }
                    msg_reset(&ms);
                }
            }
        }

        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        long elapsed_ms = (long)(now_ts.tv_sec - last_update.tv_sec) * 1000L
                        + (long)(now_ts.tv_nsec - last_update.tv_nsec) / 1000000L;
        if (elapsed_ms >= 1000L) {
            last_update = now_ts;
            update_stats();
            render(fb);
            send_frame(sock, fb);
        }
    }

    ipc_send_msg(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock);
    free(fb);
    free(g_glyph);
    return 0;
}
