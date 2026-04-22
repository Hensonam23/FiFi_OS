/* FiFi Settings — standalone IPC process.
 * Shows system info and controls ALSA volume directly.
 * Theme and display settings are managed by the compositor.
 *
 * Build: gcc -O2 -static -o fifi-settings settings.c
 */

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
#include <sys/ioctl.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <sound/asound.h>

/* ── IPC ─────────────────────────────────────────────────────────────────── */
#define FIFI_SOCK        "/tmp/fifi-compositor.sock"
#define IPC_APP_CONNECT  0x01u
#define IPC_APP_FRAME    0x02u
#define IPC_APP_CLOSE    0x04u
#define IPC_WIN_CREATED  0x10u
#define IPC_INPUT_KEY    0x11u
#define IPC_INPUT_MOUSE  0x12u
#define IPC_INVALIDATE   0x15u
#define IPC_NOTIFY       0x16u

/* ── Window ──────────────────────────────────────────────────────────────── */
#define WIN_W 480
#define WIN_H 380
#define PAD   16
#define ROW_H 28

/* ── Colours ─────────────────────────────────────────────────────────────── */
#define C_BG      0x00121820u
#define C_HDR     0x001a2432u
#define C_ROW_A   0x00141c28u
#define C_ROW_B   0x00101820u
#define C_ACCENT  0x00206090u
#define C_FILL    0x00307090u
#define C_BORDER  0x00243448u
#define C_TITLE   0x00d0e8ffu
#define C_KEY     0x0060a0c0u
#define C_VAL     0x00b0c8e0u
#define C_GREY    0x00506070u
#define C_BTN_BG  0x001e2c40u
#define C_BTN_HL  0x00284860u

/* ── PSF1 font (same as filebrowser) ─────────────────────────────────────── */
#define PSF1_MAGIC 0x0436u
typedef struct { uint16_t magic; uint8_t mode; uint8_t charsize; } Psf1Hdr;

static uint8_t *g_glyph   = NULL;
static int      g_glyph_h = 16;
static int      g_nglyphs = 256;

static bool font_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    Psf1Hdr h;
    if (read(fd, &h, 4) != 4 || h.magic != PSF1_MAGIC) { close(fd); return false; }
    g_glyph_h = h.charsize;
    g_nglyphs = (h.mode & 1) ? 512 : 256;
    int tot = g_nglyphs * g_glyph_h;
    g_glyph = malloc(tot);
    if (!g_glyph) { close(fd); return false; }
    if (read(fd, g_glyph, tot) < tot) { free(g_glyph); g_glyph = NULL; close(fd); return false; }
    close(fd);
    return true;
}

static void draw_char(uint32_t *fb, int c, int px, int py, uint32_t fg) {
    if (!g_glyph || c < 0 || c >= g_nglyphs) return;
    const uint8_t *bits = g_glyph + c * g_glyph_h;
    for (int r = 0; r < g_glyph_h; r++) {
        uint8_t b = bits[r];
        for (int col = 0; col < 8; col++) {
            if (b & (0x80u >> col)) {
                int x = px + col, y = py + r;
                if (x >= 0 && x < WIN_W && y >= 0 && y < WIN_H)
                    fb[y * WIN_W + x] = fg;
            }
        }
    }
}

static void draw_str(uint32_t *fb, const char *s, int x, int y, uint32_t fg) {
    for (; *s; s++, x += 9) draw_char(fb, (unsigned char)*s, x, y, fg);
}

static int str_w(const char *s) { return (int)strlen(s) * 9; }

static void fill(uint32_t *fb, int x, int y, int w, int h, uint32_t col) {
    for (int r = y; r < y + h; r++) {
        if (r < 0 || r >= WIN_H) continue;
        int x0 = x < 0 ? 0 : x, x1 = x + w > WIN_W ? WIN_W : x + w;
        for (int c = x0; c < x1; c++) fb[r * WIN_W + c] = col;
    }
}

static void hline(uint32_t *fb, int y, uint32_t col) {
    if (y < 0 || y >= WIN_H) return;
    for (int x = 0; x < WIN_W; x++) fb[y * WIN_W + x] = col;
}

/* ── ALSA volume (direct ioctl — no library) ─────────────────────────────── */
static int   g_ctl    = -1;
static struct snd_ctl_elem_id g_vid;
static long  g_vmin   = 0, g_vmax = 100;
static int   g_vcount = 2;
static int   g_vol    = 50;

