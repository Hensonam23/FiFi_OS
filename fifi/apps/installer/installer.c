/* FiFi OS Installer — IPC app that guides installation to disk.
 *
 * Sections:
 *   1. Includes, constants, types
 *   2. Shared draw helpers
 *   3. Step 0: Welcome
 *   4. Step 1: Disk selection
 *   5. Step 2: Browser choice
 *   6. Step 3: Software selection
 *   7. Step 4: Confirm
 *   8. Step 5: Progress (runs install script)
 *   9. Step 6: Done / Error
 *  10. IPC message loop and main
 */

/* ── 1. Includes, constants, types ────────────────────────────────────── */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>

#define WIN_W 700
#define WIN_H 520

/* IPC message types */
#define IPC_APP_CONNECT  0x01u
#define IPC_APP_FRAME    0x02u
#define IPC_APP_CLOSE    0x05u
#define IPC_WIN_CREATED  0x10u
#define IPC_INPUT_KEY    0x20u
#define IPC_INPUT_MOUSE  0x21u
#define IPC_INVALIDATE   0x30u
#define FIFI_SOCK "/tmp/fifi-compositor.sock"

/* Steps */
#define STEP_WELCOME  0
#define STEP_DISK     1
#define STEP_BROWSER  2
#define STEP_SOFTWARE 3
#define STEP_CONFIRM  4
#define STEP_PROGRESS 5
#define STEP_DONE     6

#define MAX_DISKS 8
#define MAX_LOG   64

/* Disk info */
typedef struct {
    char name[32];    /* e.g. "sda" */
    char model[64];   /* e.g. "Samsung SSD 870" */
    uint64_t bytes;   /* total size in bytes */
    char size_str[16];/* e.g. "250 GB" */
} disk_t;

/* Browser choice */
#define BROWSER_LIBREWOLF 0
#define BROWSER_FIREFOX   1

/* Software flags */
#define SW_LIBREOFFICE (1u << 0)

/* App state */
static int      g_step          = STEP_WELCOME;
static disk_t   g_disks[MAX_DISKS];
static int      g_ndisks        = 0;
static int      g_sel_disk      = -1;
static int      g_browser       = BROWSER_LIBREWOLF;
static uint32_t g_software      = SW_LIBREOFFICE;
static int      g_hover         = -1;
static bool     g_dirty         = true;
static bool     g_done_ok       = false;
static char     g_error_msg[256];

/* Progress */
static char     g_log[MAX_LOG][128];
static int      g_log_count     = 0;
static int      g_log_scroll    = 0;
static pid_t    g_install_pid   = -1;
static int      g_install_pipe  = -1;
static int      g_progress_pct  = 0;

/* Glyph font */
static uint8_t *g_glyph         = NULL;
static int      g_glyph_h       = 16;

/* IPC globals */
static uint32_t *g_fb            = NULL;
static int       g_win_w         = WIN_W;
static int       g_win_h         = WIN_H;

/* ── 2. Shared draw helpers ───────────────────────────────────────────── */

static void put_pixel(uint32_t *fb, int x, int y, uint32_t col) {
    if (x < 0 || y < 0 || x >= g_win_w || y >= g_win_h) return;
    fb[y * g_win_w + x] = col;
}

static void fill(uint32_t *fb, int x, int y, int w, int h, uint32_t col) {
    for (int row = y; row < y + h; row++)
        for (int col2 = x; col2 < x + w; col2++)
            put_pixel(fb, col2, row, col);
}

static void draw_char(uint32_t *fb, int x, int y, unsigned char c,
                      uint32_t fg, uint32_t bg) {
    if (!g_glyph) return;
    const uint8_t *bits = g_glyph + c * g_glyph_h;
    for (int row = 0; row < g_glyph_h; row++) {
        uint8_t b = bits[row];
        for (int col = 0; col < 8; col++) {
            uint32_t px = (b & (0x80u >> col)) ? fg : bg;
            put_pixel(fb, x + col, y + row, px);
        }
    }
}

