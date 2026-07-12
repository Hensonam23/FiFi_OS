/* FiFi App Store — standalone IPC app.
 * Reads the catalog produced by appstore-sync.sh (/fifi-data/apps/catalog.tsv),
 * shows apps grouped/filterable by category with a search bar, and installs a
 * selected app by running appstore-install.sh (which downloads the .AppImage and
 * registers a desktop icon). Progress is read from <name>.status files.
 *
 * Build: gcc -O2 -static -o fifi-appstore appstore.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>

/* ── IPC protocol ────────────────────────────────────────────────────────── */
#define FIFI_SOCK       "/tmp/fifi-compositor.sock"
#define IPC_APP_CONNECT 0x01u
#define IPC_APP_FRAME   0x02u
#define IPC_APP_TITLE   0x03u
#define IPC_APP_CLOSE   0x04u
#define IPC_WIN_CREATED 0x10u
#define IPC_INPUT_KEY   0x11u
#define IPC_INPUT_MOUSE 0x12u
#define IPC_INVALIDATE  0x15u
#define IPC_NOTIFY      0x16u
#define IPC_WIN_RESIZE  0x1Bu

/* ── Window geometry ─────────────────────────────────────────────────────── */
/* The window is user-resizable: the compositor sends IPC_WIN_RESIZE and we
 * re-render at the new native size (it stops scale-blitting once our frame
 * matches the window). WIN_W/WIN_H stay as names so layout code reads well. */
#define WIN_W0   820    /* initial size */
#define WIN_H0   580
static int g_win_w = WIN_W0, g_win_h = WIN_H0;
#define WIN_W    g_win_w
#define WIN_H    g_win_h
#define TITLE_H  24
#define SRCH_H   34    /* search bar */
#define TAB_H    28    /* category tabs */
#define ITEM_H   30    /* app row */
#define FOOT_H   22
#define PAD_X    12
#define BTN_W    92    /* Install button width */

/* ── Colours (0x00RRGGBB) ────────────────────────────────────────────────── */
#define C_BG      0x00121820u
#define C_BAR     0x001a2432u
#define C_BORDER  0x00243448u
#define C_WHITE   0x00f0f0f0u
#define C_GREY    0x00708090u
#define C_ACCENT  0x003878d8u
#define C_NAME    0x00d8e4f0u
#define C_CATTXT  0x0080a0c0u
#define C_BTN     0x00306840u
#define C_BTN_HOV 0x00409850u
#define C_BTN_DIS 0x00404850u
#define C_TAB     0x001e2836u
#define C_TAB_SEL 0x003878d8u
#define C_ROWSEL  0x001b2740u
#define C_RUN     0x0040b060u   /* running badge */
#define C_HDR     0x005890c8u   /* section header text */
#define C_UPDATE  0x00c07820u   /* update-available button (orange) */

/* ── PSF1 font (loaded from compositor fonts) ────────────────────────────── */
#define PSF1_MAGIC 0x0436u
typedef struct { uint16_t magic; uint8_t mode; uint8_t charsize; } Psf1Hdr;
static uint8_t *g_glyph = NULL;
static int g_glyph_h = 16, g_n_glyphs = 256;

static bool font_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    Psf1Hdr h;
    if (read(fd, &h, sizeof h) != sizeof h || h.magic != PSF1_MAGIC) { close(fd); return false; }
    g_glyph_h = h.charsize;
    g_n_glyphs = (h.mode & 1) ? 512 : 256;
    int total = g_n_glyphs * g_glyph_h;
    g_glyph = malloc(total);
    if (!g_glyph) { close(fd); return false; }
    if (read(fd, g_glyph, total) < total) { free(g_glyph); g_glyph = NULL; close(fd); return false; }
    close(fd);
    return true;
}
static void draw_char(uint32_t *fb, int c, int px, int py, uint32_t fg) {
    if (!g_glyph || c < 0 || c >= g_n_glyphs) return;
    const uint8_t *b = g_glyph + c * g_glyph_h;
    for (int row = 0; row < g_glyph_h; row++)
        for (int col = 0; col < 8; col++)
            if (b[row] & (0x80u >> col)) {
                int x = px + col, y = py + row;
                if (x >= 0 && x < WIN_W && y >= 0 && y < WIN_H) fb[y*WIN_W+x] = fg;
            }
}
static void draw_str(uint32_t *fb, const char *s, int x, int y, uint32_t fg) {
    for (; *s; s++, x += 9) { if (x > WIN_W) break; draw_char(fb, (unsigned char)*s, x, y, fg); }
}
static void draw_strn(uint32_t *fb, const char *s, int n, int x, int y, uint32_t fg) {
    for (int i = 0; i < n && s[i]; i++, x += 9) draw_char(fb, (unsigned char)s[i], x, y, fg);
}
/* 2x-scale text (detail-view app name). */
static void draw_char2(uint32_t *fb, int c, int px, int py, uint32_t fg) {
    if (!g_glyph || c < 0 || c >= g_n_glyphs) return;
    const uint8_t *b = g_glyph + c * g_glyph_h;
    for (int row = 0; row < g_glyph_h; row++)
        for (int col = 0; col < 8; col++)
            if (b[row] & (0x80u >> col))
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        int x = px + col*2 + dx, y = py + row*2 + dy;
                        if (x >= 0 && x < WIN_W && y >= 0 && y < WIN_H) fb[y*WIN_W+x] = fg;
                    }
}
static void draw_str2(uint32_t *fb, const char *s, int x, int y, uint32_t fg) {
    for (; *s; s++, x += 18) { if (x > WIN_W - 18) break; draw_char2(fb, (unsigned char)*s, x, y, fg); }
}
/* Word-wrapped paragraph; returns y after the last line drawn. */
static int draw_wrapped(uint32_t *fb, const char *s, int x, int y,
                        int max_chars, int max_lines, uint32_t fg) {
    int line = 0;
    while (*s && line < max_lines) {
        while (*s == ' ') s++;
        int len = (int)strlen(s);
        int take = len < max_chars ? len : max_chars;
        if (take < len) {                 /* break at the last space that fits */
            int br = take;
            while (br > 0 && s[br] != ' ') br--;
            if (br > max_chars / 2) take = br;
        }
        draw_strn(fb, s, take, x, y + line * (g_glyph_h + 3), fg);
        s += take;
        line++;
    }
    return y + line * (g_glyph_h + 3);
}
static void fill(uint32_t *fb, int x, int y, int w, int h, uint32_t c) {
    for (int r = y; r < y+h; r++) { if (r < 0 || r >= WIN_H) continue;
        int a = x < 0 ? 0 : x, b = x+w > WIN_W ? WIN_W : x+w;
        for (int cc = a; cc < b; cc++) fb[r*WIN_W+cc] = c; }
}
static void hline(uint32_t *fb, int y, int x0, int x1, uint32_t c) {
    if (y < 0 || y >= WIN_H) return;
    for (int x = x0; x < x1 && x < WIN_W; x++) if (x >= 0) fb[y*WIN_W+x] = c;
}
/* Colored-initial placeholder for apps with no icon yet. */
static void draw_icon_ph(uint32_t *fb, const char *name, int dx, int dy, int dsz) {
    uint32_t hash = 2166136261u;
    for (const char *p = name; *p; p++) hash = (hash ^ (uint8_t)*p) * 16777619u;
    uint32_t col = 0x00304050u | ((hash & 0x7F) << 16) | ((hash >> 7 & 0x3F) << 8) | (hash >> 13 & 0x3F);
    fill(fb, dx, dy, dsz, dsz, col);
    char c = (char)toupper((unsigned char)name[0]);
    if (dsz >= 32) draw_char2(fb, c, dx + (dsz-16)/2, dy + (dsz-32)/2, C_WHITE);
    else draw_char(fb, c, dx + (dsz-8)/2, dy + (dsz-g_glyph_h)/2, C_WHITE);
}

