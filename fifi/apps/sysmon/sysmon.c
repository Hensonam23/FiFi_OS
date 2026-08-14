/* FiFi System Monitor — live CPU, RAM, and process stats via IPC.
 * Reads /proc/stat and /proc/meminfo every second.
 * Build: gcc -O2 -static -o fifi-sysmon sysmon.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/sysinfo.h>
#include <time.h>

/* ── IPC ─────────────────────────────────────────────────────────────────── */
#define FIFI_SOCK        "/tmp/fifi-compositor.sock"
#include "../../shared/ipc.h"

/* ── Window ──────────────────────────────────────────────────────────────── */
#define WIN_W    480
#define WIN_H    400
#define TITLE_H   24   /* reserved for compositor title bar */
#define PAD       14
#define ROW_H     22
#define BAR_H     10

static int g_win_w = WIN_W;
static int g_win_h = WIN_H;

/* ── Colours (shared FiFi design language) ───────────────────────────────── */
#define C_BG      0x000e1620u   /* window background   */
#define C_CARD    0x0016202eu   /* panels / cards      */
#define C_HEADER  0x001a2740u   /* header / toolbar    */
#define C_ROW_A   0x0018232fu   /* zebra row (lighter) */
#define C_ROW_B   0x00131d29u   /* zebra row (darker)  */
#define C_BORDER  0x00243448u   /* subtle divider      */
#define C_ACCENT  0x00409cffu   /* primary / meter     */
#define C_ACCENT2 0x002f6bbfu   /* hover / dim accent  */
#define C_KEY     0x00409cffu   /* section headers     */
#define C_VAL     0x00d8e8f8u   /* primary text        */
#define C_GREY    0x006a8098u   /* muted / secondary   */
#define C_GREEN   0x0040cc80u   /* ok state            */
#define C_AMBER   0x00e0a030u   /* warning state       */
#define C_CPU_LO  0x00409cffu   /* meter fill (normal) */
#define C_CPU_HI  0x00e0a030u   /* meter fill (busy)   */
#define C_RAM     0x0040cc80u   /* memory meter        */
#define C_BAR_BG  0x000c141eu   /* meter track         */
#define C_SEC_HDR 0x001a2740u

/* ── PSF1 font ───────────────────────────────────────────────────────────── */
#define PSF1_MAGIC 0x0436u
typedef struct { uint16_t magic; uint8_t mode; uint8_t charsize; } Psf1Hdr;
static uint8_t *g_glyph = NULL;
static int g_glyph_h = 16;
static int g_nglyphs = 256;

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
    for (int row = 0; row < g_glyph_h; row++) {
        uint8_t b = bits[row];
        for (int col = 0; col < 8; col++) {
            if (b & (0x80u >> col)) {
                int x = px + col, y = py + row;
                if (x >= 0 && x < g_win_w && y >= 0 && y < g_win_h)
                    fb[y * g_win_w + x] = fg;
            }
        }
    }
}
static void draw_str(uint32_t *fb, const char *s, int x, int y, uint32_t fg) {
    for (; *s; s++, x += 9) draw_char(fb, (unsigned char)*s, x, y, fg);
}
static void fill(uint32_t *fb, int x, int y, int w, int h, uint32_t col) {
    for (int r = y; r < y+h; r++) {
        if (r < 0 || r >= g_win_h) continue;
        int x0 = x < 0 ? 0 : x, x1 = x+w > g_win_w ? g_win_w : x+w;
        for (int c = x0; c < x1; c++) fb[r * g_win_w + c] = col;
    }
}
/* Filled rectangle with softened (rounded) corners — radius r pixels. */
static void fill_round(uint32_t *fb, int x, int y, int w, int h, uint32_t col, int r) {
    if (r < 1) { fill(fb, x, y, w, h, col); return; }
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;
    for (int row = 0; row < h; row++) {
        int dy = (row < r) ? (r - 1 - row)
               : (row >= h - r) ? (row - (h - r)) : -1;
        int inset = 0;
        if (dy >= 0) {
            /* smallest inset dx such that (r-1-dx,dy) is inside quarter circle */
            while (inset < r) {
                int dx = r - 1 - inset;
                if (dx*dx + dy*dy <= (r-1)*(r-1)) break;
                inset++;
            }
        }
        fill(fb, x + inset, y + row, w - 2*inset, 1, col);
    }
}