static int str_len(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

static void draw_str(uint32_t *fb, const char *s, int x, int y,
                     uint32_t fg, uint32_t bg) {
    for (int i = 0; s[i]; i++)
        draw_char(fb, x + i * 9, y, (unsigned char)s[i], fg, bg);
}

/* Draw string clipped to max_w pixels */
static void draw_str_clip(uint32_t *fb, const char *s, int x, int y,
                          int max_w, uint32_t fg, uint32_t bg) {
    int max_chars = max_w / 9;
    int len = str_len(s);
    if (len <= max_chars) {
        draw_str(fb, s, x, y, fg, bg);
    } else if (max_chars >= 3) {
        for (int i = 0; i < max_chars - 3; i++)
            draw_char(fb, x + i * 9, y, (unsigned char)s[i], fg, bg);
        for (int i = 0; i < 3; i++)
            draw_char(fb, x + (max_chars - 3 + i) * 9, y, '.', 0x00506070u, bg);
    }
}

/* Draw a filled rounded button */
static void draw_btn(uint32_t *fb, int x, int y, int w, int h,
                     const char *label, bool hovered, bool primary) {
    uint32_t bg = primary ? (hovered ? 0x003d78d8u : 0x003060c0u)
                          : (hovered ? 0x00304050u : 0x00222e3cu);
    uint32_t fg = 0x00e8eeffu;
    fill(fb, x, y, w, h, bg);
    /* Border */
    for (int i = x; i < x + w; i++) { put_pixel(fb, i, y, 0x004070a0u); put_pixel(fb, i, y+h-1, 0x004070a0u); }
    for (int i = y; i < y + h; i++) { put_pixel(fb, x, i, 0x004070a0u); put_pixel(fb, x+w-1, i, 0x004070a0u); }
    int lw = str_len(label) * 9;
    int tx = x + (w - lw) / 2;
    int ty = y + (h - g_glyph_h) / 2;
    draw_str(fb, label, tx, ty, fg, bg);
}

/* Draw a horizontal progress bar */
static void draw_progress(uint32_t *fb, int x, int y, int w, int h, int pct) {
    fill(fb, x, y, w, h, 0x00101820u);
    int filled = (w - 2) * pct / 100;
    if (filled > 0) fill(fb, x + 1, y + 1, filled, h - 2, 0x003060c0u);
    for (int i = x; i < x + w; i++) { put_pixel(fb, i, y, 0x00304050u); put_pixel(fb, i, y+h-1, 0x00304050u); }
    for (int i = y; i < y + h; i++) { put_pixel(fb, x, i, 0x00304050u); put_pixel(fb, x+w-1, i, 0x00304050u); }
}

/* Draw a radio button: filled circle if selected */
static void draw_radio(uint32_t *fb, int cx, int cy, int r, bool selected) {
    uint32_t border = 0x004080c0u;
    uint32_t inner  = selected ? 0x003060c0u : 0x00101820u;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx*dx + dy*dy;
            if (d2 <= (r-1)*(r-1)) put_pixel(fb, cx+dx, cy+dy, inner);
            else if (d2 <= r*r)    put_pixel(fb, cx+dx, cy+dy, border);
        }
    }
}

/* Draw a checkbox */
static void draw_checkbox(uint32_t *fb, int x, int y, int sz, bool checked) {
    fill(fb, x, y, sz, sz, 0x00101820u);
    for (int i = x; i < x + sz; i++) { put_pixel(fb, i, y, 0x004080c0u); put_pixel(fb, i, y+sz-1, 0x004080c0u); }
    for (int i = y; i < y + sz; i++) { put_pixel(fb, x, i, 0x004080c0u); put_pixel(fb, x+sz-1, i, 0x004080c0u); }
    if (checked) {
        /* Checkmark */
        for (int i = 2; i < sz - 2; i++) {
            int j = (i < sz/2) ? (i - 2 + 2) : (sz - i - 1 + 2);
            put_pixel(fb, x + i, y + j, 0x0050d890u);
            put_pixel(fb, x + i, y + j + 1, 0x0050d890u);
        }
    }
}

/* Draw a divider line */
static void draw_sep(uint32_t *fb, int y) {
    for (int x = 20; x < g_win_w - 20; x++) put_pixel(fb, x, y, 0x00202838u);
}

/* Common header: title bar area and step indicator */
static void draw_header(uint32_t *fb, const char *title) {
    fill(fb, 0, 0, g_win_w, g_win_h, 0x000c1018u);
    /* Title area */
    fill(fb, 0, 0, g_win_w, 40, 0x00101828u);
    draw_str(fb, title, 20, 12, 0x0090c4e8u, 0x00101828u);
    draw_sep(fb, 39);
    /* Step dots */
    int steps[] = {STEP_DISK, STEP_BROWSER, STEP_SOFTWARE, STEP_CONFIRM, STEP_PROGRESS, STEP_DONE};
    int n = 6;
    int dot_x = g_win_w - 20 - n * 14;
    int dot_y = 19;
    for (int i = 0; i < n; i++) {
        bool active = (g_step == steps[i]);
        bool done   = (g_step > steps[i]);
        uint32_t col = done ? 0x003060c0u : (active ? 0x0060a0e0u : 0x00283848u);
        int cx = dot_x + i * 14 + 4;
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++)
                if (dx*dx + dy*dy <= 9) put_pixel(fb, cx+dx, dot_y+dy, col);
    }
}

/* ── 3. Step 0: Welcome ────────────────────────────────────────────────── */

static void render_welcome(uint32_t *fb) {
    draw_header(fb, "FiFi OS  Installer");
    int y = 70;
    draw_str(fb, "Welcome to FiFi OS", 30, y, 0x00c8dce8u, 0x000c1018u); y += 28;
    draw_str(fb, "This will install FiFi OS to a disk on this machine.", 30, y, 0x00708898u, 0x000c1018u); y += 20;
    draw_str(fb, "Your chosen disk will be erased. Back up anything important", 30, y, 0x00708898u, 0x000c1018u); y += 20;
    draw_str(fb, "before continuing.", 30, y, 0x00708898u, 0x000c1018u); y += 36;
    draw_str(fb, "What this installer does:", 30, y, 0x0090a8c0u, 0x000c1018u); y += 22;
    const char *steps[] = {
        "  1.  Choose a disk to install to",
        "  2.  Choose your browser  (Firefox or LibreWolf)",
        "  3.  Select additional software  (LibreOffice included by default)",
        "  4.  Install FiFi OS and download selected software",
        NULL
    };
    for (int i = 0; steps[i]; i++) {
        draw_str(fb, steps[i], 30, y, 0x00607888u, 0x000c1018u);
        y += 20;
    }
    y += 20;
    draw_str(fb, "Requires an internet connection to download browser and LibreOffice.", 30, y, 0x00485868u, 0x000c1018u);
    /* Next button */
    draw_btn(fb, g_win_w - 130, g_win_h - 56, 110, 36, "Get Started", g_hover == 0, true);
}

