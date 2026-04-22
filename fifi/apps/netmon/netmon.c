/* fifi-netmon — Network Monitor IPC app for FiFi OS.
 * Shows interface stats, IP addresses, RX/TX rates, and ping status.
 * 480×340 window, updates every second. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── IPC protocol ────────────────────────────────────────────────────────── */
#define IPC_SOCK_PATH    "/tmp/fifi-ipc.sock"
#define IPC_APP_CONNECT  0x01u
#define IPC_APP_FRAME    0x02u
#define IPC_APP_TITLE    0x03u
#define IPC_APP_CLOSE    0x04u
#define IPC_WIN_CREATED  0x10u
#define IPC_INPUT_KEY    0x11u
#define IPC_INPUT_MOUSE  0x12u
#define IPC_INVALIDATE   0x15u
#define IPC_NOTIFY       0x16u

#define WIN_W   480
#define WIN_H   340
#define TITLE_H 24

/* ── PSF1 font ───────────────────────────────────────────────────────────── */
#define PSF1_MAGIC 0x0436
typedef struct { uint16_t magic; uint8_t mode; uint8_t charsize; } Psf1Hdr;
static uint8_t *g_glyph = NULL;
static uint32_t g_fw = 8, g_fh = 16;

static bool font_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    Psf1Hdr h;
    if (fread(&h, 1, sizeof(h), f) < sizeof(h) || h.magic != PSF1_MAGIC) { fclose(f); return false; }
    g_fh = h.charsize; g_fw = 8;
    size_t sz = 256 * g_fh;
    g_glyph = malloc(sz);
    if (!g_glyph) { fclose(f); return false; }
    fread(g_glyph, 1, sz, f);
    fclose(f); return true;
}

static void draw_char(uint32_t *fb, int cx, int cy, unsigned char c, uint32_t fg, uint32_t bg) {
    if (!g_glyph) return;
    uint8_t *row = g_glyph + (unsigned)c * g_fh;
    for (uint32_t y = 0; y < g_fh; y++) {
        for (uint32_t x = 0; x < g_fw; x++) {
            int px = cx + (int)x, py = cy + (int)y;
            if (px < 0 || py < 0 || px >= WIN_W || py >= WIN_H) continue;
            fb[py * WIN_W + px] = (row[y] & (0x80u >> x)) ? fg : bg;
        }
    }
}

static void draw_str(uint32_t *fb, int x, int y, const char *s, uint32_t fg, uint32_t bg) {
    for (int i = 0; s[i]; i++) draw_char(fb, x + i * (int)g_fw, y, (unsigned char)s[i], fg, bg);
}

static void fill_rect(uint32_t *fb, int x, int y, int w, int h, uint32_t col) {
    for (int row = 0; row < h; row++)
        for (int col2 = 0; col2 < w; col2++) {
            int px = x + col2, py = y + row;
            if (px >= 0 && py >= 0 && px < WIN_W && py < WIN_H)
                fb[py * WIN_W + px] = col;
        }
}

/* ── IPC helpers ─────────────────────────────────────────────────────────── */
typedef struct { uint32_t type, len; } IpcHdr;

static void ipc_send_msg(int fd, uint32_t type, const void *data, uint32_t len) {
    IpcHdr h = { type, len };
    write(fd, &h, sizeof(h));
    if (len && data) write(fd, data, len);
}

static void send_frame(int sock, uint32_t *fb) {
    uint32_t stride = WIN_W;
    uint32_t src_x = 0, src_y = 0;
    uint8_t msg[16 + WIN_W * WIN_H * 4];
    memcpy(msg + 0,  &stride, 4);
    memcpy(msg + 4,  &src_x,  4);
    memcpy(msg + 8,  &src_y,  4);
    uint32_t pw = WIN_W, ph = WIN_H;
    memcpy(msg + 12, &pw, 4);
    /* actual pixel data: one big rect */
    uint32_t total = 16 + WIN_W * WIN_H * 4;
    uint8_t *out = msg;
    memcpy(out + 0,  &stride, 4);
    memcpy(out + 4,  &src_x,  4);
    memcpy(out + 8,  &src_y,  4);
    memcpy(out + 12, &ph,     4);
    memcpy(out + 16, fb, WIN_W * WIN_H * 4);
    ipc_send_msg(sock, IPC_APP_FRAME, out, total);
}

/* ── Network stats ───────────────────────────────────────────────────────── */
#define MAX_IFACES 8
typedef struct {
    char     name[16];
    uint64_t rx_bytes, tx_bytes;
    uint64_t rx_prev,  tx_prev;
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
    /* Check flags for UP */
    if (ioctl(sk, SIOCGIFFLAGS, &ifr) == 0)
        ifc->up = !!(ifr.ifr_flags & IFF_UP) && !!(ifr.ifr_flags & IFF_RUNNING);
    close(sk);
}