/* ── Catalog ─────────────────────────────────────────────────────────────── */
#define MAX_APPS 1400
#define ICON_DIR  "/fifi-data/apps/icons"
#define ICON_BASE "https://appimage.github.io/database/"
typedef struct {
    char name[48];
    char cat[24];
    char repo[80];
    char icon[96];     /* feed icon path (under ICON_BASE), "" = none */
    char desc[512];
    char status[12];   /* "", installing, downloading, done, error */
    bool installing;
    uint8_t icon_state; /* 0=not tried, 1=downloading, 2=unavailable */
} App;
static App g_apps[MAX_APPS];
static int g_napps = 0;
static int g_view[MAX_APPS];   /* filtered indices */
static int g_nview = 0;
static int g_scroll = 0;

static char g_search[48];
static int  g_slen = 0;
static char g_cat[24] = "";    /* "" = All */

/* Category tabs (short label -> feed category). Chosen to fit one row + cover the bulk. */
static const char *TABS[][2] = {
    {"All",""},{"Utility","Utility"},{"Dev","Development"},{"Net","Network"},
    {"Office","Office"},{"Graphics","Graphics"},{"Games","Game"},{"Media","AudioVideo"},
    {"Science","Science"},{"System","System"},
};
#define NTABS ((int)(sizeof(TABS)/sizeof(TABS[0])))
static int g_tab_x[NTABS], g_tab_w[NTABS];   /* computed at render for click hit-test */
static int g_instab_x, g_instab_w;           /* right-aligned "Installed" tab */

/* Copy src into dst dropping non-ASCII bytes (the PSF renderer is ASCII-only). */
static void ascii_copy(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (; *src && o + 1 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c >= 0x20 && c < 0x7F) dst[o++] = (char)c;
        else if (o && dst[o-1] != ' ' && (c == '\t' || c >= 0x80)) dst[o++] = ' ';
    }
    dst[o] = '\0';
}

static void load_catalog(void) {
    g_napps = 0;
    FILE *f = fopen("/fifi-data/apps/catalog.tsv", "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f) && g_napps < MAX_APPS) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *t1 = strchr(line, '\t'); if (!t1) continue; *t1 = '\0';
        char *t2 = strchr(t1+1, '\t'); if (!t2) continue; *t2 = '\0';
        char *t3 = strchr(t2+1, '\t'); if (t3) *t3 = '\0';
        char *t4 = t3 ? strchr(t3+1, '\t') : NULL; if (t4) *t4 = '\0';
        App *a = &g_apps[g_napps];
        snprintf(a->name, sizeof a->name, "%s", line);
        snprintf(a->cat,  sizeof a->cat,  "%s", t1+1);
        snprintf(a->repo, sizeof a->repo, "%s", t2+1);
        snprintf(a->icon, sizeof a->icon, "%s", t3 ? t3+1 : "");
        ascii_copy(a->desc, sizeof a->desc, t4 ? t4+1 : "");
        a->status[0] = '\0'; a->installing = false; a->icon_state = 0;
        /* already installed from an earlier session? */
        char ap[160]; snprintf(ap, sizeof ap, "/fifi-data/apps/%s.AppImage", a->name);
        struct stat st;
        if (stat(ap, &st) == 0) snprintf(a->status, sizeof a->status, "done");
        g_napps++;
    }
    fclose(f);
}

static bool ci_contains(const char *hay, const char *needle) {
    if (!*needle) return true;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}
static void rebuild_view(void) {
    g_nview = 0;
    for (int i = 0; i < g_napps; i++) {
        if (g_cat[0] && strcmp(g_apps[i].cat, g_cat) != 0) continue;
        if (g_slen && !ci_contains(g_apps[i].name, g_search)) continue;
        g_view[g_nview++] = i;
    }
    g_scroll = 0;
}

/* ── App icons (PNG logos extracted at install time) ─────────────────────── */
#include "../../platform/linux/vendor/lodepng.h"

typedef struct { char name[64]; uint32_t *img; unsigned w, h; bool tried; } AppIcon;
#define MAX_ICONS 512
static AppIcon g_icons[MAX_ICONS];
static int g_nicons = 0;
static int g_icon_evict = 0;

/* Decode a PNG into ARGB, box-downsampled to at most 128px a side (feed icons
 * can be 512x512; keeping them full-size would blow the cache). */
static uint32_t *decode_icon(const char *path, unsigned *ow, unsigned *oh) {
    unsigned char *rgba = NULL; unsigned w = 0, h = 0;
    if (lodepng_decode32_file(&rgba, &w, &h, path) != 0 || !rgba || !w || !h) {
        free(rgba); return NULL;
    }
    unsigned dw = w, dh = h;
    while (dw > 128 || dh > 128) { dw = (dw + 1) / 2; dh = (dh + 1) / 2; }
    uint32_t *img = malloc((size_t)dw * dh * 4u);
    if (img) {
        for (unsigned y = 0; y < dh; y++)
            for (unsigned x = 0; x < dw; x++) {
                size_t s = ((size_t)((uint64_t)y * h / dh) * w + (uint64_t)x * w / dw) * 4u;
                img[y*dw+x] = ((uint32_t)rgba[s+3] << 24) | ((uint32_t)rgba[s+0] << 16)
                            | ((uint32_t)rgba[s+1] << 8)  |  (uint32_t)rgba[s+2];
            }
        *ow = dw; *oh = dh;
    }
    free(rgba);
    return img;
}

/* Load (and cache) an app icon: installer-extracted PNG first, then the
 * store-downloaded feed icon. Misses are cached too (cleared by _forget). */
static AppIcon *app_icon(const char *name) {
    for (int i = 0; i < g_nicons; i++)
        if (!strcmp(g_icons[i].name, name))
            return g_icons[i].img ? &g_icons[i] : NULL;
    AppIcon *ic;
    if (g_nicons < MAX_ICONS) ic = &g_icons[g_nicons++];
    else {                                  /* round-robin eviction */
        ic = &g_icons[g_icon_evict];
        g_icon_evict = (g_icon_evict + 1) % MAX_ICONS;
        free(ic->img); ic->img = NULL;
    }
    snprintf(ic->name, sizeof ic->name, "%s", name);
    char path[224];
    snprintf(path, sizeof path, "/fifi-data/apps/%s.png", name);
    ic->img = decode_icon(path, &ic->w, &ic->h);
    if (!ic->img) {
        snprintf(path, sizeof path, ICON_DIR "/%s.png", name);
        ic->img = decode_icon(path, &ic->w, &ic->h);
    }
    ic->tried = true;
    return ic->img ? ic : NULL;
}