static void click_welcome(int mx, int my) {
    if (mx >= g_win_w - 130 && mx < g_win_w - 20 &&
        my >= g_win_h - 56  && my < g_win_h - 20) {
        g_step = STEP_DISK;
        g_dirty = true;
    }
}

/* ── 4. Step 1: Disk selection ────────────────────────────────────────── */

static void fmt_size(char *buf, uint64_t bytes) {
    if (bytes >= 1000000000000ULL)
        snprintf(buf, 16, "%.1f TB", (double)bytes / 1e12);
    else if (bytes >= 1000000000ULL)
        snprintf(buf, 16, "%.0f GB", (double)bytes / 1e9);
    else
        snprintf(buf, 16, "%.0f MB", (double)bytes / 1e6);
}

static void scan_disks(void) {
    g_ndisks = 0;
    DIR *d = opendir("/sys/block");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_ndisks < MAX_DISKS) {
        const char *name = de->d_name;
        if (name[0] == '.') continue;
        /* Skip loop, ram, sr, zram devices */
        if (name[0] == 'l' && name[1] == 'o') continue;
        if (name[0] == 'r' && name[1] == 'a') continue;
        if (name[0] == 's' && name[1] == 'r') continue;
        if (name[0] == 'z' && name[1] == 'r') continue;
        disk_t *dk = &g_disks[g_ndisks];
        snprintf(dk->name, sizeof(dk->name), "%s", name);
        /* Read size */
        char sz_path[80];
        snprintf(sz_path, sizeof(sz_path), "/sys/block/%s/size", name);
        FILE *f = fopen(sz_path, "r");
        if (f) {
            uint64_t sectors = 0;
            if (fscanf(f, "%llu", (unsigned long long *)&sectors) == 1)
                dk->bytes = sectors * 512ULL;
            fclose(f);
        }
        if (dk->bytes < 4ULL * 1024 * 1024 * 1024) continue; /* skip <4GB */
        /* Read model */
        char mdl_path[80];
        snprintf(mdl_path, sizeof(mdl_path), "/sys/block/%s/device/model", name);
        f = fopen(mdl_path, "r");
        if (f) {
            if (fgets(dk->model, sizeof(dk->model), f)) {
                int l = str_len(dk->model);
                while (l > 0 && (dk->model[l-1] == '\n' || dk->model[l-1] == ' ')) dk->model[--l] = '\0';
            }
            fclose(f);
        } else {
            snprintf(dk->model, sizeof(dk->model), "Disk");
        }
        fmt_size(dk->size_str, dk->bytes);
        g_ndisks++;
    }
    closedir(d);
}

static void render_disk(uint32_t *fb) {
    draw_header(fb, "FiFi OS Installer  |  Select Disk");
    int y = 56;
    draw_str(fb, "Choose a disk to install FiFi OS to.", 20, y, 0x00708898u, 0x000c1018u); y += 18;
    draw_str(fb, "The entire disk will be erased.", 20, y, 0x00a06040u, 0x000c1018u); y += 28;
    if (g_ndisks == 0) {
        draw_str(fb, "No suitable disks found.", 20, y, 0x00a07060u, 0x000c1018u);
    }
    for (int i = 0; i < g_ndisks; i++) {
        bool sel = (g_sel_disk == i);
        bool hov = (g_hover == i);
        uint32_t row_bg = sel ? 0x001c3050u : (hov ? 0x00182030u : 0x00101820u);
        fill(fb, 20, y, g_win_w - 40, 48, row_bg);
        /* Border */
        uint32_t brd = sel ? 0x003060c0u : 0x00253040u;
        for (int x = 20; x < g_win_w - 20; x++) { put_pixel(fb, x, y, brd); put_pixel(fb, x, y+47, brd); }
        /* Radio + disk info */
        draw_radio(fb, 38, y + 24, 8, sel);
        char line1[80], line2[40];
        snprintf(line1, sizeof(line1), "/dev/%s  —  %s", g_disks[i].name, g_disks[i].model);
        snprintf(line2, sizeof(line2), "%s", g_disks[i].size_str);
        draw_str_clip(fb, line1, 56, y + 10, g_win_w - 130, 0x00c0d0e0u, row_bg);
        draw_str(fb, line2, 56, y + 26, 0x00607888u, row_bg);
        y += 54;
    }
    /* Back + Next */
    draw_btn(fb, 20, g_win_h - 56, 100, 36, "Back", g_hover == 100, false);
    bool can_next = (g_sel_disk >= 0);
    uint32_t next_bg = can_next ? (g_hover == 101 ? 0x003d78d8u : 0x003060c0u) : 0x00202838u;
    uint32_t next_fg = can_next ? 0x00e8eeffu : 0x00404858u;
    fill(fb, g_win_w - 130, g_win_h - 56, 110, 36, next_bg);
    int lw = str_len("Next") * 9;
    draw_str(fb, "Next", g_win_w - 130 + (110 - lw) / 2, g_win_h - 56 + (36 - g_glyph_h) / 2, next_fg, next_bg);
}

