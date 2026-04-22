/* FiFi File Browser — standalone IPC process.
 * Connects to the FiFi compositor, creates a window, and renders a file
 * browser UI using pixel frames over the IPC socket protocol.
 *
 * Input: IPC_INPUT_KEY (arrow keys, enter, escape, backspace)
 *        IPC_INPUT_MOUSE (click on entry)
 * Build: gcc -O2 -static -o fifi-filebrowser filebrowser.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <time.h>

/* ── IPC protocol ────────────────────────────────────────────────────────── */
#define FIFI_SOCK         "/tmp/fifi-compositor.sock"
#define IPC_APP_CONNECT   0x01u
#define IPC_APP_FRAME     0x02u
#define IPC_APP_CLOSE     0x04u
#define IPC_WIN_CREATED   0x10u
#define IPC_INPUT_KEY     0x11u
#define IPC_INPUT_MOUSE   0x12u

/* ── Window geometry ─────────────────────────────────────────────────────── */
#define WIN_W   640
#define WIN_H   480
#define HDR_H   36    /* path bar */
#define FOOT_H  24    /* status bar */
#define ITEM_H  22    /* height per entry row */
#define PAD_X   12

/* ── Colours (ARGB / 0x00RRGGBB) ────────────────────────────────────────── */
#define C_BG        0x00121820u
#define C_HDR_BG    0x001a2432u
#define C_FOOT_BG   0x001a2432u
#define C_SEL       0x00204060u
#define C_DIR       0x006090d8u
#define C_FILE      0x00b0c8e0u
#define C_GREY      0x00506070u
#define C_BORDER    0x00243448u
#define C_WHITE     0x00f0f0f0u

/* ── PSF1 font ───────────────────────────────────────────────────────────── */
#define PSF1_MAGIC 0x0436u

typedef struct {
    uint16_t magic;
    uint8_t  mode;
    uint8_t  charsize;  /* bytes per glyph = height; width always 8 */
} Psf1Hdr;

static uint8_t *g_glyph  = NULL;
static int      g_glyph_h = 16;
static int      g_n_glyphs = 256;

static bool font_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    Psf1Hdr hdr;
    if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) { close(fd); return false; }
    if (hdr.magic != PSF1_MAGIC) { close(fd); return false; }
    g_glyph_h  = hdr.charsize;
    g_n_glyphs = (hdr.mode & 1) ? 512 : 256;
    int total  = g_n_glyphs * g_glyph_h;
    g_glyph = malloc(total);
    if (!g_glyph) { close(fd); return false; }
    if (read(fd, g_glyph, total) < total) { free(g_glyph); g_glyph = NULL; close(fd); return false; }
    close(fd);
    return true;
}

static void font_draw_char(uint32_t *fb, int fw, int c,
                           int px, int py, uint32_t fg) {
    if (!g_glyph || c < 0 || c >= g_n_glyphs) return;
    const uint8_t *bits = g_glyph + c * g_glyph_h;
    for (int row = 0; row < g_glyph_h; row++) {
        uint8_t b = bits[row];
        for (int col = 0; col < 8; col++) {
            if (b & (0x80u >> col)) {
                int x = px + col, y = py + row;
                if (x >= 0 && x < fw && y >= 0 && y < WIN_H)
                    fb[y * WIN_W + x] = fg;
            }
        }
    }
}

static void font_draw_str(uint32_t *fb, const char *s,
                          int x, int y, uint32_t fg) {
    for (; *s; s++, x += 9)
        font_draw_char(fb, WIN_W, (unsigned char)*s, x, y, fg);
}

static void font_draw_strn(uint32_t *fb, const char *s, int n,
                           int x, int y, uint32_t fg) {
    for (int i = 0; i < n && s[i]; i++, x += 9)
        font_draw_char(fb, WIN_W, (unsigned char)s[i], x, y, fg);
}

static int font_strw(const char *s) { return (int)strlen(s) * 9; }

/* ── Rect fill ───────────────────────────────────────────────────────────── */
static void fill_rect(uint32_t *fb, int x, int y, int w, int h, uint32_t col) {
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= WIN_H) continue;
        int x0 = x < 0 ? 0 : x;
        int x1 = x + w > WIN_W ? WIN_W : x + w;
        for (int col2 = x0; col2 < x1; col2++)
            fb[row * WIN_W + col2] = col;
    }
}