/* Drop a cached miss so the next app_icon() re-reads the file. */
static void app_icon_forget(const char *name) {
    for (int i = 0; i < g_nicons; i++)
        if (!strcmp(g_icons[i].name, name)) {
            free(g_icons[i].img);
            g_icons[i] = g_icons[--g_nicons];
            if (g_icon_evict >= g_nicons && g_nicons) g_icon_evict %= g_nicons;
            return;
        }
}

/* ── Lazy feed-icon downloads (for browse rows + detail view) ────────────── */
static int g_icon_dl = 0;   /* curls in flight */

/* Start fetching an app's feed icon in the background (max 4 at once). */
static void icon_dl_start(App *a) {
    if (a->icon_state != 0 || !a->icon[0] || g_icon_dl >= 4) return;
    char fin[224], fail[240];
    snprintf(fin, sizeof fin, ICON_DIR "/%s.png", a->name);
    snprintf(fail, sizeof fail, "%s.fail", fin);
    struct stat st;
    if (stat(fin, &st) == 0) return;                       /* already there */
    if (stat(fail, &st) == 0) { a->icon_state = 2; return; } /* known-bad */
    a->icon_state = 1;
    g_icon_dl++;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul,0); dup2(nul,1); dup2(nul,2); }
        char cmd[640];
        snprintf(cmd, sizeof cmd,
            "mkdir -p " ICON_DIR "; "
            "curl -sL --max-time 30 -o '%s.part' '" ICON_BASE "%s' "
            "&& mv '%s.part' '%s' || { rm -f '%s.part'; : > '%s'; }",
            fin, a->icon, fin, fin, fin, fail);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    if (pid < 0) { a->icon_state = 0; g_icon_dl--; }
}

/* Poll in-flight icon downloads; true if any finished (needs redraw). */
static bool icon_dl_poll(void) {
    bool changed = false;
    for (int i = 0; i < g_napps; i++) {
        if (g_apps[i].icon_state != 1) continue;
        char fin[224], fail[240]; struct stat st;
        snprintf(fin, sizeof fin, ICON_DIR "/%s.png", g_apps[i].name);
        snprintf(fail, sizeof fail, "%s.fail", fin);
        if (stat(fin, &st) == 0) {
            g_apps[i].icon_state = 0;
            if (g_icon_dl > 0) g_icon_dl--;
            app_icon_forget(g_apps[i].name);
            changed = true;
        } else if (stat(fail, &st) == 0) {
            g_apps[i].icon_state = 2;
            if (g_icon_dl > 0) g_icon_dl--;
        }
    }
    return changed;
}

/* Alpha-blend scale-blit an ARGB icon into the window framebuffer. */
static void draw_icon(uint32_t *fb, AppIcon *ic, int dx, int dy, int dsz) {
    for (int y = 0; y < dsz; y++) {
        int py = dy + y; if (py < 0 || py >= WIN_H) continue;
        const uint32_t *srow = ic->img + (size_t)((uint64_t)y * ic->h / dsz) * ic->w;
        for (int x = 0; x < dsz; x++) {
            int px = dx + x; if (px < 0 || px >= WIN_W) continue;
            uint32_t s = srow[(uint64_t)x * ic->w / dsz];
            uint32_t a = s >> 24;
            if (a == 0) continue;
            if (a >= 0xF8u) { fb[py*WIN_W+px] = s & 0x00ffffffu; continue; }
            uint32_t ia = 255u - a, d = fb[py*WIN_W+px];
            uint32_t r = ((((s>>16)&0xffu)*a) + (((d>>16)&0xffu)*ia)) >> 8;
            uint32_t g = ((((s>>8)&0xffu)*a)  + (((d>>8)&0xffu)*ia))  >> 8;
            uint32_t b = (((s&0xffu)*a)       + ((d&0xffu)*ia))       >> 8;
            fb[py*WIN_W+px] = (r<<16)|(g<<8)|b;
        }
    }
}

/* ── Installed apps + running services (the "Installed" view) ────────────── */
static void send_msg(int fd, uint32_t type, const void *d, uint32_t len);
#define MAX_INST 128
typedef struct {
    char name[64];
    char path[192];      /* .AppImage path */
    long size_mb;
    bool running;
    bool has_update;     /* <Name>.update marker present */
} Inst;
static Inst g_inst[MAX_INST];
static int  g_ninst = 0;

#define MAX_SVC 96
typedef struct { char comm[32]; int pid; int count; } Svc;
static Svc g_svc[MAX_SVC];
static int g_nsvc = 0;

static bool g_installed_mode = false;

/* Row list for the Installed view: headers + apps + services. */
typedef struct { int kind; int idx; } IRow;   /* kind: 0=header-apps 1=app 2=header-svc 3=svc */
static IRow g_irows[2 + MAX_INST + MAX_SVC];
static int  g_nirows = 0;
static int  g_chkupd_x = 0, g_chkupd_y = 0, g_chkupd_w = 0;  /* Check Updates button rect */

/* True if some process was launched from this AppImage's extracted dir. Checks
 * the exe + cwd symlinks (which resolve INTO the app dir even when the app is
 * run with a relative cmdline, e.g. LibreOffice's "./soffice.bin") as well as
 * the raw cmdline. */
static bool proc_matches(const char *pid, const char *needle) {
    char p[64], link[512];
    snprintf(p, sizeof p, "/proc/%s/exe", pid);
    ssize_t ln = readlink(p, link, sizeof link - 1);
    if (ln > 0) { link[ln] = '\0'; if (strstr(link, needle)) return true; }
    snprintf(p, sizeof p, "/proc/%s/cwd", pid);
    ln = readlink(p, link, sizeof link - 1);
    if (ln > 0) { link[ln] = '\0'; if (strstr(link, needle)) return true; }
    snprintf(p, sizeof p, "/proc/%s/cmdline", pid);
    int fd = open(p, O_RDONLY); if (fd < 0) return false;
    char buf[512]; ssize_t n = read(fd, buf, sizeof buf - 1); close(fd);
    if (n <= 0) return false;
    for (ssize_t i = 0; i < n; i++) if (!buf[i]) buf[i] = ' ';
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}
static bool proc_scan(const char *needle) {
    DIR *d = opendir("/proc");
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        if (proc_matches(e->d_name, needle)) found = true;
    }
    closedir(d);
    return found;
}
/* Kill every process launched from an app's extracted dir (force close). */
static int proc_kill_all(const char *needle) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e; int killed = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        if (proc_matches(e->d_name, needle)) { kill(atoi(e->d_name), SIGKILL); killed++; }
    }
    closedir(d);
    return killed;
}