static void click_disk(int mx, int my) {
    /* Disk rows */
    int y = 102;
    for (int i = 0; i < g_ndisks; i++) {
        if (mx >= 20 && mx < g_win_w - 20 && my >= y && my < y + 48) {
            g_sel_disk = i; g_dirty = true;
        }
        y += 54;
    }
    /* Back */
    if (mx >= 20 && mx < 120 && my >= g_win_h - 56 && my < g_win_h - 20) {
        g_step = STEP_WELCOME; g_dirty = true;
    }
    /* Next */
    if (g_sel_disk >= 0 && mx >= g_win_w - 130 && mx < g_win_w - 20 &&
        my >= g_win_h - 56 && my < g_win_h - 20) {
        g_step = STEP_BROWSER; g_dirty = true;
    }
}

/* ── 5. Step 2: Browser choice ────────────────────────────────────────── */

static void render_browser(uint32_t *fb) {
    draw_header(fb, "FiFi OS Installer  |  Choose Browser");
    int y = 56;
    draw_str(fb, "Which browser do you want installed?", 20, y, 0x00708898u, 0x000c1018u); y += 18;
    draw_str(fb, "The browser will be downloaded during installation.", 20, y, 0x00485868u, 0x000c1018u); y += 36;

    /* LibreWolf */
    bool lw_sel = (g_browser == BROWSER_LIBREWOLF);
    bool lw_hov = (g_hover == 0);
    uint32_t lw_bg = lw_sel ? 0x001c3050u : (lw_hov ? 0x00182030u : 0x00101820u);
    fill(fb, 20, y, g_win_w - 40, 80, lw_bg);
    uint32_t lw_brd = lw_sel ? 0x003060c0u : 0x00253040u;
    for (int x = 20; x < g_win_w - 20; x++) { put_pixel(fb, x, y, lw_brd); put_pixel(fb, x, y+79, lw_brd); }
    draw_radio(fb, 40, y + 40, 9, lw_sel);
    draw_str(fb, "LibreWolf", 58, y + 16, 0x00c8dce8u, lw_bg);
    draw_str(fb, "Privacy-hardened Firefox fork. No telemetry, enhanced tracking", 58, y + 34, 0x00607888u, lw_bg);
    draw_str(fb, "protection, and stricter security defaults.", 58, y + 52, 0x00607888u, lw_bg);
    y += 86;

    /* Firefox */
    bool ff_sel = (g_browser == BROWSER_FIREFOX);
    bool ff_hov = (g_hover == 1);
    uint32_t ff_bg = ff_sel ? 0x001c3050u : (ff_hov ? 0x00182030u : 0x00101820u);
    fill(fb, 20, y, g_win_w - 40, 80, ff_bg);
    uint32_t ff_brd = ff_sel ? 0x003060c0u : 0x00253040u;
    for (int x = 20; x < g_win_w - 20; x++) { put_pixel(fb, x, y, ff_brd); put_pixel(fb, x, y+79, ff_brd); }
    draw_radio(fb, 40, y + 40, 9, ff_sel);
    draw_str(fb, "Firefox", 58, y + 16, 0x00c8dce8u, ff_bg);
    draw_str(fb, "Standard Firefox release. Familiar and widely supported.", 58, y + 34, 0x00607888u, ff_bg);
    draw_str(fb, "Can add any extension from Mozilla Add-ons.", 58, y + 52, 0x00607888u, ff_bg);
    y += 86;

    draw_btn(fb, 20, g_win_h - 56, 100, 36, "Back", g_hover == 100, false);
    draw_btn(fb, g_win_w - 130, g_win_h - 56, 110, 36, "Next", g_hover == 101, true);
}

static void click_browser(int mx, int my) {
    int y = 110;
    /* LibreWolf row */
    if (mx >= 20 && mx < g_win_w - 20 && my >= y && my < y + 80)
        { g_browser = BROWSER_LIBREWOLF; g_dirty = true; }
    y += 86;
    /* Firefox row */
    if (mx >= 20 && mx < g_win_w - 20 && my >= y && my < y + 80)
        { g_browser = BROWSER_FIREFOX; g_dirty = true; }
    if (mx >= 20 && mx < 120 && my >= g_win_h - 56 && my < g_win_h - 20)
        { g_step = STEP_DISK; g_dirty = true; }
    if (mx >= g_win_w - 130 && mx < g_win_w - 20 && my >= g_win_h - 56 && my < g_win_h - 20)
        { g_step = STEP_SOFTWARE; g_dirty = true; }
}

/* ── 6. Step 3: Software selection ────────────────────────────────────── */

static void render_software(uint32_t *fb) {
    draw_header(fb, "FiFi OS Installer  |  Software");
    int y = 56;
    draw_str(fb, "Select additional software to install.", 20, y, 0x00708898u, 0x000c1018u); y += 18;
    draw_str(fb, "These will be downloaded during installation.", 20, y, 0x00485868u, 0x000c1018u); y += 36;

    /* LibreOffice row */
    bool lo_chk = !!(g_software & SW_LIBREOFFICE);
    bool lo_hov = (g_hover == 0);
    uint32_t lo_bg = lo_chk ? 0x001c3050u : (lo_hov ? 0x00182030u : 0x00101820u);
    fill(fb, 20, y, g_win_w - 40, 72, lo_bg);
    uint32_t lo_brd = lo_chk ? 0x003060c0u : 0x00253040u;
    for (int x = 20; x < g_win_w - 20; x++) { put_pixel(fb, x, y, lo_brd); put_pixel(fb, x, y+71, lo_brd); }
    draw_checkbox(fb, 32, y + 26, 18, lo_chk);
    draw_str(fb, "LibreOffice  (Recommended)", 60, y + 14, 0x00c8dce8u, lo_bg);
    draw_str(fb, "Full office suite: Writer, Calc, Impress, Draw. Compatible with", 60, y + 32, 0x00607888u, lo_bg);
    draw_str(fb, "Microsoft Office formats.", 60, y + 50, 0x00607888u, lo_bg);
    y += 78;
    draw_str(fb, "More software can be added after installation.", 20, y, 0x00485868u, 0x000c1018u);

    draw_btn(fb, 20, g_win_h - 56, 100, 36, "Back", g_hover == 100, false);
    draw_btn(fb, g_win_w - 130, g_win_h - 56, 110, 36, "Next", g_hover == 101, true);
}

