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
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <time.h>

#include "../fifi_u8.h"

/* ── IPC protocol ────────────────────────────────────────────────────────── */
#define FIFI_SOCK         "/tmp/fifi-compositor.sock"
#define IPC_APP_CONNECT   0x01u
#define IPC_APP_FRAME     0x02u
#define IPC_APP_CLOSE     0x04u
#define IPC_APP_TITLE     0x03u
#define IPC_WIN_CREATED   0x10u
#define IPC_INPUT_KEY     0x11u
#define IPC_INPUT_MOUSE   0x12u
#define IPC_INVALIDATE    0x15u
#define IPC_CLIP_SET      0x17u
#define IPC_CLIP_GET      0x18u
#define IPC_CLIP_DATA     0x19u
#define IPC_NOTIFY        0x16u
#define IPC_OPEN_FILE     0x1Au
#define IPC_WIN_RESIZE    0x1Bu
#define IPC_DRAG_START    0x1Cu
#define IPC_DROP_FILE     0x1Du

/* ── Window geometry ─────────────────────────────────────────────────────── */
/* Current framebuffer size. Updated on IPC_WIN_RESIZE so the listing re-renders
 * crisply at the window's real pixel size (more rows) instead of the compositor
 * upscaling a fixed 640×480 frame. Starts at the initial window size. */
static int g_w = 640;
static int g_h = 480;
#define TITLE_H  24   /* reserved for compositor title bar (drawn by compositor) */
#define HDR_H    32   /* path bar below the title */
#define FOOT_H   22   /* status bar */
#define ITEM_H   20   /* height per entry row */
#define PAD_X    12

/* ── Colours (0x00RRGGBB — shared FiFi design language) ──────────────────── */
#define C_BG        0x000E1620u   /* window background */
#define C_HDR_BG    0x001A2740u   /* path/toolbar bar */
#define C_FOOT_BG   0x0016202Eu   /* status bar (card tone) */
#define C_SEL       0x00409CFFu   /* selection — accent */
#define C_FIELD     0x00203450u   /* inline rename field */
#define C_DIR       0x00409CFFu   /* directories — accent */
#define C_FILE      0x00D8E8F8u   /* files — primary text */
#define C_GREY      0x006A8098u   /* muted / secondary */
#define C_BORDER    0x00243448u   /* subtle divider */
#define C_WHITE     0x00FFFFFFu   /* on-accent / bright */
#define C_WARN      0x00E05050u

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
                if (x >= 0 && x < fw && y >= 0 && y < g_h)
                    fb[y * g_w + x] = fg;
            }
        }
    }
}

/* UTF-8 aware: decode each codepoint and fold it to a byte the bitmap font can
 * show (dashes/curly quotes/accents → ASCII), drawing one cell per codepoint so
 * filenames with non-ASCII characters render sensibly instead of garbage. */
static void font_draw_str(uint32_t *fb, const char *s,
                          int x, int y, uint32_t fg) {
    for (size_t i = 0; s[i]; ) {
        uint32_t cp = fifi_u8_next(s, &i);
        int c = fifi_fold_ascii(cp);
        if (c == 0) continue;                 /* zero-width: skip */
        font_draw_char(fb, g_w, c, x, y, fg);
        x += 9;
    }
}

/* Same, bounded to the first n bytes of s (used for truncated filenames and the
 * inline rename field). n is a byte budget; each codepoint still draws one cell. */
static void font_draw_strn(uint32_t *fb, const char *s, int n,
                           int x, int y, uint32_t fg) {
    for (size_t i = 0; (int)i < n && s[i]; ) {
        uint32_t cp = fifi_u8_next(s, &i);
        int c = fifi_fold_ascii(cp);
        if (c == 0) continue;                 /* zero-width: skip */
        font_draw_char(fb, g_w, c, x, y, fg);
        x += 9;
    }
}

/* ── Rect fill ───────────────────────────────────────────────────────────── */
static void fill_rect(uint32_t *fb, int x, int y, int w, int h, uint32_t col) {
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= g_h) continue;
        int x0 = x < 0 ? 0 : x;
        int x1 = x + w > g_w ? g_w : x + w;
        for (int col2 = x0; col2 < x1; col2++)
            fb[row * g_w + col2] = col;
    }
}

static void draw_hline(uint32_t *fb, int y, uint32_t col) {
    if (y < 0 || y >= g_h) return;
    for (int x = 0; x < g_w; x++) fb[y * g_w + x] = col;
}