static void scan_installed(void) {
    g_ninst = 0;
    DIR *d = opendir("/fifi-data/apps");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && g_ninst < MAX_INST) {
            size_t l = strlen(e->d_name);
            if (l < 9 || strcmp(e->d_name + l - 9, ".AppImage")) continue;
            Inst *in = &g_inst[g_ninst];
            snprintf(in->name, sizeof in->name, "%.*s", (int)(l - 9), e->d_name);
            snprintf(in->path, sizeof in->path, "/fifi-data/apps/%s", e->d_name);
            struct stat st;
            in->size_mb = (stat(in->path, &st) == 0) ? (long)(st.st_size >> 20) : 0;
            char needle[224]; snprintf(needle, sizeof needle, "/fifi-data/apps/%s.d/", in->name);
            in->running = proc_scan(needle);
            char up[224]; snprintf(up, sizeof up, "/fifi-data/apps/%s.update", in->name);
            struct stat ust; in->has_update = (stat(up, &ust) == 0);
            g_ninst++;
        }
        closedir(d);
    }

    /* Running services: userspace processes deduped by comm (a light task list). */
    g_nsvc = 0;
    DIR *pd = opendir("/proc");
    if (pd) {
        struct dirent *e;
        while ((e = readdir(pd))) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            char p[64]; snprintf(p, sizeof p, "/proc/%s/cmdline", e->d_name);
            int fd = open(p, O_RDONLY); if (fd < 0) continue;
            char c1; ssize_t n = read(fd, &c1, 1); close(fd);
            if (n <= 0) continue;               /* kernel thread */
            snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
            fd = open(p, O_RDONLY); if (fd < 0) continue;
            char comm[32] = {0}; n = read(fd, comm, sizeof comm - 1); close(fd);
            if (n <= 0) continue;
            if (comm[n-1] == '\n') comm[n-1] = '\0';
            int i;
            for (i = 0; i < g_nsvc; i++)
                if (!strcmp(g_svc[i].comm, comm)) { g_svc[i].count++; break; }
            if (i == g_nsvc && g_nsvc < MAX_SVC) {
                snprintf(g_svc[g_nsvc].comm, sizeof g_svc[g_nsvc].comm, "%s", comm);
                g_svc[g_nsvc].pid = atoi(e->d_name);
                g_svc[g_nsvc].count = 1;
                g_nsvc++;
            }
        }
        closedir(pd);
    }

    /* Build row list (apply search filter to both sections). */
    g_nirows = 0;
    g_irows[g_nirows].kind = 0; g_irows[g_nirows].idx = 0; g_nirows++;
    for (int i = 0; i < g_ninst; i++) {
        if (g_slen && !ci_contains(g_inst[i].name, g_search)) continue;
        g_irows[g_nirows].kind = 1; g_irows[g_nirows].idx = i; g_nirows++;
    }
    g_irows[g_nirows].kind = 2; g_irows[g_nirows].idx = 0; g_nirows++;
    for (int i = 0; i < g_nsvc; i++) {
        if (g_slen && !ci_contains(g_svc[i].comm, g_search)) continue;
        g_irows[g_nirows].kind = 3; g_irows[g_nirows].idx = i; g_nirows++;
    }
}

/* Uninstall an installed app (fork appstore-uninstall.sh). The periodic rescan
 * drops the row once the files are gone. */
static void start_uninstall(int fd, int i) {
    char note[96];
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul,0); dup2(nul,1); dup2(nul,2); }
        execl("/bin/sh", "sh", "/fifi-data/apps/appstore-uninstall.sh",
              g_inst[i].name, (char*)NULL);
        _exit(127);
    }
    uint32_t nl = (uint32_t)snprintf(note, sizeof note, "Removing %s...", g_inst[i].name);
    send_msg(fd, IPC_NOTIFY, note, nl);
}

/* Check all installed apps for updates (fork appstore-update-check.sh). */
static void start_check_updates(int fd) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul,0); dup2(nul,1); dup2(nul,2); }
        execl("/bin/sh", "sh", "/fifi-data/apps/appstore-update-check.sh", (char*)NULL);
        _exit(127);
    }
    const char *m = "Checking for updates...";
    send_msg(fd, IPC_NOTIFY, m, (uint32_t)strlen(m));
}

/* Update an installed app: re-run the installer with its recorded source. */
static void start_update(int fd, int i) {
    char cmd[256];
    snprintf(cmd, sizeof cmd,
             "sh /fifi-data/apps/appstore-install.sh \"$(cat /fifi-data/apps/%s.src)\" \"%s\"",
             g_inst[i].name, g_inst[i].name);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul,0); dup2(nul,1); dup2(nul,2); }
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    char note[96];
    uint32_t nl = (uint32_t)snprintf(note, sizeof note, "Updating %s...", g_inst[i].name);
    send_msg(fd, IPC_NOTIFY, note, nl);
}

/* Launch an installed app through the unified fifi-run launcher, detached. */
static void launch_app(int fd, int i) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execl("/fifi-data/apps/fifi-run", "fifi-run", g_inst[i].path, (char*)NULL);
        _exit(127);
    }
    char note[96];
    uint32_t nl = (uint32_t)snprintf(note, sizeof note, "Launching %s...", g_inst[i].name);
    send_msg(fd, IPC_NOTIFY, note, nl);
}

/* ── Detail view ─────────────────────────────────────────────────────────── */
static int g_detail = -1;                       /* catalog index, -1 = list */
static int g_dt_back[4], g_dt_btn[4], g_dt_rm[4];   /* x,y,w,h hit rects */

static bool detail_installed(App *a) {
    char ap[160]; struct stat st;
    snprintf(ap, sizeof ap, "/fifi-data/apps/%s.AppImage", a->name);
    return stat(ap, &st) == 0;
}

static void open_detail(int ai) {
    g_detail = ai;
    App *a = &g_apps[ai];
    if (!app_icon(a->name) && a->icon_state == 0) icon_dl_start(a);
}

/* Launch/uninstall by app name (detail view works from the catalog, not g_inst). */
static void launch_by_name(int fd, const char *name) {
    char path[192]; snprintf(path, sizeof path, "/fifi-data/apps/%s.AppImage", name);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul,0); dup2(nul,1); dup2(nul,2); }
        execl("/fifi-data/apps/fifi-run", "fifi-run", path, (char*)NULL);
        _exit(127);
    }
    char note[96];
    uint32_t nl = (uint32_t)snprintf(note, sizeof note, "Launching %s...", name);
    send_msg(fd, IPC_NOTIFY, note, nl);
}
static void uninstall_by_name(int fd, const char *name) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul,0); dup2(nul,1); dup2(nul,2); }
        execl("/bin/sh", "sh", "/fifi-data/apps/appstore-uninstall.sh", name, (char*)NULL);
        _exit(127);
    }
    char note[96];
    uint32_t nl = (uint32_t)snprintf(note, sizeof note, "Removing %s...", name);
    send_msg(fd, IPC_NOTIFY, note, nl);
}

/* Human-readable source line for an app's install spec. */
static void detail_source(App *a, char *out, size_t cap) {
    if (!strncmp(a->repo, "url:", 4)) {
        snprintf(out, cap, "Source: direct from the publisher");
    } else if (!strncmp(a->repo, "gitlab:", 7)) {
        char proj[96]; size_t o = 0;
        for (const char *p = a->repo + 7; *p && o + 1 < sizeof proj; ) {
            if (p[0] == '%' && p[1] == '2' && (p[2] == 'F' || p[2] == 'f')) { proj[o++] = '/'; p += 3; }
            else proj[o++] = *p++;
        }
        proj[o] = '\0';
        snprintf(out, cap, "Source: gitlab.com/%s", proj);
    } else {
        snprintf(out, cap, "Source: github.com/%s", a->repo);
    }
}

