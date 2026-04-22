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
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/sysinfo.h>
#include <time.h>

/* ── IPC ─────────────────────────────────────────────────────────────────── */
#define FIFI_SOCK        "/tmp/fifi-compositor.sock"
#define IPC_APP_CONNECT  0x01u
#define IPC_APP_FRAME    0x02u
#define IPC_APP_CLOSE    0x04u
#define IPC_WIN_CREATED  0x10u
#define IPC_INPUT_KEY    0x11u
#define IPC_INVALIDATE   0x15u

/* ── Window ──────────────────────────────────────────────────────────────── */
#define WIN_W    480
#define WIN_H    400
#define TITLE_H   24   /* reserved for compositor title bar */
#define PAD       14
#define ROW_H     22
#define BAR_H     10

/* ── Colours ─────────────────────────────────────────────────────────────── */
#define C_BG      0x00101820u
#define C_ROW_A   0x00141c28u
#define C_ROW_B   0x00101820u
#define C_BORDER  0x00243448u
#define C_KEY     0x0060a0c0u
#define C_VAL     0x00b0c8e0u
#define C_GREY    0x00506070u
#define C_CPU_LO  0x00206890u
#define C_CPU_HI  0x00cc4422u
#define C_RAM     0x00408040u
#define C_BAR_BG  0x001a2432u
#define C_SEC_HDR 0x001a2432u

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
                if (x >= 0 && x < WIN_W && y >= 0 && y < WIN_H)
                    fb[y * WIN_W + x] = fg;
            }
        }
    }
}
static void draw_str(uint32_t *fb, const char *s, int x, int y, uint32_t fg) {
    for (; *s; s++, x += 9) draw_char(fb, (unsigned char)*s, x, y, fg);
}
static void fill(uint32_t *fb, int x, int y, int w, int h, uint32_t col) {
    for (int r = y; r < y+h; r++) {
        if (r < 0 || r >= WIN_H) continue;
        int x0 = x < 0 ? 0 : x, x1 = x+w > WIN_W ? WIN_W : x+w;
        for (int c = x0; c < x1; c++) fb[r * WIN_W + c] = col;
    }
}
static void hline(uint32_t *fb, int y, uint32_t col) {
    if (y < 0 || y >= WIN_H) return;
    for (int x = 0; x < WIN_W; x++) fb[y * WIN_W + x] = col;
}