static void draw_hline(uint32_t *fb, int y, uint32_t col) {
    if (y < 0 || y >= WIN_H) return;
    for (int x = 0; x < WIN_W; x++) fb[y * WIN_W + x] = col;
}

/* ── Directory listing ───────────────────────────────────────────────────── */
#define MAX_ENTRIES 512
#define NAME_MAX_DISP 56

typedef struct {
    char  name[256];
    bool  is_dir;
} Entry;

static Entry   g_entries[MAX_ENTRIES];
static int     g_nentries = 0;
static int     g_selected = 0;
static int     g_scroll   = 0;
static char    g_path[1024] = "/fifi-data";

static int entry_cmp(const void *a, const void *b) {
    const Entry *ea = (const Entry *)a;
    const Entry *eb = (const Entry *)b;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1;
    return strcmp(ea->name, eb->name);
}

static void load_dir(const char *path) {
    g_nentries = 0;
    g_selected = 0;
    g_scroll   = 0;
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && g_nentries < MAX_ENTRIES) {
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' ||
             (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        Entry *e = &g_entries[g_nentries];
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        /* stat to check type */
        char full[1280];
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        struct stat st;
        e->is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
        g_nentries++;
    }
    closedir(d);
    qsort(g_entries, g_nentries, sizeof(Entry), entry_cmp);
}

/* ── Rendering ───────────────────────────────────────────────────────────── */
static void render(uint32_t *fb) {
    /* Background */
    fill_rect(fb, 0, 0, WIN_W, WIN_H, C_BG);

    /* Header bar */
    fill_rect(fb, 0, 0, WIN_W, HDR_H, C_HDR_BG);
    draw_hline(fb, HDR_H - 1, C_BORDER);
    const char *label = "  Files: ";
    int lx = PAD_X;
    font_draw_str(fb, label, lx, (HDR_H - g_glyph_h) / 2, C_GREY);
    lx += font_strw(label);
    /* Clamp path to available width */
    int path_chars = (WIN_W - lx - PAD_X) / 9;
    const char *p = g_path;
    int plen = (int)strlen(p);
    if (plen > path_chars) p += (plen - path_chars);
    font_draw_str(fb, p, lx, (HDR_H - g_glyph_h) / 2, C_WHITE);

    /* Entry list */
    int list_top = HDR_H;
    int list_bot = WIN_H - FOOT_H;
    int visible  = (list_bot - list_top) / ITEM_H;

    for (int i = 0; i < visible; i++) {
        int idx = g_scroll + i;
        if (idx >= g_nentries) break;
        int ry = list_top + i * ITEM_H;
        bool sel = (idx == g_selected);

        if (sel) fill_rect(fb, 0, ry, WIN_W, ITEM_H, C_SEL);

        /* icon */
        const char *icon = g_entries[idx].is_dir ? ">" : " ";
        font_draw_str(fb, icon, PAD_X, ry + (ITEM_H - g_glyph_h) / 2,
                      g_entries[idx].is_dir ? C_DIR : C_GREY);

        /* name (truncate if needed) */
        const char *name = g_entries[idx].name;
        int namelen = (int)strlen(name);
        uint32_t fg = g_entries[idx].is_dir ? C_DIR : C_FILE;
        int max_chars = (WIN_W - PAD_X - 16 - PAD_X) / 9;
        int draw_chars = namelen < max_chars ? namelen : max_chars;
        font_draw_strn(fb, name, draw_chars,
                       PAD_X + 16, ry + (ITEM_H - g_glyph_h) / 2, fg);

        /* row divider */
        draw_hline(fb, ry + ITEM_H - 1, C_BORDER);
    }

    /* Footer */
    fill_rect(fb, 0, list_bot, WIN_W, FOOT_H, C_FOOT_BG);
    draw_hline(fb, list_bot, C_BORDER);
    char foot[128];
    snprintf(foot, sizeof(foot), "  %d items  |  arrows: navigate  enter: open  esc: up  q: quit",
             g_nentries);
    font_draw_str(fb, foot, 0, list_bot + (FOOT_H - g_glyph_h) / 2, C_GREY);

    /* Outer border */
    for (int x = 0; x < WIN_W; x++) { fb[x] = C_BORDER; fb[(WIN_H-1)*WIN_W+x] = C_BORDER; }
    for (int y = 0; y < WIN_H; y++) { fb[y*WIN_W] = C_BORDER; fb[y*WIN_W+WIN_W-1] = C_BORDER; }
}