static void render_detail(uint32_t *fb) {
    App *a = &g_apps[g_detail];
    fill(fb, 0, 0, WIN_W, WIN_H, C_BG);

    /* header strip with Back */
    int hy = TITLE_H;
    fill(fb, 0, hy, WIN_W, 40, C_BAR);
    hline(fb, hy + 39, 0, WIN_W, C_BORDER);
    g_dt_back[0] = PAD_X; g_dt_back[1] = hy + 7; g_dt_back[2] = 6*9 + 18; g_dt_back[3] = 26;
    fill(fb, g_dt_back[0], g_dt_back[1], g_dt_back[2], g_dt_back[3], C_TAB);
    draw_str(fb, "< Back", g_dt_back[0] + 9, hy + (40 - g_glyph_h)/2, C_WHITE);
    draw_str(fb, a->cat, WIN_W - PAD_X - (int)strlen(a->cat)*9, hy + (40 - g_glyph_h)/2, C_CATTXT);

    /* icon + name block */
    int iy = hy + 56, isz = 96;
    AppIcon *ic = app_icon(a->name);
    if (ic) draw_icon(fb, ic, PAD_X, iy, isz);
    else draw_icon_ph(fb, a->name, PAD_X, iy, isz);
    int tx = PAD_X + isz + 20;
    draw_str2(fb, a->name, tx, iy + 4, C_WHITE);
    char src[144]; detail_source(a, src, sizeof src);
    draw_str(fb, src, tx, iy + 44, C_GREY);
    char cl[48]; snprintf(cl, sizeof cl, "Category: %s", a->cat[0] ? a->cat : "Other");
    draw_str(fb, cl, tx, iy + 66, C_GREY);

    /* action buttons */
    bool inst = detail_installed(a);
    int bw = 130, bh = 34;
    int bx = WIN_W - PAD_X - bw, by = iy + isz - bh;
    const char *lbl; uint32_t bc;
    if (a->installing) {
        lbl = !strcmp(a->status, "downloading") ? "Downloading" : "Installing";
        bc = C_ACCENT;
    } else if (inst)                        { lbl = "Launch";  bc = C_RUN; }
    else if (!strcmp(a->status, "error"))   { lbl = "Retry";   bc = 0x00803030u; }
    else                                    { lbl = "Install"; bc = C_BTN; }
    g_dt_btn[0] = bx; g_dt_btn[1] = by; g_dt_btn[2] = bw; g_dt_btn[3] = bh;
    fill(fb, bx, by, bw, bh, bc);
    draw_str(fb, lbl, bx + (bw - (int)strlen(lbl)*9)/2, by + (bh - g_glyph_h)/2, C_WHITE);
    if (inst && !a->installing) {           /* Remove above Launch */
        g_dt_rm[0] = bx; g_dt_rm[1] = by - bh - 8; g_dt_rm[2] = bw; g_dt_rm[3] = bh;
        fill(fb, bx, g_dt_rm[1], bw, bh, 0x00803038u);
        draw_str(fb, "Remove", bx + (bw - 6*9)/2, g_dt_rm[1] + (bh - g_glyph_h)/2, C_WHITE);
    } else g_dt_rm[2] = 0;

    /* description */
    int dy = iy + isz + 24;
    hline(fb, dy - 12, PAD_X, WIN_W - PAD_X, C_BORDER);
    if (a->desc[0]) {
        int max_ch = (WIN_W - 2*PAD_X) / 9;
        int max_ln = (WIN_H - FOOT_H - dy) / (g_glyph_h + 3);
        draw_wrapped(fb, a->desc, PAD_X, dy, max_ch, max_ln, C_NAME);
    } else {
        draw_str(fb, "No description available.", PAD_X, dy, C_GREY);
    }

    /* footer */
    int fy = WIN_H - FOOT_H;
    fill(fb, 0, fy, WIN_W, FOOT_H, C_BAR);
    hline(fb, fy, 0, WIN_W, C_BORDER);
    const char *hint = inst ? "  installed on this system" : "  click Install to download from the source";
    draw_str(fb, hint, 0, fy + (FOOT_H - g_glyph_h)/2, C_GREY);
}

static int list_top(void) { return TITLE_H + SRCH_H + TAB_H; }
static int visible_rows(void) { return (WIN_H - list_top() - FOOT_H) / ITEM_H; }
static void clamp_scroll(void) {
    int rows = g_installed_mode ? g_nirows : g_nview;
    int max = rows - visible_rows();
    if (g_scroll > max) g_scroll = max;
    if (g_scroll < 0) g_scroll = 0;
}