static void click_software(int mx, int my) {
    int y = 110;
    if (mx >= 20 && mx < g_win_w - 20 && my >= y && my < y + 72)
        { g_software ^= SW_LIBREOFFICE; g_dirty = true; }
    if (mx >= 20 && mx < 120 && my >= g_win_h - 56 && my < g_win_h - 20)
        { g_step = STEP_BROWSER; g_dirty = true; }
    if (mx >= g_win_w - 130 && mx < g_win_w - 20 && my >= g_win_h - 56 && my < g_win_h - 20)
        { g_step = STEP_CONFIRM; g_dirty = true; }
}

/* ── 7. Step 4: Confirm ───────────────────────────────────────────────── */

static void render_confirm(uint32_t *fb) {
    draw_header(fb, "FiFi OS Installer  |  Confirm");
    int y = 56;
    draw_str(fb, "Review your choices before installing.", 20, y, 0x00708898u, 0x000c1018u); y += 30;

    /* Summary box */
    fill(fb, 20, y, g_win_w - 40, 160, 0x00101820u);
    for (int x = 20; x < g_win_w - 20; x++) { put_pixel(fb, x, y, 0x00304050u); put_pixel(fb, x, y+159, 0x00304050u); }
    int sy = y + 14;
    draw_str(fb, "Disk:", 36, sy, 0x00607888u, 0x00101820u);
    if (g_sel_disk >= 0) {
        char dstr[80];
        snprintf(dstr, sizeof(dstr), "/dev/%s  (%s  %s)",
                 g_disks[g_sel_disk].name, g_disks[g_sel_disk].size_str,
                 g_disks[g_sel_disk].model);
        draw_str_clip(fb, dstr, 130, sy, g_win_w - 160, 0x00c0d0e0u, 0x00101820u);
    }
    sy += 24;
    draw_str(fb, "Browser:", 36, sy, 0x00607888u, 0x00101820u);
    draw_str(fb, g_browser == BROWSER_LIBREWOLF ? "LibreWolf" : "Firefox",
             130, sy, 0x00c0d0e0u, 0x00101820u);
    sy += 24;
    draw_str(fb, "Software:", 36, sy, 0x00607888u, 0x00101820u);
    draw_str(fb, (g_software & SW_LIBREOFFICE) ? "LibreOffice" : "(none extra)",
             130, sy, 0x00c0d0e0u, 0x00101820u);
    sy += 24;
    draw_str(fb, "Action:", 36, sy, 0x00607888u, 0x00101820u);
    if (g_sel_disk >= 0) {
        char act[80];
        snprintf(act, sizeof(act), "Erase /dev/%s and install FiFi OS",
                 g_disks[g_sel_disk].name);
        draw_str_clip(fb, act, 130, sy, g_win_w - 160, 0x00e87060u, 0x00101820u);
    }
    y += 170;
    draw_str(fb, "This cannot be undone. The selected disk will be permanently erased.", 20, y, 0x00a06848u, 0x000c1018u);

    draw_btn(fb, 20, g_win_h - 56, 100, 36, "Back", g_hover == 100, false);
    draw_btn(fb, g_win_w - 150, g_win_h - 56, 130, 36, "Install Now", g_hover == 101, true);
}

static void click_confirm(int mx, int my) {
    if (mx >= 20 && mx < 120 && my >= g_win_h - 56 && my < g_win_h - 20)
        { g_step = STEP_SOFTWARE; g_dirty = true; }
    if (mx >= g_win_w - 150 && mx < g_win_w - 20 && my >= g_win_h - 56 && my < g_win_h - 20) {
        g_step = STEP_PROGRESS;
        g_log_count = 0; g_log_scroll = 0; g_progress_pct = 0;
        g_dirty = true;
        /* Launch the install script in background */
        int pipefd[2];
        if (pipe(pipefd) == 0) {
            g_install_pipe = pipefd[0];
            fcntl(g_install_pipe, F_SETFL, O_NONBLOCK);
            g_install_pid = fork();
            if (g_install_pid == 0) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                dup2(pipefd[1], STDERR_FILENO);
                close(pipefd[1]);
                char disk[32];
                snprintf(disk, sizeof(disk), "/dev/%s",
                         g_disks[g_sel_disk].name);
                execl("/bin/fifi-install.sh", "fifi-install.sh",
                      disk,
                      g_browser == BROWSER_LIBREWOLF ? "librewolf" : "firefox",
                      (g_software & SW_LIBREOFFICE) ? "libreoffice" : "none",
                      NULL);
                printf("ERROR: /bin/fifi-install.sh not found\n");
                fflush(stdout);
                _exit(1);
            }
            close(pipefd[1]);
        }
    }
}