/* A card panel with 1px subtle border. */
static void draw_card(uint32_t *fb, int x, int y, int w, int h) {
    fill_round(fb, x, y, w, h, C_CARD, 4);
}

/* Section header: accent label + short accent underline tab. */
static int section_header(uint32_t *fb, const char *label, int x, int y, int w) {
    fill(fb, x, y + 1, 3, g_glyph_h, C_ACCENT);          /* accent tick   */
    draw_str(fb, label, x + 8, y, C_ACCENT);
    int uw = w > 0 ? w : 40;
    fill(fb, x, y + g_glyph_h + 3, uw, 1, C_BORDER);      /* divider line  */
    return y + g_glyph_h + 6;
}

/* ── CPU usage tracking ──────────────────────────────────────────────────── */
typedef struct { unsigned long long user, nice, sys, idle, iowait, irq, softirq; } CpuStat;
#define MAX_CPUS 64
static CpuStat g_prev[MAX_CPUS + 1];  /* [0]=total, [1..N]=per-core */
static int     g_ncpus = 0;
static float   g_cpu_pct[MAX_CPUS + 1];  /* [0]=total */

static void parse_cpu_line(const char *line, CpuStat *s) {
    sscanf(line, "%*s %llu %llu %llu %llu %llu %llu %llu",
           &s->user, &s->nice, &s->sys, &s->idle,
           &s->iowait, &s->irq, &s->softirq);
}

static float cpu_pct(const CpuStat *a, const CpuStat *b) {
    unsigned long long total_a = a->user + a->nice + a->sys + a->idle + a->iowait + a->irq + a->softirq;
    unsigned long long total_b = b->user + b->nice + b->sys + b->idle + b->iowait + b->irq + b->softirq;
    unsigned long long dtotal  = total_b - total_a;
    if (dtotal == 0) return 0.0f;
    unsigned long long didle   = b->idle - a->idle;
    return (float)(dtotal - didle) * 100.0f / (float)dtotal;
}