/* Filled rect with softened (notched) corners — reads as a rounded highlight. */
static void fill_round(uint32_t *fb, int x, int y, int w, int h,
                       uint32_t col, uint32_t bg) {
    fill_rect(fb, x, y, w, h, col);
    const int r = 3;
    for (int i = 0; i < r; i++)
        for (int j = 0; j < r; j++)
            if (i + j < r) {
                if (x + i         >= 0 && x + i         < g_w) { if (y+j>=0&&y+j<g_h) fb[(y+j)*g_w+x+i]=bg; if (y+h-1-j>=0&&y+h-1-j<g_h) fb[(y+h-1-j)*g_w+x+i]=bg; }
                if (x + w - 1 - i >= 0 && x + w - 1 - i < g_w) { if (y+j>=0&&y+j<g_h) fb[(y+j)*g_w+x+w-1-i]=bg; if (y+h-1-j>=0&&y+h-1-j<g_h) fb[(y+h-1-j)*g_w+x+w-1-i]=bg; }
            }
}

/* ── Directory listing ───────────────────────────────────────────────────── */
#define MAX_ENTRIES 512

typedef struct {
    char  name[256];
    bool  is_dir;
} Entry;

static Entry   g_entries[MAX_ENTRIES];
static int     g_nentries = 0;
static int     g_selected = 0;
static int     g_scroll   = 0;
static char    g_path[1024] = "/fifi-data";
static bool    g_confirm_delete = false;
static bool    g_renaming = false;
static char    g_rename_buf[256];
static int     g_rename_len = 0;
/* File drag state */
static bool    g_drag_pending = false;
static int32_t g_drag_smx = 0, g_drag_smy = 0;  /* mouse pos at drag start */
static char    g_drag_path[1280] = {0};
#define DRAG_THRESH 6

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
    /* Full background */
    fill_rect(fb, 0, 0, g_w, g_h, C_BG);
    /* Top TITLE_H px left dark — compositor draws the title bar there */

    /* Path bar */
    int hdr_y = TITLE_H;
    fill_rect(fb, 0, hdr_y, g_w, HDR_H, C_HDR_BG);
    draw_hline(fb, hdr_y + HDR_H - 1, C_BORDER);
    int htext_y = hdr_y + (HDR_H - g_glyph_h) / 2;
    /* Accent location marker */
    fill_round(fb, PAD_X, htext_y + 1, 6, g_glyph_h - 2, C_DIR, C_HDR_BG);
    int lx = PAD_X + 14;
    int path_chars = (g_w - lx - PAD_X) / 9;
    const char *p = g_path;
    int plen = (int)strlen(p);
    if (plen > path_chars) p += (plen - path_chars);
    font_draw_str(fb, p, lx, htext_y, C_WHITE);

    /* Entry list (origin must match mouse hit-test: TITLE_H + HDR_H) */
    int list_top = TITLE_H + HDR_H;
    int list_bot = g_h - FOOT_H;
    int visible  = (list_bot - list_top) / ITEM_H;

    for (int i = 0; i < visible; i++) {
        int idx = g_scroll + i;
        if (idx >= g_nentries) break;
        int ry = list_top + i * ITEM_H;
        bool sel = (idx == g_selected);

        /* Rounded selection highlight, inset from the edges */
        if (sel && !g_renaming)
            fill_round(fb, 6, ry + 1, g_w - 12, ITEM_H - 2, C_SEL, C_BG);

        /* When renaming this row, show inline input field */
        if (sel && g_renaming) {
            fill_round(fb, PAD_X + 14, ry + 1, g_w - PAD_X - 14 - PAD_X, ITEM_H - 2,
                       C_FIELD, C_BG);
            int max_ch = (g_w - PAD_X - 14 - PAD_X - 4) / 9;
            int draw_from = g_rename_len > max_ch ? g_rename_len - max_ch : 0;
            int draw_n    = g_rename_len - draw_from;
            font_draw_strn(fb, g_rename_buf + draw_from, draw_n,
                           PAD_X + 16, ry + (ITEM_H - g_glyph_h) / 2, C_WHITE);
            /* Cursor bar */
            int cx = PAD_X + 16 + draw_n * 9;
            fill_rect(fb, cx, ry + 3, 2, ITEM_H - 6, C_DIR);
        } else {
            /* Selected rows draw on the accent bar → use on-accent white */
            const char *icon = g_entries[idx].is_dir ? ">" : " ";
            uint32_t icon_fg = sel ? C_WHITE : (g_entries[idx].is_dir ? C_DIR : C_GREY);
            font_draw_str(fb, icon, PAD_X + 4, ry + (ITEM_H - g_glyph_h) / 2, icon_fg);

            const char *name = g_entries[idx].name;
            int namelen = (int)strlen(name);
            uint32_t fg = sel ? C_WHITE : (g_entries[idx].is_dir ? C_DIR : C_FILE);
            int max_chars = (g_w - PAD_X - 20 - PAD_X) / 9;
            int draw_chars = namelen < max_chars ? namelen : max_chars;
            font_draw_strn(fb, name, draw_chars,
                           PAD_X + 20, ry + (ITEM_H - g_glyph_h) / 2, fg);
        }
    }

    /* Footer */
    fill_rect(fb, 0, list_bot, g_w, FOOT_H, C_FOOT_BG);
    draw_hline(fb, list_bot, C_BORDER);
    int foot_y = list_bot + (FOOT_H - g_glyph_h) / 2;
    if (g_renaming) {
        font_draw_str(fb, "  Rename: Enter to confirm, Esc to cancel",
                      0, foot_y, C_WHITE);
    } else if (g_confirm_delete) {
        font_draw_str(fb, "  CONFIRM DELETE: press d again  (any other key cancels)",
                      0, foot_y, C_WARN);
    } else {
        char foot[128];
        snprintf(foot, sizeof(foot), "  %d items  F5:rename  d:del  n:mkdir  r:reload  c:copy  q:quit",
                 g_nentries);
        font_draw_str(fb, foot, 0, foot_y, C_GREY);
    }
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
    uint32_t frm[4] = {0, 0, g_w, g_h};
    uint32_t total  = 16 + g_w * g_h * 4;
    uint8_t *msg    = malloc(total);
    if (!msg) return;
    memcpy(msg,      frm,    16);
    memcpy(msg + 16, pixels, g_w * g_h * 4);
    ipc_send_msg(fd, IPC_APP_FRAME, msg, total);
    free(msg);
}