/* ── Render ──────────────────────────────────────────────────────────────── */
static void render(uint32_t *fb) {
    if (g_detail >= 0) { render_detail(fb); return; }
    fill(fb, 0, 0, WIN_W, WIN_H, C_BG);

    /* Search bar */
    int sy = TITLE_H;
    fill(fb, 0, sy, WIN_W, SRCH_H, C_BAR);
    hline(fb, sy + SRCH_H - 1, 0, WIN_W, C_BORDER);
    draw_str(fb, "Search:", PAD_X, sy + (SRCH_H - g_glyph_h)/2, C_GREY);
    int box_x = PAD_X + 8*9, box_w = WIN_W - box_x - PAD_X;
    fill(fb, box_x, sy + 5, box_w, SRCH_H - 10, 0x00202c3cu);
    hline(fb, sy + 5, box_x, box_x + box_w, C_BORDER);
    if (g_slen) draw_str(fb, g_search, box_x + 6, sy + (SRCH_H - g_glyph_h)/2, C_WHITE);
    else draw_str(fb, "type to filter apps...", box_x + 6, sy + (SRCH_H - g_glyph_h)/2, C_GREY);
    /* cursor */
    int cx = box_x + 6 + g_slen*9;
    fill(fb, cx, sy + 6, 2, SRCH_H - 12, C_ACCENT);

    /* Category tabs + right-aligned Installed tab */
    int ty = TITLE_H + SRCH_H;
    fill(fb, 0, ty, WIN_W, TAB_H, 0x000e141cu);
    hline(fb, ty + TAB_H - 1, 0, WIN_W, C_BORDER);
    int tx = PAD_X;
    for (int i = 0; i < NTABS; i++) {
        int w = (int)strlen(TABS[i][0]) * 9 + 16;
        g_tab_x[i] = tx; g_tab_w[i] = w;
        bool sel = !g_installed_mode && strcmp(g_cat, TABS[i][1]) == 0;
        fill(fb, tx, ty + 3, w, TAB_H - 6, sel ? C_TAB_SEL : C_TAB);
        draw_str(fb, TABS[i][0], tx + 8, ty + (TAB_H - g_glyph_h)/2, sel ? C_WHITE : C_GREY);
        tx += w + 4;
    }
    g_instab_w = 9*9 + 16;                     /* "Installed" */
    g_instab_x = WIN_W - g_instab_w - PAD_X;
    fill(fb, g_instab_x, ty + 3, g_instab_w, TAB_H - 6,
         g_installed_mode ? C_RUN : 0x00204030u);
    draw_str(fb, "Installed", g_instab_x + 8, ty + (TAB_H - g_glyph_h)/2,
             g_installed_mode ? C_WHITE : 0x0090c8a0u);

    if (g_installed_mode) {
        /* Installed apps + running services */
        int lt = list_top(), vis = visible_rows();
        for (int r = 0; r < vis; r++) {
            int vi = g_scroll + r;
            if (vi >= g_nirows) break;
            IRow *ir = &g_irows[vi];
            int ry = lt + r * ITEM_H;
            if (ir->kind == 0 || ir->kind == 2) {
                fill(fb, 0, ry, WIN_W, ITEM_H, 0x000e161eu);
                draw_str(fb, ir->kind == 0 ? "INSTALLED APPS" : "RUNNING SERVICES",
                         PAD_X, ry + (ITEM_H - g_glyph_h)/2, C_HDR);
                if (ir->kind == 0 && g_ninst == 0)
                    draw_str(fb, "(none yet - install from the store tabs)",
                             PAD_X + 16*9, ry + (ITEM_H - g_glyph_h)/2, C_GREY);
                /* "Check Updates" button on the INSTALLED APPS header */
                if (ir->kind == 0 && g_ninst > 0) {
                    g_chkupd_w = 13*9 + 14;
                    g_chkupd_x = WIN_W - g_chkupd_w - PAD_X;
                    g_chkupd_y = ry + 3;
                    fill(fb, g_chkupd_x, g_chkupd_y, g_chkupd_w, ITEM_H - 6, C_BTN);
                    draw_str(fb, "Check Updates", g_chkupd_x + 7,
                             ry + (ITEM_H - g_glyph_h)/2, C_WHITE);
                } else if (ir->kind == 0) {
                    g_chkupd_w = 0;
                }
            } else if (ir->kind == 1) {
                Inst *in = &g_inst[ir->idx];
                if (r & 1) fill(fb, 0, ry, WIN_W, ITEM_H, 0x00161e28u);
                AppIcon *ic = app_icon(in->name);
                if (ic) draw_icon(fb, ic, PAD_X + 4, ry + 3, ITEM_H - 6);
                draw_str(fb, in->name, PAD_X + 8 + ITEM_H, ry + (ITEM_H - g_glyph_h)/2, C_NAME);
                char sz[24]; snprintf(sz, sizeof sz, "%ld MB", in->size_mb);
                draw_str(fb, sz, WIN_W - BTN_W - 24 - 130, ry + (ITEM_H - g_glyph_h)/2, C_CATTXT);
                if (in->running)
                    draw_str(fb, "RUNNING", WIN_W - BTN_W - 24 - 220,
                             ry + (ITEM_H - g_glyph_h)/2, C_RUN);
                int bx = WIN_W - BTN_W - PAD_X, by = ry + 3, bh = ITEM_H - 6;
                /* Main button always launches (reopens a running app); Update if
                 * an update is pending. A separate red Close button force-quits a
                 * running app. */
                uint32_t bcol = in->has_update ? C_UPDATE : C_ACCENT;
                const char *lbl = in->has_update ? "Update" : "Launch";
                fill(fb, bx, by, BTN_W, bh, bcol);
                draw_str(fb, lbl, bx + (BTN_W - (int)strlen(lbl)*9)/2,
                         ry + (ITEM_H - g_glyph_h)/2, C_WHITE);
                /* Remove (uninstall) button to the left of Launch */
                int rmx = bx - 76 - 6;
                fill(fb, rmx, by, 76, bh, 0x00803038u);
                /* Close (force-quit) button, left of Remove, only when running */
                if (in->running) {
                    int cbx = rmx - 66 - 6;
                    fill(fb, cbx, by, 66, bh, 0x00b04030u);
                    draw_str(fb, "Close", cbx + (66 - 5*9)/2,
                             ry + (ITEM_H - g_glyph_h)/2, C_WHITE);
                }
                draw_str(fb, "Remove", rmx + (76 - 6*9)/2,
                         ry + (ITEM_H - g_glyph_h)/2, C_WHITE);
            } else {
                Svc *s = &g_svc[ir->idx];
                if (r & 1) fill(fb, 0, ry, WIN_W, ITEM_H, 0x00161e28u);
                draw_str(fb, s->comm, PAD_X + 8, ry + (ITEM_H - g_glyph_h)/2, 0x00b0c0d0u);
                char info[40];
                snprintf(info, sizeof info, s->count > 1 ? "pid %d  x%d" : "pid %d",
                         s->pid, s->count);
                draw_str(fb, info, WIN_W - PAD_X - (int)strlen(info)*9,
                         ry + (ITEM_H - g_glyph_h)/2, C_GREY);
            }
            hline(fb, ry + ITEM_H - 1, 0, WIN_W, 0x001a2230u);
        }
        int fy2 = WIN_H - FOOT_H;
        fill(fb, 0, fy2, WIN_W, FOOT_H, C_BAR);
        hline(fb, fy2, 0, WIN_W, C_BORDER);
        char foot2[96];
        snprintf(foot2, sizeof foot2, "  %d app%s installed - %d services running",
                 g_ninst, g_ninst == 1 ? "" : "s", g_nsvc);
        draw_str(fb, foot2, 0, fy2 + (FOOT_H - g_glyph_h)/2, C_GREY);
        return;
    }

    /* App list */
    int lt = list_top(), vis = visible_rows();
    for (int r = 0; r < vis; r++) {
        int vi = g_scroll + r;
        if (vi >= g_nview) break;
        App *a = &g_apps[g_view[vi]];
        int ry = lt + r * ITEM_H;
        if (r & 1) fill(fb, 0, ry, WIN_W, ITEM_H, 0x00161e28u);

        /* icon (installed PNG / downloaded feed logo / colored placeholder) */
        AppIcon *ic = app_icon(a->name);
        if (!ic && a->icon_state == 0) icon_dl_start(a);
        if (ic) draw_icon(fb, ic, PAD_X, ry + 3, ITEM_H - 6);
        else draw_icon_ph(fb, a->name, PAD_X, ry + 3, ITEM_H - 6);

        /* name */
        int nx = PAD_X + ITEM_H + 4;
        int max_ch = (WIN_W - nx - BTN_W - 24 - 140) / 9;
        int nl = (int)strlen(a->name);
        draw_strn(fb, a->name, nl < max_ch ? nl : max_ch, nx, ry + (ITEM_H - g_glyph_h)/2, C_NAME);
        /* category */
        draw_str(fb, a->cat, WIN_W - BTN_W - 24 - 130, ry + (ITEM_H - g_glyph_h)/2, C_CATTXT);

        /* Install button / status */
        int bx = WIN_W - BTN_W - PAD_X, by = ry + 3, bh = ITEM_H - 6;
        const char *lbl; uint32_t bc;
        if (!strcmp(a->status, "done"))            { lbl = "Installed"; bc = C_BTN_DIS; }
        else if (!strcmp(a->status, "downloading")){ lbl = "..."; bc = C_ACCENT; }
        else if (!strcmp(a->status, "resolving"))  { lbl = "..."; bc = C_ACCENT; }
        else if (!strcmp(a->status, "error"))      { lbl = "Retry"; bc = 0x00803030u; }
        else                                       { lbl = "Install"; bc = C_BTN; }
        fill(fb, bx, by, BTN_W, bh, bc);
        int lw = (int)strlen(lbl) * 9;
        draw_str(fb, lbl, bx + (BTN_W - lw)/2, ry + (ITEM_H - g_glyph_h)/2, C_WHITE);
        hline(fb, ry + ITEM_H - 1, 0, WIN_W, 0x001a2230u);
    }

    /* Footer */
    int fy = WIN_H - FOOT_H;
    fill(fb, 0, fy, WIN_W, FOOT_H, C_BAR);
    hline(fb, fy, 0, WIN_W, C_BORDER);
    char foot[96];
    if (g_napps == 0)
        snprintf(foot, sizeof foot, "  downloading the app catalog...");
    else
        snprintf(foot, sizeof foot, "  %d apps%s%s   scroll to browse - click an app for details",
                 g_nview, g_cat[0] ? " in " : "", g_cat[0] ? g_cat : "");
    draw_str(fb, foot, 0, fy + (FOOT_H - g_glyph_h)/2, C_GREY);
}