static void update_cpu(void) {
    char buf[8192] = {0};
    int fd = open("/proc/stat", O_RDONLY);
    if (fd < 0) return;
    read(fd, buf, sizeof(buf)-1);
    close(fd);

    CpuStat cur[MAX_CPUS + 1] = {{0}};
    int ncpus = 0;
    char *line = buf;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line, "cpu", 3) == 0) {
            if (line[3] == ' ') {
                parse_cpu_line(line, &cur[0]);
            } else if (line[3] >= '0' && line[3] <= '9' && ncpus < MAX_CPUS) {
                /* Parse multi-digit core numbers (e.g. cpu10, cpu15) */
                int core_num = atoi(line + 3);
                int idx = core_num + 1;
                if (idx > 0 && idx <= MAX_CPUS) {
                    parse_cpu_line(line, &cur[idx]);
                    if (core_num + 1 > ncpus) ncpus = core_num + 1;
                }
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
    g_ncpus = ncpus;
    for (int i = 0; i <= ncpus && i <= MAX_CPUS; i++) {
        g_cpu_pct[i] = cpu_pct(&g_prev[i], &cur[i]);
        g_prev[i] = cur[i];
    }
}

/* ── Memory info ─────────────────────────────────────────────────────────── */
static unsigned long g_mem_total_mb = 0;
static unsigned long g_mem_used_mb  = 0;
static unsigned long g_mem_free_mb  = 0;
static unsigned long g_swap_total_mb = 0;
static unsigned long g_swap_used_mb  = 0;

static void update_mem(void) {
    char buf[4096] = {0};
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return;
    read(fd, buf, sizeof(buf)-1);
    close(fd);

    unsigned long mem_total=0, mem_free=0, mem_buffers=0, mem_cached=0;
    unsigned long swap_total=0, swap_free=0;
    char *line = buf;
    while (*line) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        unsigned long val;
        if      (sscanf(line, "MemTotal: %lu", &val) == 1) mem_total = val;
        else if (sscanf(line, "MemFree: %lu", &val) == 1)  mem_free = val;
        else if (sscanf(line, "Buffers: %lu", &val) == 1)  mem_buffers = val;
        else if (sscanf(line, "Cached: %lu", &val) == 1)   mem_cached = val;
        else if (sscanf(line, "SwapTotal: %lu", &val) == 1) swap_total = val;
        else if (sscanf(line, "SwapFree: %lu", &val) == 1)  swap_free = val;
        if (!nl) break;
        line = nl + 1;
    }
    g_mem_total_mb  = mem_total / 1024;
    g_mem_free_mb   = (mem_free + mem_buffers + mem_cached) / 1024;
    g_mem_used_mb   = g_mem_total_mb > g_mem_free_mb ? g_mem_total_mb - g_mem_free_mb : 0;
    g_swap_total_mb = swap_total / 1024;
    g_swap_used_mb  = swap_total > swap_free ? (swap_total - swap_free) / 1024 : 0;
}

/* ── Process count ───────────────────────────────────────────────────────── */
static int count_procs(void) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        char *end;
        strtol(de->d_name, &end, 10);
        if (*end == '\0') n++;
    }
    closedir(d);
    return n;
}

/* ── Draw a horizontal usage bar ─────────────────────────────────────────── */
static void draw_bar(uint32_t *fb, int x, int y, int w, int h,
                     float pct, uint32_t fill_col) {
    int r = h >= 8 ? 3 : (h/2);
    /* Track (subtle recessed groove) */
    fill_round(fb, x, y, w, h, C_BAR_BG, r);
    int fw = (int)((float)w * (pct > 100.0f ? 100.0f : pct) / 100.0f);
    if (fw < (r*2) && fw > 0) fw = r*2;
    if (fw > w) fw = w;
    if (fw > 0) fill_round(fb, x, y, fw, h, fill_col, r);
}

/* ── CPU history graph (last 60 samples) ─────────────────────────────────── */
#define HIST_N 60
static float g_cpu_hist[HIST_N] = {0};
static int   g_hist_head = 0;

static void push_hist(float v) {
    g_cpu_hist[g_hist_head % HIST_N] = v;
    g_hist_head++;
}

static void draw_graph(uint32_t *fb, int x, int y, int w, int h) {
    fill_round(fb, x, y, w, h, C_BAR_BG, 4);
    /* 25/50/75% guide lines */
    for (int q = 1; q <= 3; q++) {
        int gy = y + h - 1 - (h-2)*q/4;
        for (int i = x+2; i < x+w-2; i += 4) fill(fb, i, gy, 2, 1, C_BORDER);
    }

    int n = HIST_N;
    for (int i = 0; i < n && i < w-2; i++) {
        int idx = (g_hist_head - 1 - i + n * 1000) % n;
        float v = g_cpu_hist[idx];
        if (v < 0.0f) v = 0.0f;
        if (v > 100.0f) v = 100.0f;
        int bar_h = (int)((float)(h-2) * v / 100.0f);
        int bx = x + w - 2 - i;
        int by = y + h - 1 - bar_h;
        uint32_t col = v > 85.0f ? C_AMBER : C_ACCENT;
        if (bar_h > 0) {
            fill(fb, bx, by, 1, bar_h, col);
            fill(fb, bx, by, 1, 1, C_VAL);   /* bright tip */
        }
    }
}