/* ── 8. Step 5: Progress ──────────────────────────────────────────────── */

static void log_append(const char *line) {
    if (g_log_count < MAX_LOG) {
        int n = str_len(line);
        if (n > 127) n = 127;
        for (int i = 0; i < n; i++) g_log[g_log_count][i] = line[i];
        g_log[g_log_count][n] = '\0';
        g_log_count++;
    } else {
        /* Scroll: drop oldest */
        for (int i = 0; i < MAX_LOG - 1; i++)
            for (int j = 0; j < 128; j++) g_log[i][j] = g_log[i+1][j];
        int n = str_len(line); if (n > 127) n = 127;
        for (int i = 0; i < n; i++) g_log[MAX_LOG-1][i] = line[i];
        g_log[MAX_LOG-1][n] = '\0';
    }
}

/* Poll install script output */
static void poll_install(void) {
    if (g_install_pipe < 0) return;
    char buf[512];
    ssize_t n = read(g_install_pipe, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        /* Split by newlines */
        char *p = buf, *nl;
        while ((nl = strchr(p, '\n')) != NULL) {
            *nl = '\0';
            if (p[0]) {
                /* Parse progress lines: "PROGRESS:50" */
                if (p[0] == 'P' && p[1] == 'R' && p[8] == ':') {
                    g_progress_pct = atoi(p + 9);
                    if (g_progress_pct > 100) g_progress_pct = 100;
                } else {
                    log_append(p);
                }
                g_dirty = true;
            }
            p = nl + 1;
        }
        if (p[0]) { log_append(p); g_dirty = true; }
    } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
        /* Pipe closed — install finished */
        close(g_install_pipe);
        g_install_pipe = -1;
        int status = 0;
        if (g_install_pid > 0) {
            waitpid(g_install_pid, &status, 0);
            g_install_pid = -1;
        }
        g_done_ok = (status == 0);
        if (!g_done_ok) {
            snprintf(g_error_msg, sizeof(g_error_msg),
                     "Installation failed (exit code %d). Check log above.", WEXITSTATUS(status));
        }
        g_progress_pct = g_done_ok ? 100 : g_progress_pct;
        g_step = STEP_DONE;
        g_dirty = true;
    }
}

static void render_progress(uint32_t *fb) {
    draw_header(fb, "FiFi OS Installer  |  Installing...");
    int y = 50;
    draw_str(fb, "Installation in progress. Do not power off.", 20, y, 0x00708898u, 0x000c1018u); y += 28;
    draw_progress(fb, 20, y, g_win_w - 40, 18, g_progress_pct);
    char pct_str[16]; snprintf(pct_str, sizeof(pct_str), "%d%%", g_progress_pct);
    int pw = str_len(pct_str) * 9;
    draw_str(fb, pct_str, g_win_w / 2 - pw / 2, y + 2, 0x00c0d0e0u, 0x00101820u);
    y += 28;
    draw_sep(fb, y); y += 8;
    /* Log area */
    int log_y = y;
    int visible = (g_win_h - log_y - 20) / (g_glyph_h + 2);
    int start = g_log_count > visible ? g_log_count - visible : 0;
    for (int i = start; i < g_log_count; i++) {
        uint32_t col = 0x00506878u;
        if (g_log[i][0] == 'E' || g_log[i][0] == 'e') col = 0x00e07060u;
        else if (g_log[i][0] == '[') col = 0x0070b8e0u;
        draw_str_clip(fb, g_log[i], 20, log_y, g_win_w - 40, col, 0x000c1018u);
        log_y += g_glyph_h + 2;
    }
}

/* ── 9. Step 6: Done ──────────────────────────────────────────────────── */

static void render_done(uint32_t *fb) {
    draw_header(fb, g_done_ok ? "FiFi OS Installer  |  Complete" : "FiFi OS Installer  |  Error");
    int y = 80;
    if (g_done_ok) {
        draw_str(fb, "Installation complete!", 30, y, 0x0060e890u, 0x000c1018u); y += 28;
        draw_str(fb, "FiFi OS has been installed to the selected disk.", 30, y, 0x00708898u, 0x000c1018u); y += 20;
        draw_str(fb, "You can now remove the USB drive and reboot.", 30, y, 0x00708898u, 0x000c1018u); y += 36;
        draw_str(fb, "Your installed system includes:", 30, y, 0x0090a8c0u, 0x000c1018u); y += 22;
        draw_str(fb, g_browser == BROWSER_LIBREWOLF ? "  - LibreWolf browser" : "  - Firefox browser",
                 30, y, 0x00607888u, 0x000c1018u); y += 20;
        if (g_software & SW_LIBREOFFICE)
            { draw_str(fb, "  - LibreOffice", 30, y, 0x00607888u, 0x000c1018u); y += 20; }
        draw_btn(fb, g_win_w / 2 - 60, g_win_h - 56, 120, 36, "Reboot", g_hover == 0, true);
    } else {
        draw_str(fb, "Installation failed.", 30, y, 0x00e07060u, 0x000c1018u); y += 24;
        draw_str_clip(fb, g_error_msg, 30, y, g_win_w - 60, 0x00a07060u, 0x000c1018u); y += 28;
        draw_btn(fb, g_win_w / 2 - 55, g_win_h - 56, 110, 36, "Close", g_hover == 0, false);
    }
}