/* ── IPC ─────────────────────────────────────────────────────────────────── */
static void send_msg(int fd, uint32_t type, const void *d, uint32_t len) {
    uint8_t h[8]; memcpy(h, &type, 4); memcpy(h+4, &len, 4);
    write(fd, h, 8); if (len && d) write(fd, d, len);
}
static void send_frame(int fd, uint32_t *px) {
    uint32_t frm[4] = {0,0,WIN_W,WIN_H}; uint32_t total = 16 + WIN_W*WIN_H*4;
    uint8_t *m = malloc(total); if (!m) return;
    memcpy(m, frm, 16); memcpy(m+16, px, WIN_W*WIN_H*4);
    send_msg(fd, IPC_APP_FRAME, m, total); free(m);
}

/* Kick off install of app index i (fork the install script). */
static void start_install(int fd, int i) {
    App *a = &g_apps[i];
    snprintf(a->status, sizeof a->status, "resolving");
    a->installing = true;
    /* clear any stale status file */
    char sp[160]; snprintf(sp, sizeof sp, "/fifi-data/apps/%s.status", a->name);
    unlink(sp);
    pid_t pid = fork();
    if (pid == 0) {
        /* child: run installer detached */
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execl("/bin/sh", "sh", "/fifi-data/apps/appstore-install.sh", a->repo, a->name, (char*)NULL);
        _exit(127);
    }
    char note[96];
    uint32_t nl = (uint32_t)snprintf(note, sizeof note, "Installing %s...", a->name);
    send_msg(fd, IPC_NOTIFY, note, nl);
}

/* Poll status files for installing apps; returns true if anything changed. */
static bool poll_installs(int fd) {
    bool changed = false;
    for (int i = 0; i < g_napps; i++) {
        if (!g_apps[i].installing) continue;
        char sp[160]; snprintf(sp, sizeof sp, "/fifi-data/apps/%s.status", g_apps[i].name);
        FILE *f = fopen(sp, "r");
        if (!f) continue;
        char st[32] = {0};
        if (fgets(st, sizeof st, f)) {
            char *nl = strchr(st, '\n'); if (nl) *nl = '\0';
            if (st[0] && strncmp(st, g_apps[i].status, sizeof g_apps[i].status) != 0) {
                snprintf(g_apps[i].status, sizeof g_apps[i].status, "%s", st);
                changed = true;
                if (!strcmp(st, "done") || !strcmp(st, "error")) {
                    g_apps[i].installing = false;
                    if (!strcmp(st, "done")) app_icon_forget(g_apps[i].name);
                    char note[96];
                    uint32_t l = (uint32_t)snprintf(note, sizeof note, "%s %s",
                        g_apps[i].name, !strcmp(st,"done") ? "installed" : "failed to install");
                    send_msg(fd, IPC_NOTIFY, note, l);
                }
            }
        }
        fclose(f);
    }
    return changed;
}