/* ── Render ──────────────────────────────────────────────────────────────── */
static void render(uint32_t *fb) {
    int ww = g_win_w, wh = g_win_h;
    fill(fb, 0, 0, ww, wh, C_BG);
    /* Top TITLE_H blank — compositor draws title bar */

    int cx  = PAD;               /* content left edge  */
    int cw2 = ww - PAD*2;        /* content width      */
    int lblw = 96;               /* label gutter width */
    int barx = cx + lblw;
    int barw = ww - barx - PAD;
    int y = TITLE_H + PAD;

    /* ── CPU section ── */
    y = section_header(fb, "CPU", cx, y, cw2);

    /* Total CPU bar */
    char cpu_lbl[16];
    snprintf(cpu_lbl, sizeof(cpu_lbl), "Total %3.0f%%", g_cpu_pct[0]);
    draw_str(fb, cpu_lbl, cx, y + (BAR_H + 4 - g_glyph_h)/2, C_VAL);
    draw_bar(fb, barx, y + 2, barw, BAR_H, g_cpu_pct[0],
             g_cpu_pct[0] > 85.0f ? C_CPU_HI : C_CPU_LO);
    y += BAR_H + 8;

    /* Per-core bars (up to 8 shown, or fewer if window is short) */
    int show = g_ncpus < 8 ? g_ncpus : 8;
    if (show > 0) {
        int rows_h = show * (ROW_H - 4) + 6;
        draw_card(fb, cx, y, cw2, rows_h);
        y += 3;
    }
    for (int i = 0; i < show; i++) {
        if (y + ROW_H - 4 > wh - 4) break;
        char lbl[14];
        snprintf(lbl, sizeof(lbl), "Core %2d  %2.0f%%", i, g_cpu_pct[i+1]);
        if (i & 1) fill(fb, cx + 3, y, cw2 - 6, ROW_H - 4, C_ROW_A);
        draw_str(fb, lbl, cx + 8, y + (ROW_H - 4 - g_glyph_h)/2, C_GREY);
        draw_bar(fb, barx, y + (ROW_H - 4 - BAR_H)/2,
                 barw, BAR_H, g_cpu_pct[i+1],
                 g_cpu_pct[i+1] > 85.0f ? C_CPU_HI : C_CPU_LO);
        y += ROW_H - 4;
    }
    if (show > 0) y += 6;

    /* CPU history graph */
    if (y + 58 <= wh - 4) {
        y += 2;
        draw_graph(fb, cx, y, cw2, 50);
        draw_str(fb, "1 min", cx + 6, y + 4, C_GREY);
        y += 58;
    }

    /* ── Memory section ── */
    if (y + g_glyph_h + 6 + BAR_H + 8 > wh - 4) goto sys_section;
    y = section_header(fb, "Memory", cx, y, cw2);

    if (g_mem_total_mb > 0) {
        float mem_pct = (float)g_mem_used_mb * 100.0f / (float)g_mem_total_mb;
        char mem_lbl[48];
        snprintf(mem_lbl, sizeof(mem_lbl), "RAM %lu/%lu MB", g_mem_used_mb, g_mem_total_mb);
        draw_str(fb, "RAM", cx, y + (BAR_H + 4 - g_glyph_h)/2, C_VAL);
        draw_bar(fb, barx, y + 2, barw, BAR_H, mem_pct,
                 mem_pct > 85.0f ? C_AMBER : C_RAM);
        y += BAR_H + 6;
        draw_str(fb, mem_lbl, cx, y, C_GREY);
        y += g_glyph_h + 4;

        if (g_swap_total_mb > 0 && y + BAR_H + 6 <= wh - 4) {
            float sw_pct = (float)g_swap_used_mb * 100.0f / (float)g_swap_total_mb;
            char sw_lbl[48];
            snprintf(sw_lbl, sizeof(sw_lbl), "Swap %lu/%lu MB", g_swap_used_mb, g_swap_total_mb);
            draw_str(fb, "Swap", cx, y + (BAR_H + 4 - g_glyph_h)/2, C_VAL);
            draw_bar(fb, barx, y + 2, barw, BAR_H, sw_pct,
                     sw_pct > 50.0f ? C_AMBER : C_ACCENT2);
            y += BAR_H + 6;
            draw_str(fb, sw_lbl, cx, y, C_GREY);
            y += g_glyph_h + 4;
        }
    }

sys_section:
    /* ── System section ── */
    if (y + g_glyph_h + 6 + (ROW_H - 2)*2 > wh - 4) return;
    y += 2;
    y = section_header(fb, "System", cx, y, cw2);

    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        long up = si.uptime;
        char buf[64];
        int card_h = (ROW_H - 2) * 2 + 6;
        draw_card(fb, cx, y, cw2, card_h);
        y += 3;

        snprintf(buf, sizeof(buf), "Uptime   %ldh %02ldm %02lds",
                 up/3600, (up%3600)/60, up%60);
        draw_str(fb, buf, cx + 8, y + (ROW_H - 2 - g_glyph_h)/2, C_VAL);
        y += ROW_H - 2;

        if (y + ROW_H - 2 <= wh - 4) {
            fill(fb, cx + 3, y, cw2 - 6, ROW_H - 2, C_ROW_A);
            int np = count_procs();
            snprintf(buf, sizeof(buf), "Procs    %d   Load %.2f",
                     np, (double)si.loads[0] / 65536.0);
            draw_str(fb, buf, cx + 8, y + (ROW_H - 2 - g_glyph_h)/2, C_VAL);
        }
    }
}