static void click_done(int mx, int my) {
    if (my >= g_win_h - 56 && my < g_win_h - 20 &&
        mx >= g_win_w / 2 - 60 && mx < g_win_w / 2 + 60) {
        if (g_done_ok) {
            system("reboot");
        }
        /* Close on error — compositor will handle IPC_APP_CLOSE */
    }
}

/* ── 10. IPC message loop and main ────────────────────────────────────── */

static void render(uint32_t *fb) {
    switch (g_step) {
    case STEP_WELCOME:  render_welcome(fb);  break;
    case STEP_DISK:     render_disk(fb);     break;
    case STEP_BROWSER:  render_browser(fb);  break;
    case STEP_SOFTWARE: render_software(fb); break;
    case STEP_CONFIRM:  render_confirm(fb);  break;
    case STEP_PROGRESS: render_progress(fb); break;
    case STEP_DONE:     render_done(fb);     break;
    }
}

static void handle_click(int mx, int my) {
    switch (g_step) {
    case STEP_WELCOME:  click_welcome(mx, my);  break;
    case STEP_DISK:     click_disk(mx, my);     break;
    case STEP_BROWSER:  click_browser(mx, my);  break;
    case STEP_SOFTWARE: click_software(mx, my); break;
    case STEP_CONFIRM:  click_confirm(mx, my);  break;
    case STEP_DONE:     click_done(mx, my);     break;
    }
}

static void handle_hover(int mx, int my) {
    int old = g_hover;
    g_hover = -1;
    switch (g_step) {
    case STEP_WELCOME:
        if (mx >= g_win_w-130 && mx < g_win_w-20 && my >= g_win_h-56 && my < g_win_h-20) g_hover = 0;
        break;
    case STEP_DISK: {
        int y = 102;
        for (int i = 0; i < g_ndisks; i++) {
            if (mx >= 20 && mx < g_win_w-20 && my >= y && my < y+48) g_hover = i;
            y += 54;
        }
        if (mx >= 20 && mx < 120 && my >= g_win_h-56 && my < g_win_h-20) g_hover = 100;
        if (g_sel_disk >= 0 && mx >= g_win_w-130 && mx < g_win_w-20 && my >= g_win_h-56 && my < g_win_h-20) g_hover = 101;
        break;
    }
    case STEP_BROWSER:
        if (mx >= 20 && mx < g_win_w-20 && my >= 110 && my < 190) g_hover = 0;
        if (mx >= 20 && mx < g_win_w-20 && my >= 196 && my < 276) g_hover = 1;
        if (mx >= 20 && mx < 120 && my >= g_win_h-56 && my < g_win_h-20) g_hover = 100;
        if (mx >= g_win_w-130 && mx < g_win_w-20 && my >= g_win_h-56 && my < g_win_h-20) g_hover = 101;
        break;
    case STEP_SOFTWARE:
        if (mx >= 20 && mx < g_win_w-20 && my >= 110 && my < 182) g_hover = 0;
        if (mx >= 20 && mx < 120 && my >= g_win_h-56 && my < g_win_h-20) g_hover = 100;
        if (mx >= g_win_w-130 && mx < g_win_w-20 && my >= g_win_h-56 && my < g_win_h-20) g_hover = 101;
        break;
    case STEP_CONFIRM:
        if (mx >= 20 && mx < 120 && my >= g_win_h-56 && my < g_win_h-20) g_hover = 100;
        if (mx >= g_win_w-150 && mx < g_win_w-20 && my >= g_win_h-56 && my < g_win_h-20) g_hover = 101;
        break;
    case STEP_DONE:
        if (my >= g_win_h-56 && my < g_win_h-20 && mx >= g_win_w/2-60 && mx < g_win_w/2+60) g_hover = 0;
        break;
    }
    if (g_hover != old) g_dirty = true;
}

static void write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n > 0) { ssize_t w = write(fd, p, n); if (w <= 0) break; p += w; n -= (size_t)w; }
}

static void send_frame(int sock) {
    uint32_t hdr[4] = {0, 0, (uint32_t)g_win_w, (uint32_t)g_win_h};
    uint32_t pld_sz = 16 + (uint32_t)(g_win_w * g_win_h * 4);
    uint8_t *msg = malloc(pld_sz);
    if (!msg) return;
    uint8_t type_hdr[8];
    uint32_t t = IPC_APP_FRAME, l = pld_sz;
    memcpy(type_hdr, &t, 4); memcpy(type_hdr+4, &l, 4);
    write_all(sock, type_hdr, 8);
    memcpy(msg, hdr, 16);
    memcpy(msg + 16, g_fb, (size_t)(g_win_w * g_win_h * 4));
    write_all(sock, msg, pld_sz);
    free(msg);
}

static void ipc_send(int sock, uint32_t type, const void *data, uint32_t len) {
    uint8_t hdr[8];
    memcpy(hdr, &type, 4); memcpy(hdr+4, &len, 4);
    write_all(sock, hdr, 8);
    if (len && data) write_all(sock, data, len);
}