int main(void) {
    if (!font_load("/fifi-data/fonts/ter16b.psf")) { g_glyph = calloc(256*16,1); g_glyph_h = 16; }
    load_catalog();
    rebuild_view();
    if (g_napps == 0) {
        /* first run on a fresh install: no catalog yet — sync it in the
         * background; the tick below reloads once it lands. */
        pid_t sp = fork();
        if (sp == 0) {
            setsid();
            int nul = open("/dev/null", O_RDWR);
            if (nul >= 0) { dup2(nul,0); dup2(nul,1); dup2(nul,2); }
            execl("/bin/sh", "sh", "/fifi-data/apps/appstore-sync.sh", (char*)NULL);
            _exit(127);
        }
    }

    uint32_t *fb = malloc(WIN_W*WIN_H*4);
    if (!fb) return 1;

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr = {0}; addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIFI_SOCK, sizeof(addr.sun_path)-1);
    if (connect(sock, (struct sockaddr*)&addr, sizeof addr) < 0) { perror("connect"); return 1; }

    uint8_t conn[68] = {0};
    uint16_t w = WIN_W, h = WIN_H;
    memcpy(conn, &w, 2); memcpy(conn+2, &h, 2);
    snprintf((char*)(conn+4), 64, "App Store");
    send_msg(sock, IPC_APP_CONNECT, conn, sizeof conn);
    uint8_t hdr[8] = {0}; read(sock, hdr, 8);
    uint32_t type, plen; memcpy(&type, hdr, 4); memcpy(&plen, hdr+4, 4);
    if (type == IPC_WIN_CREATED && plen >= 20) { uint8_t r[20]; read(sock, r, 20); }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);   /* reap installer children automatically */
    render(fb); send_frame(sock, fb);

    uint8_t in_hdr[8]; int in_got = 0;
    uint8_t *in_pld = NULL; uint32_t in_plen = 0, in_pgot = 0;
    bool dirty = false, running = true, prev_lbtn = false;

    while (running) {
        uint8_t tbuf[512];
        /* non-blocking-ish read via short timeout using poll would be ideal; the
         * socket is blocking, so we rely on periodic input. Use MSG_DONTWAIT. */
        ssize_t n = recv(sock, tbuf, sizeof tbuf, MSG_DONTWAIT);
        if (n == 0) break;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;

        if (n > 0) {
            ssize_t pos = 0;
            while (pos < n) {
                if (in_got < 8) {
                    in_hdr[in_got++] = tbuf[pos++];
                    if (in_got == 8) {
                        memcpy(&type, in_hdr, 4); memcpy(&in_plen, in_hdr+4, 4);
                        if (in_plen > 65536) { in_got = 0; break; }
                        if (in_plen > 0) { in_pld = malloc(in_plen); in_pgot = 0; }
                    }
                } else if (in_plen > 0 && in_pgot < in_plen) {
                    uint32_t need = in_plen - in_pgot, have = (uint32_t)(n - pos);
                    uint32_t take = need < have ? need : have;
                    if (in_pld) memcpy(in_pld + in_pgot, tbuf + pos, take);
                    in_pgot += take; pos += take;
                    if (in_pgot >= in_plen) {
                        if (type == IPC_INPUT_KEY && in_plen >= 1 && g_detail >= 0) {
                            uint8_t k = in_pld ? in_pld[0] : 0;
                            if (k == 0x1Bu || k == 0x08u || k == 0x7Fu) { g_detail = -1; dirty = true; }
                        } else if (type == IPC_INPUT_KEY && in_plen >= 1) {
                            uint8_t k = in_pld ? in_pld[0] : 0;
                            bool schg = false;
                            /* FiFi keyboard sends DEL (0x7F) for Backspace; accept both */
                            if (k == 0x08u || k == 0x7Fu) { if (g_slen > 0) { g_search[--g_slen] = '\0'; schg = true; } }
                            else if (k == 0x1Bu) { if (g_slen) { g_slen = 0; g_search[0] = '\0'; schg = true; } }
                            else if (k == 'q' && g_slen == 0) { running = false; }
                            else if (k >= 0x20u && k < 0x7Fu && g_slen < (int)sizeof(g_search)-1) {
                                g_search[g_slen++] = (char)k; g_search[g_slen] = '\0'; schg = true;
                            }
                            if (schg) {
                                if (g_installed_mode) { scan_installed(); g_scroll = 0; }
                                else rebuild_view();
                                dirty = true;
                            }
                        } else if (type == IPC_INPUT_MOUSE && in_plen >= 9) {
                            int32_t mx, my; memcpy(&mx, in_pld, 4); memcpy(&my, in_pld+4, 4);
                            uint8_t btns = in_pld[8];
                            int8_t scroll = (in_plen >= 10) ? (int8_t)in_pld[9] : 0;
                            if (scroll != 0 && g_detail < 0) { g_scroll -= scroll * 2; clamp_scroll(); dirty = true; }
                            bool lbtn = (btns & 1);
                            if (lbtn && !prev_lbtn && g_detail >= 0) {
                                App *da = &g_apps[g_detail];
                                if (mx >= g_dt_back[0] && mx < g_dt_back[0]+g_dt_back[2] &&
                                    my >= g_dt_back[1] && my < g_dt_back[1]+g_dt_back[3]) {
                                    g_detail = -1; dirty = true;
                                } else if (mx >= g_dt_btn[0] && mx < g_dt_btn[0]+g_dt_btn[2] &&
                                           my >= g_dt_btn[1] && my < g_dt_btn[1]+g_dt_btn[3]) {
                                    if (!da->installing) {
                                        if (detail_installed(da)) launch_by_name(sock, da->name);
                                        else start_install(sock, g_detail);
                                    }
                                    dirty = true;
                                } else if (g_dt_rm[2] &&
                                           mx >= g_dt_rm[0] && mx < g_dt_rm[0]+g_dt_rm[2] &&
                                           my >= g_dt_rm[1] && my < g_dt_rm[1]+g_dt_rm[3]) {
                                    uninstall_by_name(sock, da->name);
                                    da->status[0] = '\0';
                                    dirty = true;
                                }
                            } else if (lbtn && !prev_lbtn) {
                                int ty = TITLE_H + SRCH_H;
                                if (my >= ty && my < ty + TAB_H) {
                                    if (mx >= g_instab_x && mx < g_instab_x + g_instab_w) {
                                        g_installed_mode = true;
                                        scan_installed(); g_scroll = 0; dirty = true;
                                    } else for (int i = 0; i < NTABS; i++)
                                        if (mx >= g_tab_x[i] && mx < g_tab_x[i] + g_tab_w[i]) {
                                            g_installed_mode = false;
                                            snprintf(g_cat, sizeof g_cat, "%s", TABS[i][1]);
                                            rebuild_view(); dirty = true; break;
                                        }
                                } else if (my >= list_top() && my < WIN_H - FOOT_H) {
                                    int r = (my - list_top()) / ITEM_H;
                                    int vi = g_scroll + r;
                                    if (g_installed_mode) {
                                        /* Check Updates button (on the INSTALLED APPS header row) */
                                        if (g_chkupd_w && mx >= g_chkupd_x &&
                                            mx < g_chkupd_x + g_chkupd_w &&
                                            my >= g_chkupd_y && my < g_chkupd_y + ITEM_H - 6) {
                                            start_check_updates(sock);
                                            dirty = true;
                                        } else if (vi < g_nirows && g_irows[vi].kind == 1) {
                                            int bx = WIN_W - BTN_W - PAD_X;
                                            int rmx = bx - 76 - 6;
                                            int ai = g_irows[vi].idx;
                                            Inst *in = &g_inst[ai];
                                            if (mx >= bx && mx < bx + BTN_W) {
                                                if (in->has_update) {
                                                    start_update(sock, ai);
                                                    in->has_update = false;  /* optimistic */
                                                } else {                     /* Launch (reopens if running) */
                                                    launch_app(sock, ai);
                                                    in->running = true;      /* optimistic */
                                                }
                                                dirty = true;
                                            } else if (in->running && mx >= rmx - 66 - 6 && mx < rmx - 6) {
                                                char nk[224];              /* Close = force-quit */
                                                snprintf(nk, sizeof nk, "/fifi-data/apps/%s.d/", in->name);
                                                proc_kill_all(nk);
                                                in->running = false;       /* optimistic */
                                                dirty = true;
                                            } else if (mx >= rmx && mx < rmx + 76) {
                                                start_uninstall(sock, ai);
                                                scan_installed(); clamp_scroll(); dirty = true;
                                            } else {
                                                for (int ci = 0; ci < g_napps; ci++)
                                                    if (!strcmp(g_apps[ci].name, in->name)) {
                                                        open_detail(ci); dirty = true; break;
                                                    }
                                            }
                                        }
                                    } else if (vi < g_nview) {
                                        int bx = WIN_W - BTN_W - PAD_X;
                                        int ai = g_view[vi];
                                        if (mx >= bx && mx < bx + BTN_W) {
                                            if (!g_apps[ai].installing && strcmp(g_apps[ai].status, "done"))
                                                start_install(sock, ai);
                                            dirty = true;
                                        } else {
                                            open_detail(ai); dirty = true;
                                        }
                                    }
                                }
                            }
                            prev_lbtn = lbtn;
                        } else if (type == IPC_WIN_RESIZE && in_plen >= 4 && in_pld) {
                            uint16_t nw, nh;
                            memcpy(&nw, in_pld, 2); memcpy(&nh, in_pld + 2, 2);
                            if (nw >= 300 && nh >= 220 &&
                                ((int)nw != g_win_w || (int)nh != g_win_h)) {
                                uint32_t *nfb = realloc(fb, (size_t)nw * nh * 4u);
                                if (nfb) { fb = nfb; g_win_w = nw; g_win_h = nh; clamp_scroll(); }
                            }
                            dirty = true;
                        } else if (type == IPC_INVALIDATE) {
                            dirty = true;
                        }
                        free(in_pld); in_pld = NULL; in_got = 0; in_plen = 0; in_pgot = 0;
                    }
                } else {
                    if (type == IPC_INVALIDATE) dirty = true;
                    in_got = 0; in_plen = 0; in_pgot = 0;
                }
            }
        }

        if (poll_installs(sock)) dirty = true;
        /* icon downloads land asynchronously; refresh rows as they arrive */
        static int itick = 0;
        if (++itick >= 16) { itick = 0; if (icon_dl_poll()) dirty = true; }
        /* refresh installed/services view every ~2s so RUNNING badges track reality */
        static int tick = 0;
        if (++tick >= 66) {
            tick = 0;
            if (g_installed_mode) { scan_installed(); clamp_scroll(); dirty = true; }
            if (g_napps == 0) {          /* first-run sync finished? */
                load_catalog();
                if (g_napps) { rebuild_view(); dirty = true; }
            }
        }
        if (dirty) { render(fb); send_frame(sock, fb); dirty = false; }
        struct timespec ts = {0, 30000000};  /* 30ms */
        nanosleep(&ts, NULL);
    }

    send_msg(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock); free(fb); free(g_glyph);
    return 0;
}