/* ── Navigation helpers ──────────────────────────────────────────────────── */
#define KEY_UP    0x48   /* custom — compositor maps up arrow */
#define KEY_DOWN  0x50
#define KEY_ENTER 0x0D
#define KEY_ESC   0x1B
#define KEY_BS    0x08
#define KEY_F5    0x8Eu  /* FIFI_KEY_F5 — rename */
#define KEY_q     'q'

static int g_visible(void) {
    return (g_h - TITLE_H - HDR_H - FOOT_H) / ITEM_H;
}

static void clamp_scroll(void) {
    int vis = g_visible();
    if (g_selected < g_scroll) g_scroll = g_selected;
    if (g_selected >= g_scroll + vis) g_scroll = g_selected - vis + 1;
    if (g_scroll < 0) g_scroll = 0;
}

static void update_title(int fd) {
    const char *name = strrchr(g_path, '/');
    name = name ? name + 1 : g_path;
    if (!*name) name = "/";
    char title[68];
    snprintf(title, sizeof(title), "Files: %s", name);
    ipc_send_msg(fd, IPC_APP_TITLE, title, (uint32_t)strlen(title));
}

static void nav_enter(int fd, uint32_t *fb) {
    if (g_nentries == 0) return;
    Entry *e = &g_entries[g_selected];
    if (e->is_dir) {
        char newpath[1280];
        snprintf(newpath, sizeof(newpath), "%s/%s", g_path, e->name);
        snprintf(g_path, sizeof(g_path), "%s", newpath);
        load_dir(g_path);
        update_title(fd);
        render(fb);
        send_frame(fd, fb);
    } else {
        char fullpath[1280];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", g_path, e->name);
        ipc_send_msg(fd, IPC_OPEN_FILE, fullpath, (uint32_t)strlen(fullpath));
    }
}