/* ── IPC helpers ─────────────────────────────────────────────────────────── */
static void write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    while (n > 0) { ssize_t w = write(fd, p, n); if (w <= 0) break; p += w; n -= (size_t)w; }
}

static void ipc_send_msg(int fd, uint32_t type, const void *data, uint32_t len) {
    uint8_t hdr[8];
    memcpy(hdr, &type, 4); memcpy(hdr+4, &len, 4);
    write_all(fd, hdr, 8);
    if (len && data) write_all(fd, data, len);
}

static void send_frame(int fd, uint32_t *px) {
    uint32_t fw = (uint32_t)g_win_w, fh = (uint32_t)g_win_h;
    uint32_t frm[4] = {0, 0, fw, fh};
    size_t pix = (size_t)fw * fh * 4;   /* size_t: avoid 32-bit overflow */
    size_t total = 16 + pix;
    if (total > 0xFFFFFFFFu) return;
    uint8_t *msg = malloc(total);
    if (!msg) return;
    memcpy(msg, frm, 16);
    memcpy(msg+16, px, pix);
    ipc_send_msg(fd, IPC_APP_FRAME, msg, (uint32_t)total);
    free(msg);
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void) {
    font_load("/fifi-data/fonts/ter16b.psf");
    if (!g_glyph) { g_glyph = calloc(256*16, 1); g_glyph_h = 16; }

    uint32_t *fb = malloc((size_t)WIN_W * WIN_H * 4);
    if (!fb) return 1;

    /* Initial stats (prev baseline) */
    update_cpu();
    update_mem();

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return 1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIFI_SOCK, sizeof(addr.sun_path)-1);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) return 1;

    uint8_t conn[68] = {0};
    uint16_t cw = WIN_W, ch = WIN_H;
    memcpy(conn, &cw, 2); memcpy(conn+2, &ch, 2);
    snprintf((char*)(conn+4), 64, "System Monitor");
    ipc_send_msg(sock, IPC_APP_CONNECT, conn, sizeof(conn));

    uint8_t hdr8[8] = {0};
    read(sock, hdr8, 8);
    uint32_t type, plen;
    memcpy(&type, hdr8, 4); memcpy(&plen, hdr8+4, 4);
    if (type == IPC_WIN_CREATED && plen >= 20) { uint8_t r[20]; read(sock, r, 20); }

    signal(SIGPIPE, SIG_IGN);
    render(fb); send_frame(sock, fb);

    uint8_t ibuf[8]; int igot = 0;
    uint32_t itype = 0, iplen = 0, ipgot = 0;
    uint8_t payload[16] = {0};
    bool running = true;
    struct timespec last_tick;
    clock_gettime(CLOCK_MONOTONIC, &last_tick);

    while (running) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(sock, &rfds);
        struct timeval tv = {0, 50000};  /* 50ms = 20Hz */
        if (select(sock + 1, &rfds, NULL, NULL, &tv) < 0) break;

        uint8_t tbuf[512];
        ssize_t n = 0;
        if (FD_ISSET(sock, &rfds)) {
            n = read(sock, tbuf, sizeof(tbuf));
            if (n <= 0) break;
        }

        if (n > 0) {
            ssize_t pos = 0;
            while (pos < n) {
                if (igot < 8) {
                    ibuf[igot++] = tbuf[pos++];
                    if (igot == 8) {
                        memcpy(&itype,  ibuf, 4);
                        memcpy(&iplen, ibuf+4, 4);
                        if (iplen > 65536) { igot = 0; break; }
                        ipgot = 0;
                        memset(payload, 0, sizeof(payload));
                        if (iplen == 0) {
                            if (itype == IPC_INVALIDATE) {
                                update_cpu(); update_mem();
                                push_hist(g_cpu_pct[0]);
                                render(fb); send_frame(sock, fb);
                            } else if (itype == IPC_APP_CLOSE) {
                                running = false;
                            }
                            igot = 0;
                        }
                    }
                } else {
                    /* accumulate payload bytes (up to sizeof(payload)) */
                    uint32_t have = (uint32_t)(n - pos);
                    uint32_t need = iplen - ipgot;
                    uint32_t take = have < need ? have : need;
                    for (uint32_t k = 0; k < take && ipgot + k < (uint32_t)sizeof(payload); k++)
                        payload[ipgot + k] = tbuf[pos + k];
                    pos += (ssize_t)take; ipgot += take;
                    if (ipgot >= iplen) {
                        switch (itype) {
                        case IPC_INPUT_KEY:
                            if (iplen >= 1) {
                                uint8_t key = payload[0];
                                if (key == 'q' || key == 'Q' || key == 0x1B) running = false;
                            }
                            break;
                        case IPC_WIN_RESIZE:
                            if (iplen >= 4) {
                                uint16_t nw, nh;
                                memcpy(&nw, payload, 2); memcpy(&nh, payload+2, 2);
                                if (nw >= 200 && nh >= 100 && nw <= 8192 && nh <= 8192) {
                                    uint32_t *nb = realloc(fb, (size_t)nw * nh * 4);
                                    if (nb) { fb = nb; g_win_w = nw; g_win_h = nh; }
                                }
                            }
                            render(fb); send_frame(sock, fb);
                            break;
                        case IPC_APP_CLOSE:
                            running = false;
                            break;
                        }
                        igot = 0; itype = 0; iplen = 0; ipgot = 0;
                    }
                }
            }
        }

        /* Update every second using monotonic clock (time() unreliable in initramfs) */
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        long elapsed_ms = (long)(now_ts.tv_sec - last_tick.tv_sec) * 1000L
                        + (long)(now_ts.tv_nsec - last_tick.tv_nsec) / 1000000L;
        if (elapsed_ms >= 1000L) {
            last_tick = now_ts;
            update_cpu();
            update_mem();
            push_hist(g_cpu_pct[0]);
            render(fb);
            send_frame(sock, fb);
        }

    }

    ipc_send_msg(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock);
    free(fb);
    return 0;
}