/* ── CPU usage tracking ──────────────────────────────────────────────────── */
typedef struct { unsigned long long user, nice, sys, idle, iowait, irq, softirq; } CpuStat;
#define MAX_CPUS 32
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
    char buf[4096] = {0};
    int fd = open("/proc/stat", O_RDONLY);
    if (fd < 0) return;
    read(fd, buf, sizeof(buf)-1);
    close(fd);

    CpuStat cur[MAX_CPUS + 1] = {0};
    int ncpus = 0;
    char *line = buf;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line, "cpu", 3) == 0) {
            if (line[3] == ' ') {
                parse_cpu_line(line, &cur[0]);
            } else if (line[3] >= '0' && line[3] <= '9' && ncpus < MAX_CPUS) {
                int idx = line[3] - '0' + 1;
                if (idx <= MAX_CPUS) { parse_cpu_line(line, &cur[idx]); ncpus++; }
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
        if (!nl) break; line = nl + 1;
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
    fill(fb, x, y, w, h, C_BAR_BG);
    int fw = (int)((float)w * (pct > 100.0f ? 100.0f : pct) / 100.0f);
    if (fw > 0) fill(fb, x, y, fw, h, fill_col);
    /* Border */
    fill(fb, x, y, w, 1, C_BORDER);
    fill(fb, x, y+h-1, w, 1, C_BORDER);
    fill(fb, x, y, 1, h, C_BORDER);
    fill(fb, x+w-1, y, 1, h, C_BORDER);
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
    fill(fb, x, y, w, h, C_BAR_BG);
    fill(fb, x, y, w, 1, C_BORDER);
    fill(fb, x, y+h-1, w, 1, C_BORDER);
    fill(fb, x, y, 1, h, C_BORDER);
    fill(fb, x+w-1, y, 1, h, C_BORDER);
    /* 50% guide line */
    int mid = y + h/2;
    for (int i = x; i < x+w; i += 4) fill(fb, i, mid, 2, 1, C_BORDER);

    int n = HIST_N;
    for (int i = 0; i < n && i < w-2; i++) {
        int idx = (g_hist_head - 1 - i + n * 1000) % n;
        float v = g_cpu_hist[idx];
        if (v < 0) v = 0; if (v > 100) v = 100;
        int bar_h = (int)((float)(h-2) * v / 100.0f);
        int bx = x + w - 2 - i;
        int by = y + h - 1 - bar_h;
        uint32_t col = v > 75.0f ? C_CPU_HI : C_CPU_LO;
        if (bar_h > 0) fill(fb, bx, by, 1, bar_h, col);
    }
}

/* ── Render ──────────────────────────────────────────────────────────────── */
static void render(uint32_t *fb) {
    fill(fb, 0, 0, WIN_W, WIN_H, C_BG);
    /* Top TITLE_H blank — compositor draws title bar */

    int y = TITLE_H + 6;

    /* ── CPU section ── */
    draw_str(fb, "CPU", PAD, y, C_KEY);
    hline(fb, y + g_glyph_h + 2, C_BORDER);
    y += g_glyph_h + 5;

    /* Total CPU bar */
    char cpu_lbl[16];
    snprintf(cpu_lbl, sizeof(cpu_lbl), "Total  %3.0f%%", g_cpu_pct[0]);
    draw_str(fb, cpu_lbl, PAD, y + (BAR_H + 4 - g_glyph_h)/2, C_VAL);
    draw_bar(fb, PAD + 100, y + 2, WIN_W - PAD - 100 - PAD, BAR_H, g_cpu_pct[0],
             g_cpu_pct[0] > 75.0f ? C_CPU_HI : C_CPU_LO);
    y += BAR_H + 6;

    /* Per-core bars (up to 8 shown) */
    int show = g_ncpus < 8 ? g_ncpus : 8;
    for (int i = 0; i < show; i++) {
        char lbl[12];
        snprintf(lbl, sizeof(lbl), "Core %d %2.0f%%", i, g_cpu_pct[i+1]);
        fill(fb, 0, y, WIN_W, ROW_H - 4, (i & 1) ? C_ROW_B : C_ROW_A);
        draw_str(fb, lbl, PAD, y + (ROW_H - 4 - g_glyph_h)/2, C_GREY);
        draw_bar(fb, PAD + 100, y + (ROW_H - 4 - BAR_H)/2,
                 WIN_W - PAD - 100 - PAD, BAR_H, g_cpu_pct[i+1],
                 g_cpu_pct[i+1] > 75.0f ? C_CPU_HI : C_CPU_LO);
        y += ROW_H - 4;
    }

    /* CPU history graph */
    y += 4;
    draw_graph(fb, PAD, y, WIN_W - PAD*2, 50);
    draw_str(fb, "1min history", WIN_W - PAD - 9*9, y + 2, C_GREY);
    y += 56;

    /* ── Memory section ── */
    draw_str(fb, "Memory", PAD, y, C_KEY);
    hline(fb, y + g_glyph_h + 2, C_BORDER);
    y += g_glyph_h + 5;

    if (g_mem_total_mb > 0) {
        float mem_pct = (float)g_mem_used_mb * 100.0f / (float)g_mem_total_mb;
        char mem_lbl[32];
        snprintf(mem_lbl, sizeof(mem_lbl), "RAM   %4lu/%4lu MB", g_mem_used_mb, g_mem_total_mb);
        draw_str(fb, mem_lbl, PAD, y + (BAR_H + 4 - g_glyph_h)/2, C_VAL);
        draw_bar(fb, PAD + 128, y + 2, WIN_W - PAD - 128 - PAD, BAR_H, mem_pct, C_RAM);
        y += BAR_H + 6;

        if (g_swap_total_mb > 0) {
            float sw_pct = (float)g_swap_used_mb * 100.0f / (float)g_swap_total_mb;
            char sw_lbl[32];
            snprintf(sw_lbl, sizeof(sw_lbl), "Swap  %4lu/%4lu MB", g_swap_used_mb, g_swap_total_mb);
            draw_str(fb, sw_lbl, PAD, y + (BAR_H + 4 - g_glyph_h)/2, C_GREY);
            draw_bar(fb, PAD + 128, y + 2, WIN_W - PAD - 128 - PAD, BAR_H, sw_pct, C_CPU_HI);
            y += BAR_H + 6;
        }
    }

    /* ── System section ── */
    y += 4;
    draw_str(fb, "System", PAD, y, C_KEY);
    hline(fb, y + g_glyph_h + 2, C_BORDER);
    y += g_glyph_h + 5;

    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        long up = si.uptime;
        char buf[64];

        fill(fb, 0, y, WIN_W, ROW_H - 2, C_ROW_A);
        snprintf(buf, sizeof(buf), "Uptime   %ldh %02ldm %02lds",
                 up/3600, (up%3600)/60, up%60);
        draw_str(fb, buf, PAD, y + (ROW_H - 2 - g_glyph_h)/2, C_VAL);
        y += ROW_H - 2;

        fill(fb, 0, y, WIN_W, ROW_H - 2, C_ROW_B);
        int np = count_procs();
        snprintf(buf, sizeof(buf), "Procs    %d   Load %.2f",
                 np, (double)si.loads[0] / 65536.0);
        draw_str(fb, buf, PAD, y + (ROW_H - 2 - g_glyph_h)/2, C_VAL);
    }
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
    uint32_t total = 16 + WIN_W * WIN_H * 4;
    uint8_t *msg = malloc(total);
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

    uint32_t *fb = malloc(WIN_W * WIN_H * 4);
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

    render(fb); send_frame(sock, fb);
    fcntl(sock, F_SETFL, O_NONBLOCK);

    uint8_t ibuf[8]; int igot = 0;
    uint32_t itype = 0, iplen = 0, ipgot = 0;
    bool running = true;
    time_t last_tick = time(NULL);

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
                        memcpy(&itype,  ibuf, 4);
                        memcpy(&iplen, ibuf+4, 4);
                        if (iplen > 65536) { igot = 0; break; }
                        ipgot = 0;
                        if (iplen == 0) {
                            if (itype == IPC_INVALIDATE) {
                                update_cpu(); update_mem();
                                push_hist(g_cpu_pct[0]);
                                render(fb); send_frame(sock, fb);
                            } else if (itype == IPC_INPUT_KEY) {
                                /* key with zero len shouldn't happen, skip */
                            }
                            igot = 0;
                        }
                    }
                } else {
                    /* drain payload bytes */
                    uint32_t have = (uint32_t)(n - pos);
                    uint32_t need = iplen - ipgot;
                    uint32_t take = have < need ? have : need;
                    uint8_t key = 0;
                    if (itype == IPC_INPUT_KEY && ipgot == 0 && take > 0)
                        key = tbuf[pos];
                    pos += (ssize_t)take; ipgot += take;
                    if (ipgot >= iplen) {
                        if (itype == IPC_INPUT_KEY && key) {
                            if (key == 'q' || key == 'Q' || key == 0x1B) running = false;
                        }
                        igot = 0; itype = 0; iplen = 0; ipgot = 0;
                    }
                }
            }
        }

        /* Update every second */
        time_t now = time(NULL);
        if (now != last_tick) {
            last_tick = now;
            update_cpu();
            update_mem();
            push_hist(g_cpu_pct[0]);
            render(fb);
            send_frame(sock, fb);
        }

        struct timespec ts = {0, 50000000}; /* 20Hz poll */
        nanosleep(&ts, NULL);
    }

    ipc_send_msg(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock);
    free(fb);
    return 0;
}