static void nav_up(int fd, uint32_t *fb) {
    /* Strip last path component */
    char *slash = strrchr(g_path, '/');
    if (!slash || slash == g_path) return;
    *slash = '\0';
    if (g_path[0] == '\0') { g_path[0] = '/'; g_path[1] = '\0'; }
    load_dir(g_path);
    update_title(fd);
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
    uint32_t *fb = malloc(g_w * g_h * 4);
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
    uint16_t w = g_w, h = g_h;
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

    signal(SIGPIPE, SIG_IGN);
    /* Send initial title and render */
    update_title(sock);
    render(fb);
    send_frame(sock, fb);

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

                            /* ── Rename mode input ── */
                            if (g_renaming) {
                                if (key == KEY_ESC) {
                                    g_renaming = false;
                                    redraw = true;
                                } else if (key == KEY_ENTER || key == '\r' || key == '\n') {
                                    g_renaming = false;
                                    if (g_rename_len > 0 && g_nentries > 0) {
                                        char oldpath[1280], newpath[1280];
                                        snprintf(oldpath, sizeof(oldpath), "%s/%s",
                                                 g_path, g_entries[g_selected].name);
                                        snprintf(newpath, sizeof(newpath), "%s/%.*s",
                                                 g_path, g_rename_len, g_rename_buf);
                                        if (rename(oldpath, newpath) == 0) {
                                            ipc_send_msg(sock, IPC_NOTIFY, "Renamed", 7);
                                            load_dir(g_path);
                                        } else {
                                            char ntxt[80];
                                            uint32_t nlen = (uint32_t)snprintf(ntxt, sizeof(ntxt),
                                                "Rename failed: %s", strerror(errno));
                                            ipc_send_msg(sock, IPC_NOTIFY, ntxt, nlen);
                                        }
                                    }
                                    dirty = true;
                                } else if (key == 0x16u) {
                                    /* Ctrl+V: request clipboard */
                                    ipc_send_msg(sock, IPC_CLIP_GET, NULL, 0);
                                } else if (key == KEY_BS && g_rename_len > 0) {
                                    g_rename_buf[--g_rename_len] = '\0';
                                    redraw = true;
                                } else if (key >= 0x20u && key < 0x7Fu &&
                                           g_rename_len < (int)sizeof(g_rename_buf) - 1) {
                                    g_rename_buf[g_rename_len++] = (char)key;
                                    g_rename_buf[g_rename_len]   = '\0';
                                    redraw = true;
                                }
                                if (redraw) dirty = true;
                                goto msg_done;
                            }

                            /* Any key other than 'd' cancels pending delete */
                            if (key != 'd' && key != 'D') g_confirm_delete = false;
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
                            } else if (key == 'c' || key == 'C') {
                                if (g_nentries > 0 && g_selected < g_nentries) {
                                    char fullpath[1280];
                                    snprintf(fullpath, sizeof(fullpath), "%s/%s",
                                             g_path, g_entries[g_selected].name);
                                    ipc_send_msg(sock, IPC_CLIP_SET, fullpath,
                                                 (uint32_t)strlen(fullpath));
                                }
                            } else if (key == 'd' || key == 'D') {
                                if (g_nentries > 0 && g_selected < g_nentries) {
                                    if (!g_confirm_delete) {
                                        g_confirm_delete = true;
                                        redraw = true;
                                    } else {
                                        g_confirm_delete = false;
                                        char fullpath[1280];
                                        snprintf(fullpath, sizeof(fullpath), "%s/%s",
                                                 g_path, g_entries[g_selected].name);
                                        int ret = g_entries[g_selected].is_dir
                                                  ? rmdir(fullpath) : unlink(fullpath);
                                        char ntxt[128];
                                        uint32_t nlen;
                                        if (ret == 0) {
                                            nlen = (uint32_t)snprintf(ntxt, sizeof(ntxt),
                                                "Deleted: %s", g_entries[g_selected].name);
                                            load_dir(g_path);
                                        } else {
                                            nlen = (uint32_t)snprintf(ntxt, sizeof(ntxt),
                                                "Delete failed: %s", strerror(errno));
                                        }
                                        ipc_send_msg(sock, IPC_NOTIFY, ntxt, nlen);
                                        dirty = true;
                                    }
                                }
                            } else if (key == 'n' || key == 'N') {
                                char newdir[1280];
                                snprintf(newdir, sizeof(newdir), "%s/NewFolder", g_path);
                                if (mkdir(newdir, 0755) == 0) {
                                    ipc_send_msg(sock, IPC_NOTIFY, "NewFolder created", 17);
                                    load_dir(g_path);
                                    dirty = true;
                                } else {
                                    char ntxt[64];
                                    uint32_t nlen = (uint32_t)snprintf(ntxt, sizeof(ntxt),
                                        "mkdir failed: %s", strerror(errno));
                                    ipc_send_msg(sock, IPC_NOTIFY, ntxt, nlen);
                                }
                            } else if (key == 'r' || key == 'R') {
                                load_dir(g_path);
                                dirty = true;
                            } else if (key == KEY_F5) {
                                if (g_nentries > 0 && g_selected < g_nentries) {
                                    g_renaming = true;
                                    g_rename_len = (int)strlen(g_entries[g_selected].name);
                                    if (g_rename_len >= (int)sizeof(g_rename_buf))
                                        g_rename_len = (int)sizeof(g_rename_buf) - 1;
                                    memcpy(g_rename_buf, g_entries[g_selected].name,
                                           (size_t)g_rename_len);
                                    g_rename_buf[g_rename_len] = '\0';
                                    redraw = true;
                                }
                            }
                            if (redraw) { dirty = true; }
                            msg_done:;
                        } else if (type == IPC_INPUT_MOUSE && in_plen >= 9 && in_pld) {
                            int32_t mx, my; uint8_t btns;
                            int8_t scroll = 0;
                            memcpy(&mx, in_pld,     4);
                            memcpy(&my, in_pld + 4, 4);
                            btns = in_pld[8];
                            if (in_plen >= 10) scroll = (int8_t)in_pld[9];
                            /* Scroll wheel: move selection and scroll */
                            if (scroll != 0) {
                                g_selected += scroll;
                                if (g_selected < 0) g_selected = 0;
                                if (g_selected >= g_nentries) g_selected = g_nentries - 1;
                                clamp_scroll();
                                dirty = true;
                            }
                            bool lbtn = (btns & 1);
                            if (lbtn && !prev_lbtn) {
                                /* Press: select row and start potential drag */
                                int list_top = TITLE_H + HDR_H;
                                if (my >= list_top && my < g_h - FOOT_H) {
                                    int row = (my - list_top) / ITEM_H;
                                    int idx = g_scroll + row;
                                    if (idx < g_nentries) {
                                        /* Prepare drag BEFORE nav_enter — it may
                                         * reload g_entries and stale idx */
                                        snprintf(g_drag_path, sizeof(g_drag_path),
                                                 "%s/%s", g_path, g_entries[idx].name);
                                        g_drag_pending = true;
                                        g_drag_smx = mx; g_drag_smy = my;
                                        if (idx == g_selected) {
                                            nav_enter(sock, fb);
                                        } else {
                                            g_selected = idx;
                                            clamp_scroll();
                                            dirty = true;
                                        }
                                    }
                                }
                            } else if (lbtn && g_drag_pending) {
                                /* Held + moved — check drag threshold */
                                int32_t dx = mx - g_drag_smx, dy = my - g_drag_smy;
                                if (dx*dx + dy*dy > DRAG_THRESH*DRAG_THRESH) {
                                    ipc_send_msg(sock, IPC_DRAG_START,
                                                 g_drag_path, (uint32_t)strlen(g_drag_path));
                                    g_drag_pending = false;
                                }
                            } else if (!lbtn) {
                                g_drag_pending = false;
                            }
                            prev_lbtn = lbtn;
                        } else if (type == IPC_WIN_RESIZE) {
                            /* Compositor tells us the new window pixel size (two
                             * little-endian uint16). Re-render at that resolution so
                             * text stays crisp and more rows fit, rather than being
                             * upscaled from a fixed frame. */
                            if (in_pld && in_plen >= 4) {
                                int nw = (int)(in_pld[0] | (in_pld[1] << 8));
                                int nh = (int)(in_pld[2] | (in_pld[3] << 8));
                                if (nw < 240)  nw = 240;  if (nw > 8192) nw = 8192;
                                if (nh < 180)  nh = 180;  if (nh > 8192) nh = 8192;
                                if (nw != g_w || nh != g_h) {
                                    uint32_t *nfb = realloc(fb, (size_t)nw * nh * 4);
                                    if (nfb) { fb = nfb; g_w = nw; g_h = nh; }
                                }
                            }
                            dirty = true;
                        } else if (type == IPC_CLIP_DATA && in_plen > 0 && in_pld && g_renaming) {
                            for (uint32_t ci = 0; ci < in_plen &&
                                 g_rename_len < (int)sizeof(g_rename_buf) - 1; ci++) {
                                uint8_t ch = in_pld[ci];
                                if (ch >= 0x20u && ch < 0x7Fu)
                                    g_rename_buf[g_rename_len++] = (char)ch;
                            }
                            g_rename_buf[g_rename_len] = '\0';
                            dirty = true;
                        } else if (type == IPC_DROP_FILE && in_plen > 0 && in_plen < 1024 && in_pld) {
                            /* File dropped onto us — navigate to its directory */
                            char dropped[1024];
                            memcpy(dropped, in_pld, in_plen); dropped[in_plen] = '\0';
                            char *slash = strrchr(dropped, '/');
                            if (slash && slash != dropped) {
                                *slash = '\0';
                                snprintf(g_path, sizeof(g_path), "%s", dropped);
                            }
                            load_dir(g_path);
                            dirty = true;
                        }
                        free(in_pld); in_pld = NULL;
                        in_got = 0; in_plen = 0; in_pgot = 0;
                    }
                } else {
                    /* Zero-length message fully received */
                    if (type == IPC_INVALIDATE) dirty = true;
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
