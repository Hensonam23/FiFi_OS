/* fifi-proton — Proton / Steam Play configuration panel for FiFi OS.
 * Shows XWayland, PipeWire, and Steam status; provides launch controls.
 * Build: gcc -O2 -static -Wall -o fifi-proton proton.c -s
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
#include <sys/stat.h>
#include <time.h>

/* ── IPC ─────────────────────────────────────────────────────────────────── */
#define FIFI_SOCK        "/tmp/fifi-compositor.sock"
#define IPC_APP_CONNECT  0x01u
#define IPC_APP_FRAME    0x02u
#define IPC_APP_CLOSE    0x04u
#define IPC_WIN_CREATED  0x10u
#define IPC_INPUT_KEY    0x11u
#define IPC_WIN_RESIZE   0x1Bu
#define IPC_INVALIDATE   0x15u

#define WIN_W   480
#define WIN_H   420
#define TITLE_H  24
#define PAD      14
#define ROW_H    22

static int g_win_w = WIN_W;
static int g_win_h = WIN_H;

/* ── Colours ─────────────────────────────────────────────────────────────── */
#define C_BG      0x00101820u
#define C_ROW_A   0x00141c28u
#define C_ROW_B   0x00101820u
#define C_BORDER  0x00243448u
#define C_KEY     0x0060a0c0u
#define C_VAL     0x00b0c8e0u
#define C_GREY    0x00506070u
#define C_OK      0x0040b060u
#define C_WARN    0x00c09020u
#define C_ERR     0x00c04030u
#define C_BTN     0x001e3050u
#define C_BTN_HOV 0x00285070u
#define C_SEC_HDR 0x001a2432u
#define C_ACCENT  0x003060c0u
#define C_WHITE   0x00e0e8f0u

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

static void draw_str_clip(uint32_t *fb, const char *s, int x, int y,
                          uint32_t fg, int max_px) {
    for (; *s && max_px > 9; s++, x += 9, max_px -= 9)
        draw_char(fb, (unsigned char)*s, x, y, fg);
}

static void fill(uint32_t *fb, int x, int y, int w, int h, uint32_t col) {
    for (int r = y; r < y + h; r++) {
        if (r < 0 || r >= g_win_h) continue;
        int x0 = x < 0 ? 0 : x;
        int x1 = x + w > g_win_w ? g_win_w : x + w;
        for (int c = x0; c < x1; c++) fb[r * g_win_w + c] = col;
    }
}

/* ── IPC helpers (modern 8-byte header protocol) ─────────────────────────── */
static int g_sock = -1;

static void ipc_send_msg(int fd, uint32_t type, const void *data, uint32_t len) {
    uint8_t hdr[8];
    memcpy(hdr, &type, 4); memcpy(hdr+4, &len, 4);
    send(fd, hdr, 8, 0);
    if (len && data) send(fd, data, len, 0);
}

static bool ipc_connect(void) {
    g_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_sock < 0) return false;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIFI_SOCK, sizeof(addr.sun_path) - 1);
    if (connect(g_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(g_sock); g_sock = -1; return false;
    }
    uint8_t conn[68] = {0};
    uint16_t cw = WIN_W, ch = WIN_H;
    memcpy(conn, &cw, 2); memcpy(conn+2, &ch, 2);
    snprintf((char*)(conn+4), 64, "Proton Config");
    ipc_send_msg(g_sock, IPC_APP_CONNECT, conn, sizeof(conn));
    /* Read IPC_WIN_CREATED response */
    uint8_t hdr8[8] = {0};
    recv(g_sock, hdr8, 8, 0);
    uint32_t rtype, rplen;
    memcpy(&rtype, hdr8, 4); memcpy(&rplen, hdr8+4, 4);
    if (rtype == IPC_WIN_CREATED && rplen >= 20) {
        uint8_t r[20]; recv(g_sock, r, 20, 0);
    }
    return true;
}

static void ipc_send_frame(uint32_t *fb) {
    uint32_t fw = (uint32_t)g_win_w, fh = (uint32_t)g_win_h;
    uint32_t total = 16 + fw * fh * 4;
    uint8_t *msg = malloc(total);
    if (!msg) return;
    uint32_t frm[4] = {0, 0, fw, fh};
    memcpy(msg, frm, 16);
    memcpy(msg+16, fb, fw * fh * 4);
    ipc_send_msg(g_sock, IPC_APP_FRAME, msg, total);
    free(msg);
}

/* ── Status checks ───────────────────────────────────────────────────────── */
static bool xwayland_running(void) {
    struct stat st;
    return (stat("/tmp/.X0-lock", &st) == 0);
}

static bool pipewire_running(void) {
    struct stat st;
    return (stat("/tmp/pipewire-0", &st) == 0 &&
            S_ISSOCK(st.st_mode));
}