static void alsa_init(void) {
    for (int card = 0; card < 4; card++) {
        char p[32]; snprintf(p, sizeof(p), "/dev/snd/controlC%d", card);
        g_ctl = open(p, O_RDWR);
        if (g_ctl >= 0) break;
    }
    if (g_ctl < 0) return;

    struct snd_ctl_elem_list list = {0};
    if (ioctl(g_ctl, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) return;
    unsigned total = list.count;
    if (!total) return;
    struct snd_ctl_elem_id *ids = calloc(total, sizeof(*ids));
    if (!ids) return;
    list.space = total; list.pids = ids;
    ioctl(g_ctl, SNDRV_CTL_IOCTL_ELEM_LIST, &list);

    static const char *pref[] = {
        "Master Playback Volume","PCM Playback Volume",
        "Speaker Playback Volume","Headphone Playback Volume", NULL
    };
    int fi = -1;
    for (int pi = 0; pref[pi] && fi < 0; pi++)
        for (unsigned i = 0; i < list.used; i++)
            if (!strcmp((char*)ids[i].name, pref[pi])) { fi = (int)i; break; }
    if (fi < 0)
        for (unsigned i = 0; i < list.used && fi < 0; i++)
            if (ids[i].iface == SNDRV_CTL_ELEM_IFACE_MIXER &&
                strstr((char*)ids[i].name, "Volume")) fi = (int)i;
    if (fi < 0) { free(ids); return; }

    struct snd_ctl_elem_info info = {0};
    info.id = ids[fi];
    if (ioctl(g_ctl, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0 ||
        info.type != SNDRV_CTL_ELEM_TYPE_INTEGER) { free(ids); return; }

    g_vid    = ids[fi];
    g_vmin   = info.value.integer.min;
    g_vmax   = info.value.integer.max;
    g_vcount = (int)info.count;
    free(ids);

    struct snd_ctl_elem_value ev = {0};
    ev.id = g_vid;
    if (ioctl(g_ctl, SNDRV_CTL_IOCTL_ELEM_READ, &ev) == 0) {
        long range = g_vmax - g_vmin;
        if (range > 0)
            g_vol = (int)((ev.value.integer.value[0] - g_vmin) * 100 / range);
    }
    if (g_vol == 0) g_vol = 70;  /* default if silent */
}

static void alsa_set_vol(int v) {
    if (v < 0) v = 0; if (v > 100) v = 100;
    g_vol = v;
    if (g_ctl < 0) return;
    long range = g_vmax - g_vmin;
    long raw   = g_vmin + (long)v * range / 100;
    struct snd_ctl_elem_value ev = {0};
    ev.id = g_vid;
    int ch = g_vcount < 128 ? g_vcount : 128;
    for (int i = 0; i < ch; i++) ev.value.integer.value[i] = raw;
    ioctl(g_ctl, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev);
}

/* ── System info ─────────────────────────────────────────────────────────── */
typedef struct { char key[16]; char val[64]; } InfoRow;
#define N_INFO 5
static InfoRow g_info[N_INFO];

static void gather_info(void) {
    struct sysinfo si;
    sysinfo(&si);

    snprintf(g_info[0].key, sizeof(g_info[0].key), "Uptime");
    long up = si.uptime;
    snprintf(g_info[0].val, sizeof(g_info[0].val),
             "%ldh %02ldm %02lds", up/3600, (up%3600)/60, up%60);

    snprintf(g_info[1].key, sizeof(g_info[1].key), "Memory");
    unsigned long total_mb = si.totalram * si.mem_unit / 1024 / 1024;
    unsigned long free_mb  = si.freeram  * si.mem_unit / 1024 / 1024;
    unsigned long used_mb  = total_mb - free_mb;
    snprintf(g_info[1].val, sizeof(g_info[1].val),
             "%lu MB / %lu MB", used_mb, total_mb);

    snprintf(g_info[2].key, sizeof(g_info[2].key), "Load 1m");
    double load = (double)si.loads[0] / 65536.0;
    snprintf(g_info[2].val, sizeof(g_info[2].val), "%.2f", load);

    snprintf(g_info[3].key, sizeof(g_info[3].key), "Processes");
    snprintf(g_info[3].val, sizeof(g_info[3].val), "%u", si.procs);

    snprintf(g_info[4].key, sizeof(g_info[4].key), "Time");
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    snprintf(g_info[4].val, sizeof(g_info[4].val),
             "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
}

/* ── Volume slider geometry ───────────────────────────────────────────────── */
static int  g_sl_x, g_sl_y, g_sl_w = 200, g_sl_h = 14;
static bool g_dragging = false;

/* ── Render ──────────────────────────────────────────────────────────────── */
static void render(uint32_t *fb) {
    fill(fb, 0, 0, WIN_W, WIN_H, C_BG);

    /* Title bar */
    fill(fb, 0, 0, WIN_W, 32, C_HDR);
    hline(fb, 31, C_BORDER);
    draw_str(fb, "  FiFi Settings", PAD, (32 - g_glyph_h)/2, C_TITLE);

    int y = 42;

    /* ── System info section ── */
    draw_str(fb, "System", PAD, y, C_KEY);
    hline(fb, y + g_glyph_h + 3, C_BORDER);
    y += g_glyph_h + 8;

    gather_info();
    for (int i = 0; i < N_INFO; i++) {
        uint32_t bg = (i & 1) ? C_ROW_B : C_ROW_A;
        fill(fb, 0, y, WIN_W, ROW_H, bg);
        draw_str(fb, g_info[i].key, PAD, y + (ROW_H - g_glyph_h)/2, C_KEY);
        int kw = 10 * 9;  /* key column width */
        draw_str(fb, g_info[i].val, PAD + kw, y + (ROW_H - g_glyph_h)/2, C_VAL);
        y += ROW_H;
    }

    y += 16;

    /* ── Volume section ── */
    draw_str(fb, "Volume", PAD, y, C_KEY);
    hline(fb, y + g_glyph_h + 3, C_BORDER);
    y += g_glyph_h + 12;

    /* Slider track */
    g_sl_x = PAD + 80;
    g_sl_y = y + (ROW_H - g_sl_h) / 2;

    draw_str(fb, "Level:", PAD, y + (ROW_H - g_glyph_h)/2, C_KEY);

    fill(fb, g_sl_x, g_sl_y, g_sl_w, g_sl_h, C_ACCENT);
    /* Fill portion */
    int filled = g_sl_w * g_vol / 100;
    fill(fb, g_sl_x, g_sl_y, filled, g_sl_h, C_FILL);
    /* Thumb */
    fill(fb, g_sl_x + filled - 4, g_sl_y - 3, 8, g_sl_h + 6, C_VAL);

    /* Percentage label */
    char pct[8]; snprintf(pct, sizeof(pct), "%d%%", g_vol);
    draw_str(fb, pct, g_sl_x + g_sl_w + 10, y + (ROW_H - g_glyph_h)/2, C_VAL);

    y += ROW_H + 12;

    /* ── Note about compositor-side settings ── */
    fill(fb, PAD, y, WIN_W - 2*PAD, ROW_H + 4, C_ROW_A);
    draw_str(fb, "Theme and display: use the compositor settings panel (F3)",
             PAD + 8, y + 6, C_GREY);

    y += ROW_H + 20;

    /* ── Close button ── */
    int bw = 80, bh = 26;
    int bx = (WIN_W - bw) / 2;
    fill(fb, bx, y, bw, bh, C_BTN_BG);
    fill(fb, bx, y, bw, 1, C_BORDER);
    fill(fb, bx, y+bh-1, bw, 1, C_BORDER);
    fill(fb, bx, y, 1, bh, C_BORDER);
    fill(fb, bx+bw-1, y, 1, bh, C_BORDER);
    const char *lbl = "Close";
    draw_str(fb, lbl, bx + (bw - str_w(lbl))/2, y + (bh - g_glyph_h)/2, C_VAL);

    /* Border */
    for (int x = 0; x < WIN_W; x++) { fb[x] = C_BORDER; fb[(WIN_H-1)*WIN_W+x] = C_BORDER; }
    for (int ry = 0; ry < WIN_H; ry++) { fb[ry*WIN_W] = C_BORDER; fb[ry*WIN_W+WIN_W-1] = C_BORDER; }
}

/* ── IPC helpers ─────────────────────────────────────────────────────────── */
static void ipc_send_msg(int fd, uint32_t type, const void *data, uint32_t len) {
    uint8_t hdr[8];
    memcpy(hdr, &type, 4); memcpy(hdr+4, &len, 4);
    write(fd, hdr, 8);
    if (len && data) write(fd, data, len);
}

static void send_frame(int fd, uint32_t *px) {
    uint32_t frm[4] = {0, 0, WIN_W, WIN_H};
    uint32_t total  = 16 + WIN_W * WIN_H * 4;
    uint8_t *msg    = malloc(total);
    if (!msg) return;
    memcpy(msg, frm, 16);
    memcpy(msg+16, px, WIN_W * WIN_H * 4);
    ipc_send_msg(fd, IPC_APP_FRAME, msg, total);
    free(msg);
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void) {
    font_load("/fifi-data/fonts/ter16b.psf");
    if (!g_glyph) { g_glyph = calloc(256*16, 1); g_glyph_h = 16; }
    alsa_init();

    uint32_t *fb = malloc(WIN_W * WIN_H * 4);
    if (!fb) return 1;

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return 1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIFI_SOCK, sizeof(addr.sun_path)-1);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); return 1;
    }

    uint8_t conn[68] = {0};
    uint16_t cw = WIN_W, ch = WIN_H;
    memcpy(conn, &cw, 2); memcpy(conn+2, &ch, 2);
    snprintf((char*)(conn+4), 64, "Settings");
    ipc_send_msg(sock, IPC_APP_CONNECT, conn, sizeof(conn));

    uint8_t hdr8[8] = {0};
    read(sock, hdr8, 8);
    uint32_t type, plen;
    memcpy(&type, hdr8, 4); memcpy(&plen, hdr8+4, 4);
    if (type == IPC_WIN_CREATED && plen >= 20) {
        uint8_t r[20]; read(sock, r, 20);
    }

    render(fb);
    send_frame(sock, fb);

    fcntl(sock, F_SETFL, O_NONBLOCK);

    uint8_t ibuf[8]; int igot = 0;
    uint8_t *ipld = NULL; uint32_t iplen = 0, ipgot = 0;
    bool running = true;
    bool prev_lb = false;
    time_t last_refresh = 0;

    while (running) {
        uint8_t tbuf[256];
        ssize_t n = read(sock, tbuf, sizeof(tbuf));
        if (n == 0) break;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;

        if (n > 0) {
            ssize_t pos = 0;
            while (pos < n) {
                if (igot < 8) {
                    ibuf[igot++] = tbuf[pos++];
                    if (igot == 8) {
                        memcpy(&type,  ibuf,   4);
                        memcpy(&iplen, ibuf+4, 4);
                        if (iplen > 65536) { igot = 0; break; }
                        if (iplen) { ipld = malloc(iplen); ipgot = 0; }
                    }
                } else if (iplen > 0 && ipgot < iplen) {
                    uint32_t need = iplen - ipgot, have = (uint32_t)(n - pos);
                    uint32_t take = need < have ? need : have;
                    if (ipld) memcpy(ipld + ipgot, tbuf + pos, take);
                    ipgot += take; pos += take;
                    if (ipgot >= iplen) {
                        if (type == IPC_INPUT_KEY && iplen >= 1) {
                            uint8_t k = ipld ? ipld[0] : 0;
                            if (k == 'q' || k == 'Q' || k == 0x1B) running = false;
                        } else if (type == IPC_INPUT_MOUSE && iplen >= 9) {
                            int32_t mx, my; uint8_t btns;
                            memcpy(&mx, ipld, 4); memcpy(&my, ipld+4, 4);
                            btns = ipld[8];
                            bool lb = (btns & 1);

                            if (lb) {
                                /* Slider interaction */
                                if (my >= g_sl_y - 6 && my <= g_sl_y + g_sl_h + 6 &&
                                    mx >= g_sl_x && mx < g_sl_x + g_sl_w) {
                                    int newvol = (mx - g_sl_x) * 100 / g_sl_w;
                                    if (newvol < 0) newvol = 0;
                                    if (newvol > 100) newvol = 100;
                                    alsa_set_vol(newvol);
                                    render(fb);
                                    send_frame(sock, fb);
                                }
                                /* Close button: roughly bottom center area */
                                int bw = 80, bh = 26;
                                int bx = (WIN_W - bw) / 2;
                                if (!lb && prev_lb) {  /* release */
                                    (void)bx; (void)bh;
                                }
                            }
                            if (!lb && prev_lb) {
                                /* On release — check close button */
                                int bw = 80;
                                int bx = (WIN_W - bw) / 2;
                                if (mx >= bx && mx < bx + bw &&
                                    my >= WIN_H - 80 && my < WIN_H - 20)
                                    running = false;
                                /* Volume slider release — notify compositor */
                                if (my >= g_sl_y - 6 && my <= g_sl_y + g_sl_h + 6 &&
                                    mx >= g_sl_x && mx < g_sl_x + g_sl_w) {
                                    char ntxt[24];
                                    int cv = (mx - g_sl_x) * 100 / g_sl_w;
                                    if (cv < 0) cv = 0; if (cv > 100) cv = 100;
                                    int nlen = snprintf(ntxt, sizeof(ntxt),
                                                        "Volume: %d%%", cv);
                                    if (nlen > 0)
                                        ipc_send_msg(sock, IPC_NOTIFY, ntxt, (uint32_t)nlen);
                                }
                            }
                            prev_lb = lb;
                        }
                        free(ipld); ipld = NULL;
                        igot = 0; iplen = 0; ipgot = 0;
                    }
                } else {
                    if (type == IPC_INVALIDATE) {
                        render(fb);
                        send_frame(sock, fb);
                    }
                    igot = 0; iplen = 0; ipgot = 0;
                }
            }
        }

        /* Refresh system info every second */
        time_t now = time(NULL);
        if (now != last_refresh) {
            last_refresh = now;
            render(fb);
            send_frame(sock, fb);
        }

        struct timespec ts = {0, 16000000}; /* 60Hz */
        nanosleep(&ts, NULL);
    }

    ipc_send_msg(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock);
    free(fb);
    free(g_glyph);
    return 0;
}