static void update_stats(void) {
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;
    char line[256];
    /* skip 2 header lines */
    fgets(line, sizeof(line), f);
    fgets(line, sizeof(line), f);
    g_nifaces = 0;
    while (fgets(line, sizeof(line), f) && g_nifaces < MAX_IFACES) {
        char name[16];
        uint64_t rx, tx, tmp;
        int n = sscanf(line, " %15[^:]: %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                       name, &rx, &tmp, &tmp, &tmp, &tmp, &tmp, &tmp, &tmp, &tx);
        if (n < 10 || strcmp(name, "lo") == 0) continue;
        iface_t *ifc = &g_ifaces[g_nifaces];
        /* check if we already know this iface */
        bool found = false;
        for (int i = 0; i < g_nifaces; i++) {
            if (strcmp(g_ifaces[i].name, name) == 0) { ifc = &g_ifaces[i]; found = true; break; }
        }
        if (!found) {
            memset(ifc, 0, sizeof(*ifc));
            strncpy(ifc->name, name, sizeof(ifc->name) - 1);
            g_nifaces++;
        }
        ifc->rx_rate = rx > ifc->rx_bytes ? rx - ifc->rx_bytes : 0;
        ifc->tx_rate = tx > ifc->tx_bytes ? tx - ifc->tx_bytes : 0;
        ifc->rx_bytes = rx;
        ifc->tx_bytes = tx;
        update_ip(ifc);
    }
    fclose(f);
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
    if (b >= 1024*1024*1024)
        snprintf(buf, bufsz, "%6.2f GB", b / (1024.0*1024.0*1024.0));
    else if (b >= 1024*1024)
        snprintf(buf, bufsz, "%6.2f MB", b / (1024.0*1024.0));
    else if (b >= 1024)
        snprintf(buf, bufsz, "%6.2f KB", b / 1024.0);
    else
        snprintf(buf, bufsz, "%6llu  B", (unsigned long long)b);
}

/* ── Render ──────────────────────────────────────────────────────────────── */
#define C_BG      0xFF0C1018u
#define C_HEAD    0xFF18283Cu
#define C_SEP     0xFF1E3050u
#define C_ACCENT  0xFF3878D8u
#define C_FG      0xFFC8D4F0u
#define C_MUTED   0xFF405868u
#define C_UP      0xFF40C878u
#define C_DOWN    0xFFE05040u
#define C_RX      0xFF30A0E0u
#define C_TX      0xFFE08830u

static void render(uint32_t *fb) {
    fill_rect(fb, 0, 0, WIN_W, WIN_H, C_BG);

    /* Title area (reserved for compositor chrome) */
    fill_rect(fb, 0, 0, WIN_W, TITLE_H, 0xFF0C1018u);

    /* Section header */
    int y = TITLE_H + 6;
    fill_rect(fb, 0, y, WIN_W, 1, C_SEP);
    y += 3;
    draw_str(fb, 8, y, "NETWORK INTERFACES", C_ACCENT, C_BG);
    y += (int)g_fh + 4;
    fill_rect(fb, 0, y, WIN_W, 1, C_SEP);
    y += 4;

    if (g_nifaces == 0) {
        draw_str(fb, 8, y, "No interfaces detected", C_MUTED, C_BG);
        return;
    }

    for (int i = 0; i < g_nifaces && i < 4; i++) {
        iface_t *ifc = &g_ifaces[i];

        /* Interface name + status */
        uint32_t status_col = ifc->up ? C_UP : C_DOWN;
        char namebuf[32];
        snprintf(namebuf, sizeof(namebuf), "%-10s", ifc->name);
        draw_str(fb, 8, y, namebuf, C_FG, C_BG);
        draw_str(fb, 8 + 11 * (int)g_fw, y, ifc->up ? "UP" : "DOWN", status_col, C_BG);

        /* IP address */
        draw_str(fb, 8 + 15 * (int)g_fw, y, ifc->ip4[0] ? ifc->ip4 : "---", C_FG, C_BG);
        y += (int)g_fh + 2;

        /* RX/TX rates */
        char rx_rate[16], tx_rate[16];
        fmt_rate(ifc->rx_rate, rx_rate, sizeof(rx_rate));
        fmt_rate(ifc->tx_rate, tx_rate, sizeof(tx_rate));
        char ratebuf[80];
        snprintf(ratebuf, sizeof(ratebuf), "  RX: %-12s  TX: %-12s", rx_rate, tx_rate);
        draw_str(fb, 8, y, ratebuf, C_MUTED, C_BG);
        y += (int)g_fh + 2;

        /* Totals */
        char rx_tot[16], tx_tot[16];
        fmt_bytes(ifc->rx_bytes, rx_tot, sizeof(rx_tot));
        fmt_bytes(ifc->tx_bytes, tx_tot, sizeof(tx_tot));
        char totbuf[80];
        snprintf(totbuf, sizeof(totbuf), "  Total RX: %-10s TX: %-10s", rx_tot, tx_tot);
        draw_str(fb, 8, y, totbuf, C_MUTED, C_BG);
        y += (int)g_fh + 8;

        /* Separator between interfaces */
        if (i < g_nifaces - 1)
            fill_rect(fb, 8, y, WIN_W - 16, 1, C_SEP);
        y += 6;
    }

    /* Bottom hint bar */
    int bar_y = WIN_H - (int)g_fh - 6;
    fill_rect(fb, 0, bar_y - 2, WIN_W, 1, C_SEP);
    fill_rect(fb, 0, bar_y - 1, WIN_W, (int)g_fh + 8, 0xFF0A0E14u);
    draw_str(fb, 8, bar_y + 2, "Updates every second", C_MUTED, 0xFF0A0E14u);
    char time_buf[32];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    else time_buf[0] = 0;
    int tx = WIN_W - (int)strlen(time_buf) * (int)g_fw - 8;
    draw_str(fb, tx, bar_y + 2, time_buf, C_ACCENT, 0xFF0A0E14u);
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void) {
    if (!font_load("/fifi-data/fonts/ter16b.psf"))
        font_load("/fifi-data/fonts/default.psf");

    uint32_t *fb = calloc(WIN_W * WIN_H, 4);
    if (!fb) return 1;

    /* Connect to compositor IPC */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { free(fb); return 1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, IPC_SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock); free(fb); return 1;
    }

    /* Send CONNECT */
    uint8_t conn[16];
    uint32_t ww = WIN_W, wh = WIN_H, wx = 80, wy = 80;
    memcpy(conn + 0,  &ww, 4);
    memcpy(conn + 4,  &wh, 4);
    memcpy(conn + 8,  &wx, 4);
    memcpy(conn + 12, &wy, 4);
    ipc_send_msg(sock, IPC_APP_CONNECT, conn, sizeof(conn));

    /* Send TITLE */
    const char *title = "Network Monitor";
    ipc_send_msg(sock, IPC_APP_TITLE, title, (uint32_t)strlen(title));

    /* Initial render */
    update_stats();
    render(fb);
    send_frame(sock, fb);

    time_t last_update = time(NULL);
    uint8_t in_hdr[8];
    int hdr_got = 0;
    uint32_t in_type = 0, in_plen = 0, in_pgot = 0;
    uint8_t *in_pld = NULL;
    bool running = true;

    while (running) {
        /* Poll socket with 200ms timeout */
        struct timeval tv = {0, 200000};
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        int sel = select(sock + 1, &fds, NULL, NULL, &tv);
        if (sel < 0) break;

        if (sel > 0) {
            uint8_t tbuf[4096]; ssize_t n = read(sock, tbuf, sizeof(tbuf));
            if (n <= 0) break;
            int pos = 0;
            while (pos < n) {
                if (hdr_got < 8) {
                    int need = 8 - hdr_got, have = (int)n - pos, take = need < have ? need : have;
                    memcpy(in_hdr + hdr_got, tbuf + pos, take);
                    hdr_got += take; pos += take;
                    if (hdr_got == 8) {
                        memcpy(&in_type, in_hdr, 4);
                        memcpy(&in_plen, in_hdr + 4, 4);
                        in_pgot = 0;
                        in_pld = in_plen > 0 ? malloc(in_plen) : NULL;
                    }
                } else if (in_pgot < in_plen) {
                    uint32_t need = in_plen - in_pgot, have = (uint32_t)(n - pos),
                             take = need < have ? need : have;
                    if (in_pld) memcpy(in_pld + in_pgot, tbuf + pos, take);
                    in_pgot += take; pos += take;
                    if (in_pgot >= in_plen) {
                        if (in_type == IPC_INPUT_KEY && in_plen >= 1) {
                            uint8_t key = in_pld ? in_pld[0] : 0;
                            if (key == 'q' || key == 'Q') running = false;
                        } else if (in_type == IPC_INVALIDATE) {
                            render(fb); send_frame(sock, fb);
                        }
                        free(in_pld); in_pld = NULL;
                        hdr_got = 0; in_plen = 0; in_pgot = 0;
                    }
                } else {
                    if (in_type == IPC_INVALIDATE) { render(fb); send_frame(sock, fb); }
                    hdr_got = 0; in_plen = 0; in_pgot = 0;
                }
            }
        }

        /* Update every second */
        time_t now = time(NULL);
        if (now > last_update) {
            last_update = now;
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