static bool steam_installed(void) {
    struct stat st;
    return (stat("/usr/bin/steam", &st) == 0 ||
            stat("/usr/local/bin/steam", &st) == 0 ||
            stat("/bin/steam", &st) == 0);
}

/* Scan for Proton versions under Steam's tools dir */
static int count_proton_versions(void) {
    const char *paths[] = {
        "/mnt/games/Steam/steamapps/common",
        "/mnt/wingames/SteamLibrary/steamapps/common",
        "/root/.local/share/Steam/steamapps/common",
        NULL,
    };
    int count = 0;
    for (int pi = 0; paths[pi]; pi++) {
        DIR *d = opendir(paths[pi]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strncmp(e->d_name, "Proton", 6) == 0 ||
                strncmp(e->d_name, "GE-Proton", 9) == 0)
                count++;
        }
        closedir(d);
    }
    return count;
}

/* ── Button state ────────────────────────────────────────────────────────── */
typedef struct { int x, y, w, h; const char *label; } btn_t;
#define MAX_BTNS 4
static btn_t g_btns[MAX_BTNS];
static int   g_nhover = -1;
static int   g_nbtns  = 0;

static void btn_add(int x, int y, int w, int h, const char *label) {
    if (g_nbtns >= MAX_BTNS) return;
    g_btns[g_nbtns++] = (btn_t){ x, y, w, h, label };
}

static void draw_btn(uint32_t *fb, int idx, bool hover) {
    btn_t *b = &g_btns[idx];
    uint32_t bg = hover ? C_BTN_HOV : C_BTN;
    fill(fb, b->x, b->y, b->w, b->h, bg);
    fill(fb, b->x, b->y, b->w, 1, C_ACCENT);
    fill(fb, b->x, b->y + b->h - 1, b->w, 1, C_BORDER);
    int lw = (int)strlen(b->label) * 9;
    int tx = b->x + (b->w > lw ? (b->w - lw) / 2 : 4);
    int ty = b->y + (b->h - g_glyph_h) / 2;
    draw_str_clip(fb, b->label, tx, ty, C_WHITE, b->w - 4);
}

/* ── Render ──────────────────────────────────────────────────────────────── */
static uint32_t *g_fb = NULL;

static void section(int y, const char *title) {
    fill(g_fb, PAD, y, g_win_w - PAD * 2, g_glyph_h + 4, C_SEC_HDR);
    fill(g_fb, PAD, y + g_glyph_h + 4, g_win_w - PAD * 2, 1, C_ACCENT);
    draw_str(g_fb, title, PAD + 6, y + 2, C_ACCENT);
}

/* Longest key is "XWayland  (X11 compat)" = 22 chars = 198px; value starts after + gap */
#define ROW_VAL_X  (PAD + 8 + 22*9 + 8)   /* 14+8+198+8 = 228 */

static void row_kv(int y, const char *key, const char *val, uint32_t vcol) {
    uint32_t bg = (y / (ROW_H)) % 2 == 0 ? C_ROW_A : C_ROW_B;
    fill(g_fb, PAD, y, g_win_w - PAD * 2, ROW_H, bg);
    int ty = y + (ROW_H - g_glyph_h) / 2;
    int key_max = ROW_VAL_X - PAD - 8 - 4;   /* pixels available for key */
    draw_str_clip(g_fb, key, PAD + 8, ty, C_KEY, key_max);
    int val_max = g_win_w - ROW_VAL_X - PAD;
    draw_str_clip(g_fb, val, ROW_VAL_X, ty, vcol, val_max);
}