int main(void) {
    /* Load font */
    const char *font_paths[] = {
        "/fonts/ter16b.psf", "/fonts/ter20b.psf", "/fonts/ter24b.psf",
        "/fonts/default.psf", NULL
    };
    for (int i = 0; font_paths[i]; i++) {
        int fd = open(font_paths[i], O_RDONLY);
        if (fd < 0) continue;
        uint8_t hdr[4]; read(fd, hdr, 4);
        int total; lseek(fd, 0, SEEK_END); total = (int)lseek(fd, 0, SEEK_CUR);
        lseek(fd, 0, SEEK_SET);
        g_glyph = malloc((size_t)total); if (!g_glyph) { close(fd); continue; }
        read(fd, g_glyph, (size_t)total); close(fd);
        /* PSF2: header is 32 bytes, glyph height at offset 20 */
        if (g_glyph[0]==0x72 && g_glyph[1]==0xb5 && g_glyph[2]==0x4a && g_glyph[3]==0x86) {
            g_glyph_h = (int)((uint32_t)g_glyph[20] | ((uint32_t)g_glyph[21]<<8) |
                              ((uint32_t)g_glyph[22]<<16) | ((uint32_t)g_glyph[23]<<24));
            uint32_t hdr_sz = (uint32_t)g_glyph[8] | ((uint32_t)g_glyph[9]<<8) |
                              ((uint32_t)g_glyph[10]<<16) | ((uint32_t)g_glyph[11]<<24);
            memmove(g_glyph, g_glyph + hdr_sz, (size_t)(total - (int)hdr_sz));
        } else { g_glyph_h = 16; }
        break;
    }
    if (!g_glyph) { g_glyph = calloc(256 * 16, 1); g_glyph_h = 16; }

    /* Framebuffer */
    g_fb = calloc((size_t)(g_win_w * g_win_h), 4);
    if (!g_fb) return 1;

    /* Scan disks */
    scan_disks();

    /* Connect to compositor */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return 1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIFI_SOCK, sizeof(addr.sun_path)-1);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(sock); return 1; }

    uint8_t conn[68] = {0};
    uint16_t w = WIN_W, h = WIN_H;
    memcpy(conn, &w, 2); memcpy(conn+2, &h, 2);
    snprintf((char*)(conn+4), 64, "Install FiFi OS");
    ipc_send(sock, IPC_APP_CONNECT, conn, sizeof(conn));
    { uint8_t rhdr[8]; read(sock, rhdr, 8); uint32_t pl; memcpy(&pl, rhdr+4, 4);
      if (pl && pl < 64) { uint8_t r[64]; read(sock, r, pl); } }

    signal(SIGPIPE, SIG_IGN);
    fcntl(sock, F_SETFL, O_NONBLOCK);

    /* Initial render */
    render(g_fb);
    send_frame(sock);
    g_dirty = false;

    uint8_t ibuf[8];
    int igot = 0;
    uint32_t itype = 0, iplen = 0;
    uint8_t payload[256] = {0};
    uint32_t ipgot = 0;
    bool running = true;
    bool lbtn_prev = false;

    while (running) {
        /* Poll: compositor socket + install pipe */
        struct pollfd pfds[2];
        pfds[0].fd = sock; pfds[0].events = POLLIN;
        pfds[1].fd = g_install_pipe; pfds[1].events = POLLIN;
        int nfds = (g_install_pipe >= 0) ? 2 : 1;
        poll(pfds, (nfds_t)nfds, g_step == STEP_PROGRESS ? 100 : 16);

        if (g_install_pipe >= 0 && (pfds[1].revents & POLLIN))
            poll_install();
        else if (g_step == STEP_PROGRESS && g_install_pipe >= 0)
            poll_install();

        if (pfds[0].revents & POLLIN) {
            uint8_t tbuf[4096];
            ssize_t n = read(sock, tbuf, sizeof(tbuf));
            if (n <= 0) break;
            int pos = 0;
            while (pos < (int)n) {
                if (igot < 8) {
                    ibuf[igot++] = tbuf[pos++];
                    if (igot == 8) {
                        memcpy(&itype, ibuf, 4); memcpy(&iplen, ibuf+4, 4);
                        if (iplen == 0) ipgot = 0;
                        else { if (iplen > sizeof(payload)) iplen = sizeof(payload); ipgot = 0; }
                    }
                } else if (iplen > 0 && ipgot < iplen) {
                    uint32_t take = iplen - ipgot;
                    if ((int)take > (int)n - pos) take = (uint32_t)((int)n - pos);
                    for (uint32_t k = 0; k < take; k++) payload[ipgot++] = tbuf[pos++];
                } else {
                    igot = 0;
                    switch (itype) {
                    case IPC_INPUT_KEY:
                        if (iplen >= 1) {
                            uint8_t key = payload[0];
                            if (key == 0x1Bu || key == 'q' || key == 'Q') running = false;
                        }
                        break;
                    case IPC_INPUT_MOUSE:
                        if (iplen >= 9) {
                            int32_t rx, ry; uint8_t btns;
                            memcpy(&rx, payload, 4); memcpy(&ry, payload+4, 4);
                            btns = payload[8];
                            bool lbtn = !!(btns & 1);
                            handle_hover((int)rx, (int)ry);
                            if (!lbtn && lbtn_prev)
                                handle_click((int)rx, (int)ry);
                            lbtn_prev = lbtn;
                        }
                        break;
                    case IPC_INVALIDATE: g_dirty = true; break;
                    case IPC_APP_CLOSE:  running = false; break;
                    }
                }
            }
        }

        if (g_dirty) {
            render(g_fb);
            send_frame(sock);
            g_dirty = false;
        }
    }

    ipc_send(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock);
    free(g_fb); free(g_glyph);
    return 0;
}