/* ── IPC helpers ─────────────────────────────────────────────────────────── */
static void ipc_send_msg(int fd, uint32_t type, const void *data, uint32_t len) {
    uint8_t hdr[8];
    memcpy(hdr,     &type, 4);
    memcpy(hdr + 4, &len,  4);
    write(fd, hdr, 8);
    if (len > 0 && data) write(fd, data, len);
}

static void send_frame(int fd, uint32_t *pixels) {
    uint32_t frm[4] = {0, 0, WIN_W, WIN_H};
    uint32_t total  = 16 + WIN_W * WIN_H * 4;
    uint8_t *msg    = malloc(total);
    if (!msg) return;
    memcpy(msg,      frm,    16);
    memcpy(msg + 16, pixels, WIN_W * WIN_H * 4);
    ipc_send_msg(fd, IPC_APP_FRAME, msg, total);
    free(msg);
}

/* ── Navigation helpers ──────────────────────────────────────────────────── */
#define KEY_UP    0x48   /* custom — compositor maps up arrow */
#define KEY_DOWN  0x50
#define KEY_ENTER 0x0D
#define KEY_ESC   0x1B
#define KEY_BS    0x08
#define KEY_q     'q'

static int g_visible(void) {
    return (WIN_H - HDR_H - FOOT_H) / ITEM_H;
}

static void clamp_scroll(void) {
    int vis = g_visible();
    if (g_selected < g_scroll) g_scroll = g_selected;
    if (g_selected >= g_scroll + vis) g_scroll = g_selected - vis + 1;
    if (g_scroll < 0) g_scroll = 0;
}

static void nav_enter(int fd, uint32_t *fb) {
    if (g_nentries == 0) return;
    Entry *e = &g_entries[g_selected];
    if (e->is_dir) {
        char newpath[1280];
        snprintf(newpath, sizeof(newpath), "%s/%s", g_path, e->name);
        snprintf(g_path, sizeof(g_path), "%s", newpath);
        load_dir(g_path);
        render(fb);
        send_frame(fd, fb);
    }
    /* Files: no viewer in standalone filebrowser — future work */
}