static void render(void) {
    if (!g_fb) return;
    fill(g_fb, 0, 0, g_win_w, g_win_h, C_BG);
    fill(g_fb, 0, 0, g_win_w, 1, C_ACCENT);
    fill(g_fb, 0, 0, 1, g_win_h, C_ACCENT);
    fill(g_fb, g_win_w - 1, 0, 1, g_win_h, C_ACCENT);
    fill(g_fb, 0, g_win_h - 1, g_win_w, 1, C_ACCENT);

    int y = TITLE_H + PAD;
    draw_str(g_fb, "Proton Gaming Setup", PAD + 4, y, C_WHITE);
    y += g_glyph_h + 10;

    /* ── System prerequisites ── */
    section(y, "System Prerequisites"); y += g_glyph_h + 6;

    bool xwl = xwayland_running();
    bool pw  = pipewire_running();
    row_kv(y, "XWayland  (X11 compat)", xwl ? "Running" : "Not started", xwl ? C_OK : C_WARN);
    y += ROW_H;
    row_kv(y, "PipeWire  (audio mix)", pw ? "Running" : "Not started", pw ? C_OK : C_WARN);
    y += ROW_H + 4;

    /* ── Steam ── */
    section(y, "Steam"); y += g_glyph_h + 6;
    bool stm = steam_installed();
    row_kv(y, "Steam binary", stm ? "Found" : "Not installed", stm ? C_OK : C_ERR);
    y += ROW_H;
    int pv = count_proton_versions();
    char pvstr[32];
    if (pv > 0) snprintf(pvstr, sizeof(pvstr), "%d version%s found", pv, pv == 1 ? "" : "s");
    else        snprintf(pvstr, sizeof(pvstr), "None installed");
    row_kv(y, "Proton versions", pvstr, pv > 0 ? C_OK : C_WARN);
    y += ROW_H + 4;

    /* ── Launch ── */
    section(y, "Launch"); y += g_glyph_h + 8;
    g_nbtns = 0;
    int bh = 28;
    int bw2 = (g_win_w - PAD * 2 - 12) / 2;  /* two equal-width buttons per row */
    int bx0 = PAD + 4;
    if (stm) {
        btn_add(bx0, y, bw2, bh, "Launch Steam");
        y += bh + 8;
    }
    btn_add(bx0,        y, bw2, bh, "Restart PipeWire");
    btn_add(bx0 + bw2 + 12, y, bw2, bh, "Restart XWayland");
    for (int i = 0; i < g_nbtns; i++) draw_btn(g_fb, i, i == g_nhover);
    y += bh + 8;

    /* ── Notes ── */
    y += 8;
    fill(g_fb, PAD, y, g_win_w - PAD * 2, 1, C_BORDER); y += 4;
    int note_max = g_win_w - PAD - 4 - PAD;
    draw_str_clip(g_fb, "Steam runs via XWayland. Use DISPLAY=:0 for X11 apps.", PAD + 4, y, C_GREY, note_max);
    y += g_glyph_h + 2;
    draw_str_clip(g_fb, "Proton: enable Steam Play in Steam > Settings > Compatibility.", PAD + 4, y, C_GREY, note_max);
    y += g_glyph_h + 2;
    draw_str_clip(g_fb, "Vulkan: RADV (AMD) and nvidia-open work natively on FiFi.", PAD + 4, y, C_GREY, note_max);
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void) {
    signal(SIGPIPE, SIG_IGN);

    /* Load font */
    const char *fonts[] = {
        "/fifi-data/fonts/ter16b.psf",
        "/fifi-data/fonts/ter16n.psf",
        NULL,
    };
    for (int i = 0; fonts[i]; i++) if (font_load(fonts[i])) break;

    g_fb = calloc((size_t)(WIN_W * WIN_H), 4);
    if (!g_fb) return 1;

    if (!ipc_connect()) { free(g_fb); return 1; }

    render();
    ipc_send_frame(g_fb);

    /* Event loop — modern 8-byte header protocol */
    uint8_t ibuf[8]; int igot = 0;
    uint32_t itype = 0, iplen = 0, ipgot = 0;
    uint8_t payload[64] = {0};
    bool running = true;

    while (running) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(g_sock, &rfds);
        struct timeval tv = {0, 50000};
        if (select(g_sock+1, &rfds, NULL, NULL, &tv) < 0) break;
        if (!FD_ISSET(g_sock, &rfds)) continue;

        uint8_t tbuf[512];
        ssize_t n = read(g_sock, tbuf, sizeof(tbuf));
        if (n <= 0) break;

        ssize_t pos = 0;
        while (pos < n) {
            if (igot < 8) {
                ibuf[igot++] = tbuf[pos++];
                if (igot == 8) {
                    memcpy(&itype, ibuf, 4);
                    memcpy(&iplen, ibuf+4, 4);
                    if (iplen > 65536) { igot = 0; break; }
                    ipgot = 0;
                    memset(payload, 0, sizeof(payload));
                    if (iplen == 0) {
                        if (itype == IPC_INVALIDATE) {
                            render(); ipc_send_frame(g_fb);
                        } else if (itype == IPC_APP_CLOSE) {
                            running = false;
                        }
                        igot = 0;
                    }
                }
            } else {
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
                            uint8_t ch = payload[0];
                            if (ch == 27 || ch == 'q' || ch == 'Q') running = false;
                            else if (ch == 'r' || ch == 'R') { render(); ipc_send_frame(g_fb); }
                        }
                        break;
                    case IPC_WIN_RESIZE:
                        if (iplen >= 4) {
                            uint16_t nw, nh;
                            memcpy(&nw, payload, 2); memcpy(&nh, payload+2, 2);
                            if (nw >= 300 && nh >= 200) {
                                uint32_t *nb = realloc(g_fb, (size_t)nw * nh * 4);
                                if (nb) { g_fb = nb; g_win_w = nw; g_win_h = nh; }
                            }
                        }
                        render(); ipc_send_frame(g_fb);
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

    ipc_send_msg(g_sock, IPC_APP_CLOSE, NULL, 0);
    free(g_fb);
    close(g_sock);
    return 0;
}