static void nav_up(int fd, uint32_t *fb) {
    /* Strip last path component */
    char *slash = strrchr(g_path, '/');
    if (!slash || slash == g_path) return;
    *slash = '\0';
    if (g_path[0] == '\0') { g_path[0] = '/'; g_path[1] = '\0'; }
    load_dir(g_path);
    render(fb);
    send_frame(fd, fb);
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void) {
    /* Load font */
    if (!font_load("/fifi-data/fonts/ter16b.psf")) {
        /* fallback: no font — just show empty window */
        g_glyph = calloc(256 * 16, 1);
        g_glyph_h = 16;
    }

    /* Load initial directory */
    load_dir(g_path);

    /* Allocate pixel buffer */
    uint32_t *fb = malloc(WIN_W * WIN_H * 4);
    if (!fb) return 1;

    /* Connect to compositor */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIFI_SOCK, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); return 1;
    }

    /* Register window */
    uint8_t conn[68] = {0};
    uint16_t w = WIN_W, h = WIN_H;
    memcpy(conn,     &w, 2);
    memcpy(conn + 2, &h, 2);
    snprintf((char *)(conn + 4), 64, "File Browser");
    ipc_send_msg(sock, IPC_APP_CONNECT, conn, sizeof(conn));

    /* Wait for WIN_CREATED */
    uint8_t hdr[8] = {0};
    read(sock, hdr, 8);
    uint32_t type, plen;
    memcpy(&type, hdr,     4);
    memcpy(&plen, hdr + 4, 4);
    if (type == IPC_WIN_CREATED && plen >= 20) {
        uint8_t resp[20]; read(sock, resp, 20);
    }

    /* Initial render */
    render(fb);
    send_frame(sock, fb);

    /* Set socket non-blocking */
    fcntl(sock, F_SETFL, O_NONBLOCK);

    /* Event loop */
    uint8_t in_hdr[8];
    int     in_got = 0;
    uint8_t *in_pld = NULL;
    uint32_t in_plen = 0, in_pgot = 0;
    bool     dirty = false;
    bool     running = true;

    /* Mouse tracking */
    bool prev_lbtn = false;

    while (running) {
        /* Poll for incoming messages */
        uint8_t tbuf[256];
        ssize_t n = read(sock, tbuf, sizeof(tbuf));
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
        if (n == 0) break;  /* compositor closed */

        if (n > 0) {
            /* Parse incoming bytes */
            ssize_t pos = 0;
            while (pos < n) {
                if (in_got < 8) {
                    in_hdr[in_got++] = tbuf[pos++];
                    if (in_got == 8) {
                        memcpy(&type,   in_hdr,     4);
                        memcpy(&in_plen, in_hdr + 4, 4);
                        if (in_plen > 65536) { in_got = 0; break; }
                        if (in_plen > 0) {
                            in_pld  = malloc(in_plen);
                            in_pgot = 0;
                        }
                    }
                } else if (in_plen > 0 && in_pgot < in_plen) {
                    uint32_t need = in_plen - in_pgot;
                    uint32_t have = (uint32_t)(n - pos);
                    uint32_t take = need < have ? need : have;
                    if (in_pld) memcpy(in_pld + in_pgot, tbuf + pos, take);
                    in_pgot += take;
                    pos     += take;
                    if (in_pgot >= in_plen) {
                        /* Full message received */
                        if (type == IPC_INPUT_KEY && in_plen >= 1) {
                            uint8_t key = in_pld ? in_pld[0] : 0;
                            bool redraw = false;
                            if (key == KEY_UP || key == 'A') {   /* up arrow via ANSI */
                                if (g_selected > 0) { g_selected--; clamp_scroll(); redraw = true; }
                            } else if (key == KEY_DOWN || key == 'B') {
                                if (g_selected < g_nentries - 1) { g_selected++; clamp_scroll(); redraw = true; }
                            } else if (key == KEY_ENTER || key == '\r' || key == '\n') {
                                nav_enter(sock, fb);
                                redraw = false; /* already rendered */
                            } else if (key == KEY_ESC || key == KEY_BS) {
                                nav_up(sock, fb);
                                redraw = false;
                            } else if (key == KEY_q || key == 'Q') {
                                running = false;
                            }
                            if (redraw) { dirty = true; }
                        } else if (type == IPC_INPUT_MOUSE && in_plen >= 9) {
                            int32_t mx, my; uint8_t btns;
                            memcpy(&mx, in_pld,     4);
                            memcpy(&my, in_pld + 4, 4);
                            btns = in_pld[8];
                            bool lbtn = (btns & 1);
                            if (lbtn && !prev_lbtn) {
                                /* Click: find which row */
                                int list_top = HDR_H;
                                if (my >= list_top && my < WIN_H - FOOT_H) {
                                    int row = (my - list_top) / ITEM_H;
                                    int idx = g_scroll + row;
                                    if (idx < g_nentries) {
                                        if (idx == g_selected) {
                                            /* double-click equivalent: open */
                                            nav_enter(sock, fb);
                                        } else {
                                            g_selected = idx;
                                            clamp_scroll();
                                            dirty = true;
                                        }
                                    }
                                }
                            }
                            prev_lbtn = lbtn;
                        }
                        free(in_pld); in_pld = NULL;
                        in_got = 0; in_plen = 0; in_pgot = 0;
                    }
                } else {
                    /* Zero-length message fully received */
                    if (type == IPC_WIN_CREATED) { /* already handled */ }
                    in_got = 0; in_plen = 0; in_pgot = 0;
                }
            }
        }

        if (dirty) {
            render(fb);
            send_frame(sock, fb);
            dirty = false;
        }

        /* Brief sleep to avoid spinning at 100% CPU */
        struct timespec ts = {0, 8000000}; /* 8ms ≈ 125Hz */
        nanosleep(&ts, NULL);
    }

    ipc_send_msg(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock);
    free(fb);
    free(g_glyph);
    return 0;
}
