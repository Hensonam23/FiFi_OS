/* FiFi Settings — modern tabbed "hub" for FiFi OS.
 *
 * A single window with a left sidebar/tab rail. Each rail item switches the
 * content pane so the user does not need separate apps:
 *   Personalize : read/write the compositor's persisted theme config
 *                 (/fifi-data/fifi-settings.conf — accent, wallpaper, panel
 *                 position, glass/shadow effects, corner radius). Changes are
 *                 written to disk and live-reloaded by the compositor.
 *   Wi-Fi       : scan / select / connect through the fixed Wi-Fi root broker,
 *                 using the same boundary as the standalone fifi-wifi app.
 *   Network     : live interface / IP / RX-TX throughput (netmon logic).
 *   System      : live CPU freq / memory / uptime / load / processes + volume.
 *   Security    : live status and fixed-broker toggles for firewall / DoH /
 *                 VPN / Tor, plus read-only privacy-blocking status.
 *   About       : OS version + kernel (uname).
 *
 * Deep-linking: `fifi-settings <section>` opens straight on a tab, where
 * <section> is one of: personalize | wifi | network | system | security | about
 *
 * Build: gcc -O2 -static -o fifi-settings settings.c   (see Makefile)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "../../shared/theme.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <sound/asound.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* ── IPC ─────────────────────────────────────────────────────────────────── */
#include "../../shared/app_ipc.h"
#include "../../shared/app_ui.h"
#include "../../shared/wifi_scan.h"

/* ── Window ──────────────────────────────────────────────────────────────── */
#define WIN_W       760
#define WIN_H       680
#define TITLE_H     24    /* reserved for compositor title bar */
#define SIDEBAR_W   176
#define CPAD        18    /* content padding */
#define ROW_H       24

static int g_win_w = WIN_W;
static int g_win_h = WIN_H;
static int g_draw_y_offset = 0;
static int g_draw_clip_top = 0;
static int g_draw_clip_bottom = WIN_H;

/* ── Colours (0x00RRGGBB) ────────────────────────────────────────────────── */
#define C_BG        0x00141b26u  /* content background */
#define C_SIDE      0x000d1219u  /* sidebar background */
#define C_SIDE_HOV  0x00182534u
#define C_HDR       0x00d6e8ffu  /* section header text */
#define C_KEY       0x0072a8ceu
#define C_VAL       0x00b6cce4u
#define C_GREY      0x00637586u
#define C_BORDER    0x00243448u
#define C_ROW_A     0x00161f2bu
#define C_ROW_B     0x00121a24u
#define C_ROW_SEL   0x00203858u
#define C_BTN_BG    0x001d2a3cu
#define C_WHITE     0x00eef4ffu
#define C_GREEN     0x0034c070u
#define C_RED       0x00d24444u
#define C_YELLOW    0x00c8a828u

/* Accent colour — read from config so the hub matches the user's theme. */
static uint32_t g_accent = FIFI_THEME_DEFAULT_ACCENT;

/* ── Shared bitmap UI ────────────────────────────────────────────────────── */
static fifi_ui_font_t g_font;
#define g_glyph_h (g_font.height)
#define CW 9  /* character advance in pixels (8px glyph + 1px gap) */

static bool font_load(const char *path) {
    return fifi_ui_font_load_psf1(&g_font, path);
}

/* Decode one UTF-8 sequence at s[*i]; return codepoint, advance *i. The PSF
 * bitmap font only holds Latin-1, so text is UTF-8 but glyphs beyond it must be
 * folded (below) — this keeps multi-byte chars from rendering as byte garbage. */
static uint32_t u8_next(const char *s, size_t *i) {
    unsigned char c = (unsigned char)s[*i];
    if (c < 0x80u) { (*i)++; return c; }
    uint32_t cp; int extra;
    if      ((c & 0xE0u) == 0xC0u) { cp = c & 0x1Fu; extra = 1; }
    else if ((c & 0xF0u) == 0xE0u) { cp = c & 0x0Fu; extra = 2; }
    else if ((c & 0xF8u) == 0xF0u) { cp = c & 0x07u; extra = 3; }
    else { (*i)++; return 0xFFFDu; }
    (*i)++;
    for (int k = 0; k < extra; k++) {
        unsigned char cc = (unsigned char)s[*i];
        if ((cc & 0xC0u) != 0x80u) return 0xFFFDu;
        cp = (cp << 6) | (cc & 0x3Fu); (*i)++;
    }
    return cp;
}

/* Fold a codepoint the bitmap font can't show to a printable ASCII byte.
 * Dashes are handled separately (drawn as a bar). Returns 0 to skip. */
static int fold_cp(uint32_t cp) {
    if (cp < 0x100u) return (int)cp;                 /* ASCII + Latin-1 */
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201B: return '\'';   /* curly single quotes */
    case 0x201C: case 0x201D: case 0x201F: return '"';    /* curly double quotes */
    case 0x2022: case 0x2027: case 0x00B7: return '.';    /* bullet / mid-dot */
    case 0x2026: return '.';                              /* ellipsis (one dot) */
    case 0x00A0: return ' ';                              /* nbsp */
    case 0x2039: return '<'; case 0x203A: return '>';
    case 0x00AB: return '<'; case 0x00BB: return '>';
    default: return '?';
    }
}

static void draw_char(uint32_t *fb, int c, int px, int py, uint32_t fg) {
    if (c < 0) return;
    py += g_draw_y_offset;
    if (py < g_draw_clip_top || py + g_glyph_h > g_draw_clip_bottom) return;
    fifi_ui_glyph((fifi_ui_canvas_t){fb, g_win_w, g_win_h}, &g_font,
                  px, py, (unsigned char)c, fg, 0);
}

/* Draw an em/en/figure dash as a centred horizontal bar (the PSF font has no
 * such glyph). em spans the full cell, en/minus a shorter middle run. */
static void draw_dash(uint32_t *fb, uint32_t cp, int px, int py, uint32_t fg) {
    py += g_draw_y_offset;
    int x0 = px, x1 = px + 7;                            /* em-dash: 0x2014 */
    if (cp != 0x2014) { x0 = px + 1; x1 = px + 6; }      /* en/minus: 0x2013/0x2212 */
    int y0 = py + g_glyph_h / 2;
    for (int x = x0; x <= x1; x++)
        for (int yy = y0; yy < y0 + 1; yy++)
            if (x >= 0 && x < g_win_w && yy >= g_draw_clip_top &&
                yy < g_draw_clip_bottom && yy < g_win_h)
                fb[yy * g_win_w + x] = fg;
}

/* Draw a single codepoint into one cell, folding/synthesising as needed. */
static void draw_cp(uint32_t *fb, uint32_t cp, int px, int py, uint32_t fg) {
    if (cp == 0x2014 || cp == 0x2013 || cp == 0x2212) { draw_dash(fb, cp, px, py, fg); return; }
    int c = fold_cp(cp);
    if (c > 0) draw_char(fb, c, px, py, fg);
}

static void draw_str(uint32_t *fb, const char *s, int x, int y, uint32_t fg) {
    for (size_t i = 0; s[i]; x += CW) draw_cp(fb, u8_next(s, &i), x, y, fg);
}

static void draw_str_clip(uint32_t *fb, const char *s, int x, int y,
                          uint32_t fg, int max_px) {
    for (size_t i = 0; s[i] && max_px > CW; x += CW, max_px -= CW)
        draw_cp(fb, u8_next(s, &i), x, y, fg);
}

static int str_w(const char *s) {
    int cols = 0; for (size_t i = 0; s[i]; ) { u8_next(s, &i); cols++; }
    return cols * CW;
}

static void fill(uint32_t *fb, int x, int y, int w, int h, uint32_t col) {
    y += g_draw_y_offset;
    if (y < g_draw_clip_top) { h -= g_draw_clip_top - y; y = g_draw_clip_top; }
    if (y + h > g_draw_clip_bottom) h = g_draw_clip_bottom - y;
    if (h <= 0) return;
    fifi_ui_fill((fifi_ui_canvas_t){fb, g_win_w, g_win_h}, x, y, w, h, col);
}

static void rect_border(uint32_t *fb, int x, int y, int w, int h, uint32_t col) {
    fill(fb, x, y, w, 1, col);
    fill(fb, x, y + h - 1, w, 1, col);
    fill(fb, x, y, 1, h, col);
    fill(fb, x + w - 1, y, 1, h, col);
}

/* Section header: title + accent-tinted underline. Returns the y for content. */
static int section_hdr(uint32_t *fb, const char *title, int x, int y) {
    draw_str(fb, title, x, y, C_HDR);
    int uy = y + g_glyph_h + 3 + g_draw_y_offset;
    if (uy >= g_draw_clip_top && uy < g_draw_clip_bottom && uy < g_win_h)
        for (int i = x; i < g_win_w - CPAD; i++) fb[uy * g_win_w + i] = C_BORDER;
    return y + g_glyph_h + 12;
}

/* Labelled button; highlighted (accent) when selected. */
static void draw_btn(uint32_t *fb, int x, int y, int w, int h,
                     const char *label, bool sel) {
    fill(fb, x, y, w, h, sel ? g_accent : C_BTN_BG);
    rect_border(fb, x, y, w, h, sel ? C_WHITE : C_BORDER);
    int tw = str_w(label);
    draw_str_clip(fb, label, x + (w - tw)/2 < x+4 ? x+4 : x + (w - tw)/2,
                  y + (h - g_glyph_h)/2, sel ? C_WHITE : C_VAL, w - 8);
}

/* ── Tabs ────────────────────────────────────────────────────────────────── */
enum { TAB_PERS = 0, TAB_WIFI, TAB_NET, TAB_SYS, TAB_SEC, TAB_ABOUT, N_TABS };
static const char *g_tab_labels[N_TABS] = {
    "Personalize", "Wi-Fi", "Network", "System", "Security", "About"
};
static int g_tab = TAB_PERS;

#define SIDE_TOP  (TITLE_H + 46)
#define SIDE_ROW  40

/* ── Clickable hot-regions (used by the Personalize pane) ────────────────── */
enum { ACT_ACCENT = 1, ACT_WALL, ACT_PANEL, ACT_GLASS, ACT_SHADOW, ACT_RADIUS, ACT_DOCK, ACT_STATUS,
       ACT_FONT_FAM, ACT_FONT_SZ, ACT_DESKINFO, ACT_ALIGN, ACT_AUTOHIDE,
       ACT_TBSIZE, ACT_CLOCK, ACT_WALLFIT,
       ACT_FW, ACT_DOH, ACT_VPN, ACT_TOR };
typedef struct { int x, y, w, h, act, arg; } Hot;
#define MAX_HOTS 64
static Hot g_hots[MAX_HOTS];
static int g_nhots = 0;
static void hot_reset(void) { g_nhots = 0; }
static void add_hot(int x, int y, int w, int h, int act, int arg) {
    y += g_draw_y_offset;
    if (y < g_draw_clip_top) { h -= g_draw_clip_top - y; y = g_draw_clip_top; }
    if (y + h > g_draw_clip_bottom) h = g_draw_clip_bottom - y;
    if (h <= 0) return;
    if (g_nhots < MAX_HOTS) g_hots[g_nhots++] = (Hot){x, y, w, h, act, arg};
}

/* ── Theme config store (preserves keys we don't manage) ─────────────────── */
#define CFG_PATH      FIFI_THEME_CONFIG_PATH
#define MAX_CFG_LINES 48
static char g_cfg_key[MAX_CFG_LINES][32];
static char g_cfg_val[MAX_CFG_LINES][160];
static int  g_cfg_n = 0;
static char g_pers_note[96] = "Changes apply live.";

static void cfg_load(void) {
    g_cfg_n = 0;
    FILE *f = fopen(CFG_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f) && g_cfg_n < MAX_CFG_LINES) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        char *nl = strchr(val, '\n'); if (nl) *nl = '\0';
        nl = strchr(val, '\r'); if (nl) *nl = '\0';
        snprintf(g_cfg_key[g_cfg_n], sizeof(g_cfg_key[0]), "%s", line);
        snprintf(g_cfg_val[g_cfg_n], sizeof(g_cfg_val[0]), "%s", val);
        g_cfg_n++;
    }
    fclose(f);
}

static int cfg_find(const char *key) {
    for (int i = 0; i < g_cfg_n; i++)
        if (strcmp(g_cfg_key[i], key) == 0) return i;
    return -1;
}
static int cfg_get_int(const char *key, int def) {
    int i = cfg_find(key);
    return i < 0 ? def : (int)strtol(g_cfg_val[i], NULL, 10);
}
static unsigned cfg_get_uint(const char *key, unsigned def) {
    int i = cfg_find(key);
    return i < 0 ? def : (unsigned)strtoul(g_cfg_val[i], NULL, 10);
}
static void cfg_set_str(const char *key, const char *val) {
    int i = cfg_find(key);
    if (i < 0) {
        if (g_cfg_n >= MAX_CFG_LINES) return;
        i = g_cfg_n++;
        snprintf(g_cfg_key[i], sizeof(g_cfg_key[0]), "%s", key);
    }
    snprintf(g_cfg_val[i], sizeof(g_cfg_val[0]), "%s", val);
}
static void cfg_set_int(const char *key, int v)      { char b[32]; snprintf(b, sizeof b, "%d", v); cfg_set_str(key, b); }
static void cfg_set_uint(const char *key, unsigned v){ char b[32]; snprintf(b, sizeof b, "%u", v); cfg_set_str(key, b); }

static void cfg_save(void) {
    cfg_set_uint(FIFI_THEME_CONFIG_FORMAT_KEY, FIFI_THEME_CONFIG_VERSION);
    FILE *f = fopen(CFG_PATH, "w");
    if (!f) { snprintf(g_pers_note, sizeof g_pers_note, "ERROR: cannot write %s", CFG_PATH); return; }
    for (int i = 0; i < g_cfg_n; i++)
        fprintf(f, "%s=%s\n", g_cfg_key[i], g_cfg_val[i]);
    fclose(f);
    snprintf(g_pers_note, sizeof g_pers_note, "Saved \xe2\x80\x94 applied live.");
}

/* ── System font catalog (UI font of the whole desktop) ──────────────────────
 * The compositor scans the same directory (its VFS "/fonts" = /fifi-data/fonts)
 * and persists the choice as font_file=/fonts/<name> + font_px= in the shared
 * conf; it live-applies on save like every other Personalize key. We list the
 * real directory here and present prev/next + size stepper. */
#define FONT_DIR   "/fifi-data/fonts"
#define FONT_MAX   200
static char g_font_files[FONT_MAX][64];
static int  g_font_n = 0;

static int font_suffix_ok(const char *n) {
    const char *dot = strrchr(n, '.');
    if (!dot) return 0;
    return !strcasecmp(dot, ".ttf") || !strcasecmp(dot, ".otf") || !strcasecmp(dot, ".ttc");
}
static void font_list_scan(void) {
    g_font_n = 0;
    DIR *d = opendir(FONT_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && g_font_n < FONT_MAX) {
        if (e->d_name[0] == '.' || !font_suffix_ok(e->d_name)) continue;
        if (strlen(e->d_name) >= sizeof(g_font_files[0])) continue;
        snprintf(g_font_files[g_font_n++], sizeof(g_font_files[0]), "%s", e->d_name);
    }
    closedir(d);
    for (int i = 1; i < g_font_n; i++) {          /* insertion sort, case-insensitive */
        char t[64]; snprintf(t, sizeof t, "%s", g_font_files[i]);
        int j = i - 1;
        while (j >= 0 && strcasecmp(g_font_files[j], t) > 0) {
            snprintf(g_font_files[j + 1], sizeof(g_font_files[0]), "%s", g_font_files[j]);
            j--;
        }
        snprintf(g_font_files[j + 1], sizeof(g_font_files[0]), "%s", t);
    }
}
/* Index of the conf's font_file in the list, or -1 (unset / not found). */
static int font_cur_index(void) {
    int ci = cfg_find(FIFI_THEME_KEY_FONT_FILE);
    if (ci < 0) return -1;
    const char *base = strrchr(g_cfg_val[ci], '/');
    base = base ? base + 1 : g_cfg_val[ci];
    for (int i = 0; i < g_font_n; i++)
        if (strcmp(g_font_files[i], base) == 0) return i;
    return -1;
}

/* ── TTF preview rendering (stb_truetype) ─────────────────────────────────────
 * Renders each font's name in its OWN typeface, like the old compositor picker.
 * Compact pre-rendered alpha strips mean wheel events never reopen or
 * rasterize full font files. */

#define FONT_PREVIEW_W 280
#define FONT_PREVIEW_H 20
typedef struct {
    unsigned char *alpha;
    int width;
    bool ready;
} FontPreview;
static FontPreview g_font_preview[FONT_MAX];

static void font_preview_build(int index) {
    if (index < 0 || index >= g_font_n || g_font_preview[index].ready) return;
    g_font_preview[index].ready = true;
    char path[160], name[64];
    snprintf(path, sizeof path, "%s/%s", FONT_DIR, g_font_files[index]);
    snprintf(name, sizeof name, "%s", g_font_files[index]);
    char *dot = strrchr(name, '.'); if (dot) *dot = '\0';

    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long size = ftell(f);
    if (size <= 0 || size > 40 * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); return;
    }
    unsigned char *font_data = malloc((size_t)size);
    if (!font_data) { fclose(f); return; }
    if (fread(font_data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f); free(font_data); return;
    }
    fclose(f);

    stbtt_fontinfo face;
    int offset = stbtt_GetFontOffsetForIndex(font_data, 0);
    if (offset < 0 || !stbtt_InitFont(&face, font_data, offset)) {
        free(font_data); return;
    }
    unsigned char *strip = calloc(FONT_PREVIEW_W, FONT_PREVIEW_H);
    if (!strip) { free(font_data); return; }
    float scale = stbtt_ScaleForPixelHeight(&face, 18.0f);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&face, &asc, &desc, &gap);
    (void)desc; (void)gap;
    int baseline = (int)(asc * scale);
    int pen_x = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        int advance, bearing, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&face, *p, &advance, &bearing);
        (void)bearing;
        stbtt_GetCodepointBitmapBox(&face, *p, scale, scale, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0 && gw < 256 && gh < 256) {
            unsigned char *glyph = malloc((size_t)gw * gh);
            if (glyph) {
                stbtt_MakeCodepointBitmap(&face, glyph, gw, gh, gw, scale, scale, *p);
                int gx0 = pen_x + x0, gy0 = baseline + y0;
                for (int gy = 0; gy < gh; gy++) for (int gx = 0; gx < gw; gx++) {
                    int dx = gx0 + gx, dy = gy0 + gy;
                    if (dx < 0 || dx >= FONT_PREVIEW_W || dy < 0 || dy >= FONT_PREVIEW_H) continue;
                    unsigned a = glyph[gy * gw + gx];
                    unsigned old = strip[dy * FONT_PREVIEW_W + dx];
                    strip[dy * FONT_PREVIEW_W + dx] = (unsigned char)(a + old * (255 - a) / 255);
                }
                free(glyph);
            }
        }
        pen_x += (int)(advance * scale);
        if (pen_x >= FONT_PREVIEW_W) break;
    }
    free(font_data);
    g_font_preview[index].alpha = strip;
    g_font_preview[index].width = pen_x < FONT_PREVIEW_W ? pen_x : FONT_PREVIEW_W;
}

static void font_previews_build(void) {
    for (int i = 0; i < g_font_n; i++) font_preview_build(i);
}

static void font_preview_draw(uint32_t *fb, int index, int x, int y,
                              uint32_t fg, int maxw) {
    if (index < 0 || index >= g_font_n) return;
    font_preview_build(index);
    FontPreview *preview = &g_font_preview[index];
    if (!preview->alpha) {
        char name[64]; snprintf(name, sizeof name, "%s", g_font_files[index]);
        char *dot = strrchr(name, '.'); if (dot) *dot = '\0';
        draw_str_clip(fb, name, x, y, fg, maxw / 9);
        return;
    }
    y += g_draw_y_offset;
    int width = preview->width < maxw ? preview->width : maxw;
    int fr = (fg >> 16) & 0xff, fgc = (fg >> 8) & 0xff, fbc = fg & 0xff;
    for (int py = 0; py < FONT_PREVIEW_H; py++) {
        int fy = y + py;
        if (fy < g_draw_clip_top || fy >= g_draw_clip_bottom || fy < 0 || fy >= g_win_h) continue;
        for (int px = 0; px < width; px++) {
            int fx = x + px;
            if (fx < 0 || fx >= g_win_w) continue;
            unsigned a = preview->alpha[py * FONT_PREVIEW_W + px];
            if (!a) continue;
            uint32_t *dp = &fb[(size_t)fy * g_win_w + fx], d = *dp;
            int dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
            *dp = 0xff000000u
                | (((fr * a + dr * (255 - a)) / 255) << 16)
                | (((fgc * a + dg * (255 - a)) / 255) << 8)
                |  ((fbc * a + db * (255 - a)) / 255);
        }
    }
}

/* Font size ladder + dropdown state (family=1, size=2). Geometry captured at
 * render time for the click/wheel math in pers_click. */
static const int g_font_sizes[] = FIFI_FONT_SIZES;
#define N_FONT_SIZES ((int)(sizeof(g_font_sizes) / sizeof(g_font_sizes[0])))
static int g_font_dd = 0, g_font_dd_scroll = 0;
static int g_pers_scroll = 0, g_pers_max_scroll = 0;
static int g_ff_bx, g_ff_by, g_ff_bw, g_ff_bh;   /* family combo box */
static int g_fs_bx, g_fs_by, g_fs_bw, g_fs_bh;   /* size combo box */
static int g_dd_x, g_dd_y, g_dd_w, g_dd_rowh, g_dd_vis;  /* open list */

/* Accent presets mirror the compositor's g_accent_presets[] (gui.c). */
#define N_ACCENT FIFI_ACCENT_PRESET_COUNT
static const uint32_t g_accent_presets[N_ACCENT] = FIFI_ACCENT_PRESETS;
#define WALL_N WALLPAPER_COUNT
static const char *g_wall_names[WALL_N] = {
    "Gradient", "Solid", "Stars", "Grid", "Waves", "Image",
    "Aurora", "Northern", "Nebula", "Dusk", "Ocean", "Spring", "Ember"
};
static const char *g_panel_names[4] = { "Bottom", "Top", "Left", "Right" };

/* ── System info (sysmon-style, reused from the original settings app) ───── */
typedef struct { char key[16]; char val[64]; } InfoRow;
#define N_INFO 6
static InfoRow g_info[N_INFO];

static void gather_info(void) {
    struct sysinfo si; sysinfo(&si);

    snprintf(g_info[0].key, sizeof g_info[0].key, "Uptime");
    long up = si.uptime;
    snprintf(g_info[0].val, sizeof g_info[0].val, "%ldh %02ldm %02lds", up/3600, (up%3600)/60, up%60);

    snprintf(g_info[1].key, sizeof g_info[1].key, "Memory");
    unsigned long total_mb = si.totalram * si.mem_unit / 1024 / 1024;
    unsigned long free_mb  = si.freeram  * si.mem_unit / 1024 / 1024;
    snprintf(g_info[1].val, sizeof g_info[1].val, "%lu MB / %lu MB", total_mb - free_mb, total_mb);

    snprintf(g_info[2].key, sizeof g_info[2].key, "CPU Freq");
    {
        char buf[32] = {0};
        int fd = open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", O_RDONLY);
        if (fd >= 0) { read(fd, buf, sizeof(buf)-1); close(fd); }
        unsigned long khz = (unsigned long)strtol(buf, NULL, 10);
        if (khz > 0) snprintf(g_info[2].val, sizeof g_info[2].val, "%.2f GHz", khz / 1000000.0);
        else         snprintf(g_info[2].val, sizeof g_info[2].val, "N/A");
    }

    snprintf(g_info[3].key, sizeof g_info[3].key, "Load");
    snprintf(g_info[3].val, sizeof g_info[3].val, "%.2f (1m avg)", (double)si.loads[0] / 65536.0);

    snprintf(g_info[4].key, sizeof g_info[4].key, "Processes");
    snprintf(g_info[4].val, sizeof g_info[4].val, "%u", si.procs);

    snprintf(g_info[5].key, sizeof g_info[5].key, "Time");
    time_t now = time(NULL); struct tm *t = localtime(&now);
    snprintf(g_info[5].val, sizeof g_info[5].val, "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
}

/* ── ALSA volume (direct ioctl, from the original settings app) ──────────── */
static int   g_ctl    = -1;
static struct snd_ctl_elem_id g_vid;
static long  g_vmin   = 0, g_vmax = 100;
static int   g_vcount = 2;
static int   g_vol    = 50;
static int   g_sl_x, g_sl_y, g_sl_w = 240, g_sl_h = 14;
static int   g_mouse_sl_x, g_mouse_sl_y;
static int   g_touchpad_sl_x, g_touchpad_sl_y;
static int   g_pointer_sl_w = 240, g_pointer_sl_h = 14;

static void draw_speed_slider(uint32_t *fb, const char *label, int x, int y,
                              int value, int *slider_x, int *slider_y) {
    *slider_x = x + 15*CW;
    *slider_y = y + (ROW_H - g_pointer_sl_h)/2;
    draw_str(fb, label, x, y + (ROW_H - g_glyph_h)/2, C_KEY);
    fill(fb, *slider_x, *slider_y, g_pointer_sl_w, g_pointer_sl_h, C_BORDER);
    int filled = g_pointer_sl_w * (value + 100) / 200;
    fill(fb, *slider_x, *slider_y, filled, g_pointer_sl_h, g_accent);
    fill(fb, *slider_x + filled - 4, *slider_y - 3, 8,
         g_pointer_sl_h + 6, C_WHITE);
    char pct[16]; snprintf(pct, sizeof pct, "%+d%%", value);
    draw_str(fb, pct, *slider_x + g_pointer_sl_w + 10,
             y + (ROW_H - g_glyph_h)/2, C_VAL);
}

static void alsa_init(void) {
    for (int card = 0; card < 4; card++) {
        char p[32]; snprintf(p, sizeof p, "/dev/snd/controlC%d", card);
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
            if (ids[i].iface == SNDRV_CTL_ELEM_IFACE_MIXER && strstr((char*)ids[i].name, "Volume")) fi = (int)i;
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
        if (range > 0) g_vol = (int)((ev.value.integer.value[0] - g_vmin) * 100 / range);
    }
}

static void alsa_set_vol(int v) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
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

/* ── Network stats (netmon logic) ────────────────────────────────────────── */
#define MAX_IFACES 8
typedef struct {
    char name[16];
    uint64_t rx_bytes, tx_bytes, rx_rate, tx_rate;
    char ip4[20];
    bool up;
} iface_t;
static iface_t g_ifaces[MAX_IFACES];
static int     g_nifaces = 0;

static void net_update_ip(iface_t *ifc) {
    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk < 0) return;
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, ifc->name, IFNAMSIZ - 1);
    if (ioctl(sk, SIOCGIFADDR, &ifr) == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        inet_ntop(AF_INET, &sin->sin_addr, ifc->ip4, sizeof ifc->ip4);
    } else {
        strncpy(ifc->ip4, "no IP", sizeof ifc->ip4);
    }
    if (ioctl(sk, SIOCGIFFLAGS, &ifr) == 0)
        ifc->up = !!(ifr.ifr_flags & IFF_UP) && !!(ifr.ifr_flags & IFF_RUNNING);
    close(sk);
}

static void net_update(void) {
    int fd = open("/proc/net/dev", O_RDONLY);
    if (fd < 0) return;
    char buf[4096] = {0};
    read(fd, buf, sizeof(buf)-1);
    close(fd);
    char *line = buf; int skip = 2;
    while (skip-- > 0) { line = strchr(line, '\n'); if (!line) return; line++; }

    iface_t ni[MAX_IFACES]; int nn = 0;
    while (*line && nn < MAX_IFACES) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char name[16] = {0}; uint64_t rx=0, tx=0, tmp=0;
        int n = sscanf(line, " %15[^:]: %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                       name, &rx,&tmp,&tmp,&tmp,&tmp,&tmp,&tmp,&tmp,&tx);
        bool is_real = false;
        if (n >= 10 && strcmp(name, "lo") != 0) {
            char tp[64]; snprintf(tp, sizeof tp, "/sys/class/net/%s/type", name);
            int tfd = open(tp, O_RDONLY);
            if (tfd >= 0) { char tb[8]={0}; read(tfd, tb, sizeof(tb)-1); close(tfd);
                is_real = (tb[0]=='1' && (tb[1]=='\n' || tb[1]=='\0')); }
        }
        if (is_real) {
            iface_t *ifc = &ni[nn++]; memset(ifc, 0, sizeof *ifc);
            memcpy(ifc->name, name, sizeof ifc->name);
            ifc->rx_bytes = rx; ifc->tx_bytes = tx;
            for (int i = 0; i < g_nifaces; i++)
                if (strcmp(g_ifaces[i].name, name) == 0) {
                    ifc->rx_rate = rx > g_ifaces[i].rx_bytes ? rx - g_ifaces[i].rx_bytes : 0;
                    ifc->tx_rate = tx > g_ifaces[i].tx_bytes ? tx - g_ifaces[i].tx_bytes : 0;
                    break;
                }
            net_update_ip(ifc);
        }
        if (!nl) break;
        line = nl + 1;
    }
    memcpy(g_ifaces, ni, nn * sizeof(iface_t));
    g_nifaces = nn;
}

static void fmt_rate(uint64_t bps, char *b, int n) {
    if (bps >= 1024*1024)   snprintf(b, n, "%.1f MB/s", bps / (1024.0*1024.0));
    else if (bps >= 1024)   snprintf(b, n, "%.1f KB/s", bps / 1024.0);
    else                    snprintf(b, n, "%llu B/s", (unsigned long long)bps);
}
static void fmt_bytes(uint64_t bt, char *b, int n) {
    if (bt >= 1024ULL*1024*1024) snprintf(b, n, "%.2f GB", bt / (1024.0*1024.0*1024.0));
    else if (bt >= 1024*1024)    snprintf(b, n, "%.2f MB", bt / (1024.0*1024.0));
    else if (bt >= 1024)         snprintf(b, n, "%.2f KB", bt / 1024.0);
    else                         snprintf(b, n, "%llu B", (unsigned long long)bt);
}

/* ── Wi-Fi (scan / connect logic, ported from fifi-wifi) ─────────────────── */
#define MAX_NETS 32
typedef fifi_wifi_network_t NetEntry;
static NetEntry g_nets[MAX_NETS];
static int  g_net_count = 0;
static int  g_sel = 0, g_wscroll = 0, g_list_top = 0;
typedef enum { ST_IDLE, ST_SCANNING, ST_CONNECTING, ST_CONNECTED } WState;
static WState g_wstate = ST_IDLE;
static char g_wstatus[96] = "Open the Wi-Fi tab to scan";
static char g_conn_ssid[64] = "";
static bool g_pw_mode = false;
static char g_pw_buf[128] = ""; static int g_pw_len = 0;
static char g_wif[32] = "";
static bool g_wifi_scanned = false;

static pid_t g_scan_pid = -1;
static int   g_scan_pipe = -1;
static char  g_scan_buf[65536];
static int   g_scan_buf_len = 0;

static void find_wifi_if(void) {
    g_wif[0] = '\0';
    DIR *d = opendir("/sys/class/net");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (!strncmp(e->d_name, "p2p-", 4)) continue;
        if (strlen(e->d_name) >= sizeof(g_wif)) continue;
        char wp[320];
        snprintf(wp, sizeof wp, "/sys/class/net/%s/wireless", e->d_name);
        if (access(wp, F_OK) == 0) { snprintf(g_wif, sizeof g_wif, "%s", e->d_name); break; }
        snprintf(wp, sizeof wp, "/sys/class/net/%s/phy80211", e->d_name);
        if (access(wp, F_OK) == 0) { snprintf(g_wif, sizeof g_wif, "%s", e->d_name); break; }
    }
    closedir(d);
}

static bool wifi_is_saved(const char *ssid) {
    FILE *saved = fopen("/fifi-data/wifi-saved-ssid", "r");
    if (!saved) return false;
    char line[160]; bool match = false;
    while (fgets(line, sizeof line, saved)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strcmp(line, ssid)) { match = true; break; }
    }
    fclose(saved);
    return match;
}

static void parse_scan(const char *buf) {
    g_net_count = fifi_wifi_parse_scan(buf, g_nets, MAX_NETS);
    for (int i = 0; i < g_net_count; i++)
        g_nets[i].saved = wifi_is_saved(g_nets[i].ssid);
}

static void wifi_scan_start(void) {
    if (g_scan_pid > 0) return;
    find_wifi_if();
    if (!g_wif[0]) {
        char hardware[80] = "No Wi-Fi interface found";
        FILE *diagnostic = fopen("/fifi-data/wifi-hardware", "r");
        if (diagnostic) {
            if (fgets(hardware, sizeof hardware, diagnostic))
                hardware[strcspn(hardware, "\r\n")] = '\0';
            fclose(diagnostic);
        }
        snprintf(g_wstatus, sizeof g_wstatus, "No interface: %.79s", hardware);
        return;
    }

    int pfd[2];
    if (pipe(pfd) < 0) return;
    g_scan_pid = fork();
    if (g_scan_pid == 0) {
        close(pfd[0]); dup2(pfd[1], STDOUT_FILENO); dup2(pfd[1], STDERR_FILENO); close(pfd[1]);
        execl("/bin/fifi-admin","fifi-admin","wifi","scan",g_wif,NULL); _exit(1);
    }
    close(pfd[1]);
    g_scan_pipe = pfd[0];
    fcntl(g_scan_pipe, F_SETFL, O_NONBLOCK);
    g_scan_buf_len = 0;
    g_wstate = ST_SCANNING;
    snprintf(g_wstatus, sizeof g_wstatus, "Scanning on %s...", g_wif);
}

static void wifi_scan_poll(void) {
    if (g_scan_pipe < 0) return;
    char tmp[4096]; ssize_t n;
    while ((n = read(g_scan_pipe, tmp, sizeof tmp)) > 0) {
        if (g_scan_buf_len + (int)n < (int)sizeof(g_scan_buf) - 1) {
            memcpy(g_scan_buf + g_scan_buf_len, tmp, n);
            g_scan_buf_len += (int)n; g_scan_buf[g_scan_buf_len] = '\0';
        }
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(g_scan_pipe); g_scan_pipe = -1;
        int st = 0; while (waitpid(g_scan_pid, &st, 0) < 0 && errno == EINTR) {}
        g_scan_pid = -1;
        parse_scan(g_scan_buf);
        if (g_net_count == 0) {
            char first_line[64] = "";
            if (g_scan_buf[0]) {
                const char *nl = strchr(g_scan_buf, '\n');
                size_t length = nl ? (size_t)(nl - g_scan_buf) : strlen(g_scan_buf);
                if (length > sizeof(first_line) - 1) length = sizeof(first_line) - 1;
                memcpy(first_line, g_scan_buf, length);
            }
            if (first_line[0])
                snprintf(g_wstatus, sizeof g_wstatus, "Scan failed: %.63s", first_line);
            else
                snprintf(g_wstatus, sizeof g_wstatus, "No networks found -- press R to rescan");
        }
        else
            snprintf(g_wstatus, sizeof g_wstatus, "%d network%s -- click to select, again to connect",
                     g_net_count, g_net_count == 1 ? "" : "s");
        g_wstate = ST_IDLE; g_sel = 0; g_wscroll = 0;
    }
}

static int write_admin_bytes(int fd, const void *data, size_t length) {
    const unsigned char *bytes = data;
    while (length) {
        ssize_t wrote = write(fd, bytes, length);
        if (wrote < 0) { if (errno == EINTR) continue; return -1; }
        bytes += wrote; length -= (size_t)wrote;
    }
    return 0;
}

static void wifi_connect(const char *ssid, const char *password) {
    size_t ssid_len = strlen(ssid), password_len = strlen(password);
    if (ssid_len > 128 || password_len > 128) {
        snprintf(g_wstatus, sizeof g_wstatus, "Network name or password is too long");
        return;
    }
    int pfd[2];
    if (pipe(pfd) != 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[1]); dup2(pfd[0], STDIN_FILENO); close(pfd[0]);
        for (int i=3;i<64;i++) close(i);
        execl("/bin/fifi-admin","fifi-admin","wifi","connect",g_wif,NULL); _exit(1);
    }
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return; }
    close(pfd[0]);
    unsigned char sizes[2];
    sizes[0]=(unsigned char)(ssid_len>>8); sizes[1]=(unsigned char)ssid_len;
    int failed = write_admin_bytes(pfd[1],sizes,2) ||
                 write_admin_bytes(pfd[1],ssid,ssid_len);
    sizes[0]=(unsigned char)(password_len>>8); sizes[1]=(unsigned char)password_len;
    failed = failed || write_admin_bytes(pfd[1],sizes,2) ||
             write_admin_bytes(pfd[1],password,password_len);
    close(pfd[1]);
    int status=0;
    if (waitpid(pid,&status,0)<0 || failed || !WIFEXITED(status) || WEXITSTATUS(status)!=0) {
        snprintf(g_wstatus,sizeof g_wstatus,"Could not start WiFi connection"); return;
    }
    g_wstate = ST_CONNECTING;
    snprintf(g_wstatus, sizeof g_wstatus, "Connecting to %s... (~10-20s)", ssid);
}

static void wifi_check_conn(void) {
    if (g_wstate != ST_CONNECTING && g_wstate != ST_CONNECTED) return;
    char cmd[128]; snprintf(cmd, sizeof cmd, "/bin/ip -4 addr show %s 2>/dev/null", g_wif);
    FILE *p = popen(cmd, "r");
    if (!p) return;
    char buf[512] = {0}; fread(buf, 1, sizeof(buf)-1, p); pclose(p);
    if (strstr(buf, "inet ")) {
        char *s = strstr(buf, "inet ") + 5;
        char *e = strchr(s, '/'); if (!e) e = strchr(s, ' ');
        char ip[32] = {0};
        if (e) { int l = (int)(e - s); if (l>31) l=31; memcpy(ip, s, l); }
        snprintf(g_conn_ssid, sizeof g_conn_ssid, "%s", g_sel < g_net_count ? g_nets[g_sel].ssid : "");
        snprintf(g_wstatus, sizeof g_wstatus, "Connected to %s (%s)", g_conn_ssid, ip);
        g_wstate = ST_CONNECTED;
    }
}

static const char *signal_bars(int dbm) {
    if (dbm >= -50) return "||||";
    if (dbm >= -65) return "|||.";
    if (dbm >= -75) return "||..";
    return "|...";
}

/* ── Security status (read-only; reuses security-app checks) ─────────────── */
static bool g_sec_fw_on, g_sec_doh_on, g_sec_vpn_on, g_sec_tor_on;
static char g_sec_fw[96], g_sec_doh[96], g_sec_vpn[96], g_sec_tor[96];
static int  g_sec_priv;

static bool pid_alive(const char *pidfile) {
    int fd = open(pidfile, O_RDONLY);
    if (fd < 0) return false;
    char b[16] = {0}; read(fd, b, sizeof(b)-1); close(fd);
    pid_t pid = (pid_t)atoi(b);
    return pid > 0 && kill(pid, 0) == 0;
}

static void sec_update(void) {
    /* Firewall */
    g_sec_fw_on = false; snprintf(g_sec_fw, sizeof g_sec_fw, "Not configured");
    { int fd = open("/fifi-data/firewall.log", O_RDONLY);
      if (fd >= 0) { char b[512]={0}; read(fd,b,sizeof(b)-1); close(fd);
          if (strstr(b,"firewall: active")) { g_sec_fw_on=true; snprintf(g_sec_fw,sizeof g_sec_fw,"Active (default-deny inbound)"); }
          else if (strstr(b,"firewall: failed")) snprintf(g_sec_fw,sizeof g_sec_fw,"Failed (needs kernel nftables)"); } }
    /* DoH */
    if (access("/fifi-data/doh-enabled", F_OK) != 0) { g_sec_doh_on=false; snprintf(g_sec_doh,sizeof g_sec_doh,"Disabled"); }
    else if (pid_alive("/fifi-data/doh.pid")) { g_sec_doh_on=true; snprintf(g_sec_doh,sizeof g_sec_doh,"Active (DNS-over-HTTPS)"); }
    else { g_sec_doh_on=false; snprintf(g_sec_doh,sizeof g_sec_doh,"Enabled but not running"); }
    /* VPN */
    { int fd = open("/sys/class/net/wg0/operstate", O_RDONLY);
      if (fd >= 0) { close(fd); g_sec_vpn_on=true; snprintf(g_sec_vpn,sizeof g_sec_vpn,"Connected (wg0)"); }
      else { g_sec_vpn_on=false;
          snprintf(g_sec_vpn,sizeof g_sec_vpn, access("/fifi-data/wg0.conf",F_OK)==0
                   ? "Disconnected (config ready)" : "No config"); } }
    /* Tor */
    if (access("/fifi-data/tor-enabled", F_OK) != 0) { g_sec_tor_on=false; snprintf(g_sec_tor,sizeof g_sec_tor,"Disabled"); }
    else if (pid_alive("/fifi-data/tor.pid")) { g_sec_tor_on=true; snprintf(g_sec_tor,sizeof g_sec_tor,"Running (SOCKS5 127.0.0.1:9050)"); }
    else { g_sec_tor_on=false; snprintf(g_sec_tor,sizeof g_sec_tor,"Enabled but not running"); }
    /* Privacy: count telemetry blocks in /etc/hosts */
    g_sec_priv = 0;
    { int fd = open("/etc/hosts", O_RDONLY);
      if (fd >= 0) { static char b[65536]; ssize_t n = read(fd,b,sizeof(b)-1); close(fd);
          if (n > 0) { b[n]='\0'; char *l=b;
              while (*l) { char *nl=strchr(l,'\n'); if(nl)*nl='\0';
                  if ((strncmp(l,"0.0.0.0 ",8)==0 || strncmp(l,"127.0.0.1 ",10)==0) &&
                      !strstr(l,"localhost") && !strstr(l,"fifios")) g_sec_priv++;
                  if (!nl) break;
                  l = nl + 1; } } } }
}

/* ── Content-pane renderers ──────────────────────────────────────────────── */
#define CX   (SIDEBAR_W + CPAD)   /* content text left edge */
#define CTOP (TITLE_H + 16)       /* content top */

static void render_personalize(uint32_t *fb) {
    hot_reset();
    if (g_pers_scroll < 0) g_pers_scroll = 0;
    if (g_pers_scroll > g_pers_max_scroll) g_pers_scroll = g_pers_max_scroll;
    g_draw_y_offset = -g_pers_scroll;
    g_draw_clip_top = CTOP;
    g_draw_clip_bottom = g_win_h;
    int x = CX, y = CTOP;
    unsigned cur_accent = cfg_get_uint(FIFI_THEME_KEY_ACCENT, FIFI_THEME_DEFAULT_ACCENT);
    int cur_wall  = cfg_get_int(FIFI_THEME_KEY_WALLPAPER, FIFI_THEME_DEFAULT_WALLPAPER);
    int cur_panel = cfg_get_int(FIFI_THEME_KEY_PANEL_EDGE, FIFI_THEME_DEFAULT_PANEL_EDGE);
    int glass     = cfg_get_int(FIFI_THEME_KEY_FX_GLASS, FIFI_THEME_DEFAULT_FX_GLASS);
    int shadow    = cfg_get_int(FIFI_THEME_KEY_FX_SHADOWS, FIFI_THEME_DEFAULT_FX_SHADOWS);
    int dockf     = cfg_get_int(FIFI_THEME_KEY_DOCK_FLOAT, FIFI_THEME_DEFAULT_DOCK_FLOAT);
    int topbar    = cfg_get_int(FIFI_THEME_KEY_STATUSBAR, FIFI_THEME_DEFAULT_STATUSBAR);
    int radius    = cfg_get_int(FIFI_THEME_KEY_CORNER_RADIUS, FIFI_THEME_DEFAULT_CORNER_RADIUS);

    /* Accent swatches */
    y = section_hdr(fb, "Accent Color", x, y);
    { int sw = 30, gap = 8, per = 8;
      for (int i = 0; i < N_ACCENT; i++) {
          int col = i % per, row = i / per;
          int sx = x + col*(sw+gap), sy = y + row*(sw+gap);
          fill(fb, sx, sy, sw, sw, g_accent_presets[i]);
          bool sel = (g_accent_presets[i] == cur_accent);
          rect_border(fb, sx, sy, sw, sw, sel ? C_WHITE : C_BORDER);
          if (sel) rect_border(fb, sx-1, sy-1, sw+2, sw+2, C_WHITE);
          add_hot(sx, sy, sw, sw, ACT_ACCENT, i);
      }
      y += 2*(sw+gap) + 10;
    }

    /* Wallpaper */
    y = section_hdr(fb, "Wallpaper", x, y);
    { int bw = 78, bh = 30, gap = 6, per_row = 6;
      for (int i = 0; i < WALL_N; i++) {
          int bx = x + (i % per_row)*(bw+gap);
          int by = y + (i / per_row)*(bh+gap);
          draw_btn(fb, bx, by, bw, bh, g_wall_names[i], i == cur_wall);
          add_hot(bx, by, bw, bh, ACT_WALL, i);
      }
      y += ((WALL_N + per_row - 1)/per_row)*(bh+gap) + 14 - gap;
    }
    /* Image fit modes — only relevant for the Image wallpaper (id 5). */
    if (cur_wall == 5) {
        static const char *fit_names[4] = { "Fill", "Fit", "Stretch", "Center" };
        int cur_fit = cfg_get_int(FIFI_THEME_KEY_WALL_FIT, FIFI_THEME_DEFAULT_WALL_FIT);
        int bw = 92, bh = 28, gap = 6;
        for (int i = 0; i < 4; i++) {
            int bx = x + i*(bw+gap);
            draw_btn(fb, bx, y, bw, bh, fit_names[i], i == cur_fit);
            add_hot(bx, y, bw, bh, ACT_WALLFIT, i);
        }
        y += bh + 14;
    }

    /* Panel position */
    y = section_hdr(fb, "Panel Position", x, y);
    { int bw = 100, bh = 30, gap = 8;
      for (int i = 0; i < 4; i++) {
          int bx = x + i*(bw+gap);
          draw_btn(fb, bx, y, bw, bh, g_panel_names[i], i == cur_panel);
          add_hot(bx, y, bw, bh, ACT_PANEL, i);
      }
      y += bh + 14;
    }

    /* Effects */
    y = section_hdr(fb, "Effects", x, y);
    { int bw = 150, bh = 30, gap = 12;
      char gl[32], sh[32];
      snprintf(gl, sizeof gl, "Glass: %s",   glass  ? "On" : "Off");
      snprintf(sh, sizeof sh, "Shadows: %s", shadow ? "On" : "Off");
      draw_btn(fb, x, y, bw, bh, gl, glass);           add_hot(x, y, bw, bh, ACT_GLASS, 0);
      draw_btn(fb, x+bw+gap, y, bw, bh, sh, shadow);   add_hot(x+bw+gap, y, bw, bh, ACT_SHADOW, 0);
      y += bh + 12;

      /* Floating dock: detached rounded dock over the wallpaper vs an
       * edge-to-edge taskbar — the main next-gen layout switch. */
      char df[32], tb[32];
      snprintf(df, sizeof df, "Floating dock: %s", dockf  ? "On" : "Off");
      snprintf(tb, sizeof tb, "Top bar: %s",       topbar ? "On" : "Off");
      draw_btn(fb, x, y, bw + 60, bh, df, dockf);              add_hot(x, y, bw + 60, bh, ACT_DOCK, 0);
      draw_btn(fb, x+bw+60+gap, y, bw, bh, tb, topbar);        add_hot(x+bw+60+gap, y, bw, bh, ACT_STATUS, 0);
      y += bh + 12;

      /* Desk info overlay + taskbar auto-hide toggles. When the top bar is off,
       * the clock/battery/network indicators fold into the taskbar. */
      int deskinfo = cfg_get_int(FIFI_THEME_KEY_DESKTOP_INFO, FIFI_THEME_DEFAULT_DESKTOP_INFO);
      int autohide = cfg_get_int(FIFI_THEME_KEY_PANEL_AUTOHIDE, FIFI_THEME_DEFAULT_PANEL_AUTOHIDE);
      char di[32], ah[40];
      snprintf(di, sizeof di, "Desk info: %s",   deskinfo ? "On" : "Off");
      snprintf(ah, sizeof ah, "Auto-hide bar: %s", autohide ? "On" : "Off");
      draw_btn(fb, x, y, bw, bh, di, deskinfo);               add_hot(x, y, bw, bh, ACT_DESKINFO, 0);
      draw_btn(fb, x+bw+gap, y, bw + 40, bh, ah, autohide);   add_hot(x+bw+gap, y, bw + 40, bh, ACT_AUTOHIDE, 0);
      y += bh + 12;

      /* Clock format + taskbar thickness. */
      int clock12  = cfg_get_int(FIFI_THEME_KEY_CLOCK_12H, FIFI_THEME_DEFAULT_CLOCK_12H);
      int tbsize   = cfg_get_int(FIFI_THEME_KEY_PANEL_SIZE, FIFI_THEME_DEFAULT_PANEL_SIZE);
      char ck[32];
      snprintf(ck, sizeof ck, "Clock: %s", clock12 ? "12h" : "24h");
      draw_btn(fb, x, y, bw, bh, ck, clock12);                add_hot(x, y, bw, bh, ACT_CLOCK, 0);
      draw_str(fb, "Taskbar size:", x+bw+gap, y + 6, C_KEY);
      int tsx = x + bw + gap + 14*CW;
      draw_btn(fb, tsx, y, 30, bh, "-", false);               add_hot(tsx, y, 30, bh, ACT_TBSIZE, -8);
      char tsv[16]; snprintf(tsv, sizeof tsv, "+%d px", tbsize);
      draw_str(fb, tsv, tsx + 42, y + 6, C_VAL);
      draw_btn(fb, tsx + 100, y, 30, bh, "+", false);         add_hot(tsx + 100, y, 30, bh, ACT_TBSIZE, +8);
      y += bh + 12;
    }

    /* Taskbar alignment: where the app buttons sit along the bar. */
    y = section_hdr(fb, "Taskbar Alignment", x, y);
    { int bw = 100, bh = 30, gap = 8;
      static const char *align_names[3] = { "Left", "Center", "Right" };
      int cur_align = cfg_get_int(FIFI_THEME_KEY_PANEL_ALIGN, FIFI_THEME_DEFAULT_PANEL_ALIGN);
      for (int i = 0; i < 3; i++) {
          int bx = x + i*(bw+gap);
          draw_btn(fb, bx, y, bw, bh, align_names[i], i == cur_align);
          add_hot(bx, y, bw, bh, ACT_ALIGN, i);
      }
      y += bh + 14;
    }

    { int bw = 150, bh = 30;
      (void)bw; (void)bh;
      /* Corner radius stepper */
      draw_str(fb, "Corner radius:", x, y + 6, C_KEY);
      int rx = x + 15*CW;
      draw_btn(fb, rx, y, 30, bh, "-", false);          add_hot(rx, y, 30, bh, ACT_RADIUS, -1);
      char rv[16]; snprintf(rv, sizeof rv, "%d px", radius);
      draw_str(fb, rv, rx + 42, y + 6, C_VAL);
      draw_btn(fb, rx + 100, y, 30, bh, "+", false);    add_hot(rx + 100, y, 30, bh, ACT_RADIUS, +1);
      y += bh + 16;
    }

    /* System font — dropdown pickers: family (each name shown in its OWN face)
     * + size. The open list is drawn as an overlay at the END of this function
     * so it sits on top. The compositor persists font_file/font_px + live-applies. */
    y = section_hdr(fb, "System Font", x, y);
    { int bh = 32;
      int cur = font_cur_index();
      int fpx = cfg_get_int(FIFI_THEME_KEY_FONT_PX, FIFI_THEME_DEFAULT_FONT_PX);
      /* Family combo */
      int fw = 300;
      g_ff_bx = x; g_ff_by = y; g_ff_bw = fw; g_ff_bh = bh;
      fill(fb, x, y, fw, bh, C_BTN_BG);
      rect_border(fb, x, y, fw, bh, g_font_dd == 1 ? C_WHITE : C_BORDER);
      if (cur >= 0) {
          font_preview_draw(fb, cur, x + 8, y + 6, C_VAL, fw - 30);
      } else {
          draw_str_clip(fb, "(default)", x + 8, y + (bh - g_glyph_h)/2, C_VAL, (fw - 30)/9);
      }
      draw_str(fb, "v", x + fw - 15, y + (bh - g_glyph_h)/2, C_KEY);
      add_hot(x, y, fw, bh, ACT_FONT_FAM, 0);
      /* Size combo */
      int sx = x + fw + 16, sw = 96;
      g_fs_bx = sx; g_fs_by = y; g_fs_bw = sw; g_fs_bh = bh;
      fill(fb, sx, y, sw, bh, C_BTN_BG);
      rect_border(fb, sx, y, sw, bh, g_font_dd == 2 ? C_WHITE : C_BORDER);
      char sv[16]; snprintf(sv, sizeof sv, "%d px", fpx);
      draw_str(fb, sv, sx + 8, y + (bh - g_glyph_h)/2, C_VAL);
      draw_str(fb, "v", sx + sw - 15, y + (bh - g_glyph_h)/2, C_KEY);
      add_hot(sx, y, sw, bh, ACT_FONT_SZ, 0);
      y += bh + 14;
    }

    /* Note */
    fill(fb, x, y, g_win_w - x - CPAD, ROW_H, C_ROW_A);
    draw_str_clip(fb, g_pers_note, x + 8, y + (ROW_H - g_glyph_h)/2, C_YELLOW, g_win_w - x - CPAD - 16);

    /* ── Font dropdown overlay (drawn last = on top of everything) ────────── */
    if (g_font_dd == 1 && g_font_n > 0) {
        int rowh = 28, vis = 11;
        if (vis > g_font_n) vis = g_font_n;
        int dw = g_ff_bw, dx = g_ff_bx, dy = g_ff_by + g_ff_bh;
        if (dy - g_pers_scroll + rowh * vis > g_win_h)
            dy = g_ff_by - rowh * vis;
        g_dd_x = dx; g_dd_y = dy - g_pers_scroll;
        g_dd_w = dw; g_dd_rowh = rowh; g_dd_vis = vis;
        if (g_font_dd_scroll > g_font_n - vis) g_font_dd_scroll = g_font_n - vis;
        if (g_font_dd_scroll < 0) g_font_dd_scroll = 0;
        fill(fb, dx, dy, dw, rowh * vis, 0x00101a26u);
        rect_border(fb, dx, dy, dw, rowh * vis, C_WHITE);
        int cur = font_cur_index();
        for (int r = 0; r < vis; r++) {
            int idx = g_font_dd_scroll + r;
            if (idx >= g_font_n) break;
            int ry = dy + r * rowh;
            if (idx == cur) fill(fb, dx + 1, ry, dw - 2, rowh, g_accent);
            font_preview_draw(fb, idx, dx + 8, ry + 4, C_WHITE, dw - 20);
        }
        if (g_font_n > vis) {                       /* scrollbar */
            int trk = rowh * vis, th = trk * vis / g_font_n; if (th < 14) th = 14;
            int ty = dy + (trk - th) * g_font_dd_scroll / (g_font_n - vis);
            fill(fb, dx + dw - 5, dy, 4, trk, 0x00243448u);
            fill(fb, dx + dw - 5, ty, 4, th, C_KEY);
        }
    } else if (g_font_dd == 2) {
        int rowh = 28, vis = N_FONT_SIZES;
        int dw = g_fs_bw, dx = g_fs_bx, dy = g_fs_by + g_fs_bh;
        if (dy - g_pers_scroll + rowh * vis > g_win_h)
            dy = g_fs_by - rowh * vis;
        g_dd_x = dx; g_dd_y = dy - g_pers_scroll;
        g_dd_w = dw; g_dd_rowh = rowh; g_dd_vis = vis;
        fill(fb, dx, dy, dw, rowh * vis, 0x00101a26u);
        rect_border(fb, dx, dy, dw, rowh * vis, C_WHITE);
        int fpx = cfg_get_int(FIFI_THEME_KEY_FONT_PX, FIFI_THEME_DEFAULT_FONT_PX);
        for (int r = 0; r < vis; r++) {
            int ry = dy + r * rowh;
            if (g_font_sizes[r] == fpx) fill(fb, dx + 1, ry, dw - 2, rowh, g_accent);
            char sv[16]; snprintf(sv, sizeof sv, "%d px", g_font_sizes[r]);
            draw_str(fb, sv, dx + 8, ry + (rowh - g_glyph_h)/2, C_WHITE);
        }
    }

    int content_bottom = y + ROW_H;
    g_pers_max_scroll = content_bottom > g_win_h - 8 ?
                        content_bottom - (g_win_h - 8) : 0;
    if (g_pers_scroll > g_pers_max_scroll) g_pers_scroll = g_pers_max_scroll;
    g_draw_y_offset = 0;
    g_draw_clip_top = 0;
    g_draw_clip_bottom = g_win_h;
    if (g_pers_max_scroll > 0) {
        int track_y = CTOP, track_h = g_win_h - CTOP - 4;
        int thumb_h = track_h * track_h / (track_h + g_pers_max_scroll);
        if (thumb_h < 28) thumb_h = 28;
        int thumb_y = track_y + (track_h - thumb_h) * g_pers_scroll /
                      g_pers_max_scroll;
        fill(fb, g_win_w - 5, track_y, 3, track_h, C_BORDER);
        fill(fb, g_win_w - 5, thumb_y, 3, thumb_h, C_KEY);
    }
}

static void render_system(uint32_t *fb) {
    int x = CX, y = CTOP;
    gather_info();
    y = section_hdr(fb, "System Information", x, y);
    for (int i = 0; i < N_INFO; i++) {
        uint32_t bg = (i & 1) ? C_ROW_B : C_ROW_A;
        fill(fb, SIDEBAR_W, y, g_win_w - SIDEBAR_W, ROW_H, bg);
        int ty = y + (ROW_H - g_glyph_h)/2;
        draw_str(fb, g_info[i].key, x, ty, C_KEY);
        draw_str_clip(fb, g_info[i].val, x + 11*CW, ty, C_VAL, g_win_w - (x + 11*CW) - CPAD);
        y += ROW_H;
    }
    y += 16;

    y = section_hdr(fb, "Audio", x, y);
    g_sl_x = x + 9*CW;
    g_sl_y = y + (ROW_H - g_sl_h)/2;
    draw_str(fb, "Volume:", x, y + (ROW_H - g_glyph_h)/2, C_KEY);
    fill(fb, g_sl_x, g_sl_y, g_sl_w, g_sl_h, C_BORDER);
    int filled = g_sl_w * g_vol / 100;
    fill(fb, g_sl_x, g_sl_y, filled, g_sl_h, g_accent);
    fill(fb, g_sl_x + filled - 4, g_sl_y - 3, 8, g_sl_h + 6, C_WHITE);
    char pct[8]; snprintf(pct, sizeof pct, "%d%%", g_vol);
    draw_str(fb, pct, g_sl_x + g_sl_w + 10, y + (ROW_H - g_glyph_h)/2, C_VAL);
    y += ROW_H + 14;

    y = section_hdr(fb, "Pointer", x, y);
    int mouse_speed = cfg_get_int(FIFI_INPUT_KEY_MOUSE_SPEED,
                                  FIFI_INPUT_DEFAULT_MOUSE_SPEED);
    int touchpad_speed = cfg_get_int(FIFI_INPUT_KEY_TOUCHPAD_SPEED,
                                     FIFI_INPUT_DEFAULT_TOUCHPAD_SPEED);
    if (mouse_speed < -100) mouse_speed = -100;
    if (mouse_speed > 100) mouse_speed = 100;
    if (touchpad_speed < -100) touchpad_speed = -100;
    if (touchpad_speed > 100) touchpad_speed = 100;
    draw_speed_slider(fb, "Mouse speed:", x, y, mouse_speed,
                      &g_mouse_sl_x, &g_mouse_sl_y);
    y += ROW_H;
    draw_speed_slider(fb, "Touchpad speed:", x, y, touchpad_speed,
                      &g_touchpad_sl_x, &g_touchpad_sl_y);
    y += ROW_H + 14;

    y = section_hdr(fb, "Devices", x, y);
    fill(fb, SIDEBAR_W, y, g_win_w - SIDEBAR_W, ROW_H, C_ROW_A);
    draw_str(fb, "Gamepad:", x, y + (ROW_H - g_glyph_h)/2, C_KEY);
    { int gp = open("/dev/input/js0", O_RDONLY | O_NONBLOCK);
      if (gp >= 0) { close(gp); draw_str(fb, "Connected", x + 11*CW, y + (ROW_H - g_glyph_h)/2, C_GREEN); }
      else          draw_str(fb, "None", x + 11*CW, y + (ROW_H - g_glyph_h)/2, C_GREY); }
}

static void render_network(uint32_t *fb) {
    int x = CX, y = CTOP;
    y = section_hdr(fb, "Network Interfaces", x, y);
    if (g_nifaces == 0) {
        draw_str(fb, "No interfaces detected", x, y, C_GREY);
        return;
    }
    for (int i = 0; i < g_nifaces && i < 4; i++) {
        iface_t *ifc = &g_ifaces[i];
        char rxr[16], txr[16], rxt[16], txt[16];
        fmt_rate(ifc->rx_rate, rxr, sizeof rxr); fmt_rate(ifc->tx_rate, txr, sizeof txr);
        fmt_bytes(ifc->rx_bytes, rxt, sizeof rxt); fmt_bytes(ifc->tx_bytes, txt, sizeof txt);

        char nm[32]; snprintf(nm, sizeof nm, "%-10.15s", ifc->name);
        draw_str(fb, nm, x, y, C_WHITE);
        draw_str(fb, ifc->up ? "UP" : "DOWN", x + 11*CW, y, ifc->up ? C_GREEN : C_RED);
        draw_str(fb, ifc->ip4[0] ? ifc->ip4 : "---", x + 16*CW, y, C_VAL);
        y += g_glyph_h + 3;
        char rl[80]; snprintf(rl, sizeof rl, "  RX %-12s  TX %-12s", rxr, txr);
        draw_str(fb, rl, x, y, C_KEY); y += g_glyph_h + 3;
        char tl[80]; snprintf(tl, sizeof tl, "  Total RX %-10s  TX %-10s", rxt, txt);
        draw_str(fb, tl, x, y, C_GREY); y += g_glyph_h + 8;
        if (i < g_nifaces - 1 && i < 3) { fill(fb, x, y, g_win_w - x - CPAD, 1, C_BORDER); y += 8; }
    }
}

static void render_wifi(uint32_t *fb) {
    int x = CX, y = CTOP;
    int ww = g_win_w;
    /* Status bar */
    fill(fb, SIDEBAR_W, y - 4, ww - SIDEBAR_W, ROW_H, C_ROW_A);
    draw_str_clip(fb, g_wstatus, x, y + (ROW_H - g_glyph_h)/2 - 4, C_VAL, ww - x - CPAD);
    y += ROW_H + 4;
    fill(fb, x, y, ww - x - CPAD, 1, C_BORDER); y += 6;

    /* Column headers */
    draw_str(fb, "Sig  Network", x, y, C_KEY);
    draw_str(fb, "Security", ww - CPAD - 8*CW, y, C_KEY);
    y += g_glyph_h + 4;
    fill(fb, x, y, ww - x - CPAD, 1, C_BORDER); y += 4;

    g_list_top = y;
    int foot_reserve = g_pw_mode ? (ROW_H*3 + 16) : (g_glyph_h + 14);
    int rows_visible = (g_win_h - y - foot_reserve) / ROW_H;
    if (rows_visible < 1) rows_visible = 1;
    if (g_sel < g_wscroll) g_wscroll = g_sel;
    if (g_sel >= g_wscroll + rows_visible) g_wscroll = g_sel - rows_visible + 1;
    if (g_wscroll < 0) g_wscroll = 0;

    if (g_net_count == 0 && g_wstate == ST_IDLE) {
        draw_str(fb, "No networks -- press R to scan", x, y, C_GREY);
    } else {
        for (int i = g_wscroll; i < g_net_count && i < g_wscroll + rows_visible; i++) {
            bool sel = (i == g_sel);
            fill(fb, SIDEBAR_W, y, ww - SIDEBAR_W, ROW_H, sel ? C_ROW_SEL : ((i&1)?C_ROW_A:C_ROW_B));
            int ty = y + (ROW_H - g_glyph_h)/2;
            draw_str(fb, signal_bars(g_nets[i].signal), x, ty,
                     g_nets[i].signal >= -65 ? C_GREEN : C_YELLOW);
            if (g_nets[i].saved) draw_char(fb, '*', x + 5*CW, ty, C_GREEN);
            int sx = x + 6*CW;
            draw_str_clip(fb, g_nets[i].ssid, sx, ty, sel ? C_WHITE : C_VAL,
                          ww - CPAD - sx - 9*CW);
            draw_str(fb, g_nets[i].security, ww - CPAD - (int)strlen(g_nets[i].security)*CW, ty, C_GREY);
            y += ROW_H;
        }
    }

    /* Password overlay */
    if (g_pw_mode && g_sel < g_net_count) {
        int py = g_win_h - ROW_H*3 - 12;
        fill(fb, SIDEBAR_W, py - 4, ww - SIDEBAR_W, ROW_H*3 + 16, 0x000e1828u);
        fill(fb, SIDEBAR_W, py - 4, ww - SIDEBAR_W, 1, C_BORDER);
        char prompt[96]; snprintf(prompt, sizeof prompt, "Connect to: %s", g_nets[g_sel].ssid);
        draw_str_clip(fb, prompt, x, py + (ROW_H - g_glyph_h)/2, C_KEY, ww - x - CPAD);
        py += ROW_H + 2;
        char stars[130]; int sl = g_pw_len < 128 ? g_pw_len : 128;
        for (int i = 0; i < sl; i++) stars[i] = '*';
        stars[sl] = '|'; stars[sl+1] = '\0';
        char pl[160]; snprintf(pl, sizeof pl, "Password: %s", stars);
        draw_str_clip(fb, pl, x, py + (ROW_H - g_glyph_h)/2, C_YELLOW, ww - x - CPAD);
        py += ROW_H + 2;
        draw_str_clip(fb, "Enter=connect  Esc=cancel", x, py + (ROW_H - g_glyph_h)/2, C_GREY, ww - x - CPAD);
    } else {
        int fy = g_win_h - g_glyph_h - 6;
        draw_str_clip(fb, "R=scan  click=select  click-again/Enter=connect  D=disconnect",
                      x, fy, C_GREY, ww - x - CPAD);
    }
}

static void render_security(uint32_t *fb) {
    hot_reset();
    int x = CX, y = CTOP;
    y = section_hdr(fb, "Security", x, y);
    int btn_w = 90, btn_h = ROW_H - 8;
    int btn_x = g_win_w - CPAD - btn_w;
/* One toggleable protection row: status dot + name + detail + Enable/Disable button. */
#define SEC_ROW(name, on, txt, act) do { \
        fill(fb, SIDEBAR_W, y, g_win_w - SIDEBAR_W, ROW_H, C_ROW_A); \
        int ty = y + (ROW_H - g_glyph_h)/2; \
        draw_str(fb, (on) ? "[ON] " : "[OFF]", x, ty, (on) ? C_GREEN : C_RED); \
        draw_str(fb, (name), x + 6*CW, ty, C_KEY); \
        draw_str_clip(fb, (txt), x + 16*CW, ty, C_VAL, btn_x - (x+16*CW) - 8); \
        draw_btn(fb, btn_x, y + 4, btn_w, btn_h, (on) ? "Disable" : "Enable", false); \
        add_hot(btn_x, y + 4, btn_w, btn_h, (act), (on) ? 0 : 1); \
        y += ROW_H + 4; \
    } while (0)

    SEC_ROW("Firewall", g_sec_fw_on, g_sec_fw, ACT_FW);
    SEC_ROW("DoH", g_sec_doh_on, g_sec_doh, ACT_DOH);
    SEC_ROW("VPN", g_sec_vpn_on, g_sec_vpn, ACT_VPN);
    SEC_ROW("Tor", g_sec_tor_on, g_sec_tor, ACT_TOR);
#undef SEC_ROW
    /* Privacy row is status-only (managed via the hosts blocklist). */
    { char pv[64];
      fill(fb, SIDEBAR_W, y, g_win_w - SIDEBAR_W, ROW_H, C_ROW_A);
      int ty = y + (ROW_H - g_glyph_h)/2;
      bool on = g_sec_priv > 0;
      draw_str(fb, on ? "[ON] " : "[OFF]", x, ty, on ? C_GREEN : C_RED);
      draw_str(fb, "Privacy", x + 6*CW, ty, C_KEY);
      snprintf(pv, sizeof pv, on ? "%d telemetry domains blocked" : "No telemetry blocking", g_sec_priv);
      draw_str_clip(fb, pv, x + 16*CW, ty, C_VAL, g_win_w - (x+16*CW) - CPAD);
      y += ROW_H + 4;
    }
    y += 10;
    draw_str_clip(fb, "Toggle each protection above. Changes apply immediately.",
                  x, y, C_GREY, g_win_w - x - CPAD);
}

static void render_about(uint32_t *fb) {
    int x = CX, y = CTOP;
    y = section_hdr(fb, "About FiFi OS", x, y);

    char ver[96] = "FiFi OS (linux-desktop)";
    int vfd = open("/etc/fifi-version", O_RDONLY);
    if (vfd >= 0) { ssize_t n = read(vfd, ver, sizeof(ver)-1); close(vfd);
        if (n > 0) { ver[n]='\0'; char *nl=strchr(ver,'\n'); if(nl)*nl='\0'; } }

    struct utsname u; bool have_uts = (uname(&u) == 0);
    struct { const char *k, *v; } rows[] = {
        { "Version",  ver },
        { "Kernel",   have_uts ? u.release : "?" },
        { "Machine",  have_uts ? u.machine : "?" },
        { "Hostname", have_uts ? u.nodename : "?" },
    };
    for (int i = 0; i < (int)(sizeof(rows)/sizeof(rows[0])); i++) {
        fill(fb, SIDEBAR_W, y, g_win_w - SIDEBAR_W, ROW_H, (i&1)?C_ROW_B:C_ROW_A);
        int ty = y + (ROW_H - g_glyph_h)/2;
        draw_str(fb, rows[i].k, x, ty, C_KEY);
        draw_str_clip(fb, rows[i].v, x + 11*CW, ty, C_VAL, g_win_w - (x+11*CW) - CPAD);
        y += ROW_H;
    }
    y += 16;
    draw_str_clip(fb, "A hand-built Linux distro with a native C Wayland compositor.",
                  x, y, C_GREY, g_win_w - x - CPAD); y += g_glyph_h + 4;
    draw_str_clip(fb, "Updates: run fifi upgrade, then reboot when it finishes.",
                  x, y, C_GREY, g_win_w - x - CPAD);
}

/* ── Sidebar + top-level render ──────────────────────────────────────────── */
static void render(uint32_t *fb) {
    g_draw_y_offset = 0;
    g_draw_clip_top = 0;
    g_draw_clip_bottom = g_win_h;
    fill(fb, 0, 0, g_win_w, g_win_h, C_BG);
    /* Sidebar */
    fill(fb, 0, 0, SIDEBAR_W, g_win_h, C_SIDE);
    fill(fb, SIDEBAR_W - 1, 0, 1, g_win_h, C_BORDER);
    draw_str(fb, "Settings", 18, TITLE_H + 14, C_HDR);
    for (int i = 0; i < N_TABS; i++) {
        int ty = SIDE_TOP + i*SIDE_ROW;
        bool active = (i == g_tab);
        if (active) {
            fill(fb, 0, ty, SIDEBAR_W - 1, SIDE_ROW, C_SIDE_HOV);
            fill(fb, 0, ty, 4, SIDE_ROW, g_accent);   /* accent marker */
        }
        draw_str(fb, g_tab_labels[i], 20, ty + (SIDE_ROW - g_glyph_h)/2,
                 active ? C_WHITE : C_VAL);
    }

    /* Content */
    switch (g_tab) {
        case TAB_PERS:  render_personalize(fb); break;
        case TAB_WIFI:  render_wifi(fb);        break;
        case TAB_NET:   render_network(fb);     break;
        case TAB_SYS:   render_system(fb);      break;
        case TAB_SEC:   render_security(fb);    break;
        case TAB_ABOUT: render_about(fb);       break;
    }
}

/* ── IPC helpers ─────────────────────────────────────────────────────────── */
static void send_frame(int fd, uint32_t *px) {
    (void)fifi_app_ipc_send_frame(fd, (uint16_t)g_win_w, (uint16_t)g_win_h, px);
}

static void send_font_dropdown(int fd, uint32_t *px) {
    int height = g_dd_rowh * g_dd_vis;
    if (g_dd_x < 0 || g_dd_y < 0 || g_dd_w <= 0 || height <= 0 ||
        g_dd_x + g_dd_w > g_win_w || g_dd_y + height > g_win_h) {
        send_frame(fd, px);
        return;
    }
    (void)fifi_app_ipc_send_region(fd, (uint16_t)g_win_w, (uint16_t)g_win_h, px,
                                   (uint16_t)g_dd_x, (uint16_t)g_dd_y,
                                   (uint16_t)g_dd_w, (uint16_t)height);
}

/* ── Message parser state ────────────────────────────────────────────────── */
typedef struct { uint8_t hdr[8]; int hgot; uint32_t type, plen, pgot; uint8_t *pld; } MsgState;
static bool msg_feed(MsgState *m, const uint8_t *buf, int n, int *pos) {
    while (*pos < n) {
        if (m->hgot < 8) {
            m->hdr[m->hgot++] = buf[(*pos)++];
            if (m->hgot == 8) {
                memcpy(&m->type, m->hdr, 4); memcpy(&m->plen, m->hdr+4, 4);
                if (m->plen > 4*1024*1024u) { m->hgot = 0; return false; }
                m->pgot = 0; free(m->pld); m->pld = NULL;
                /* On malloc failure pld stays NULL: payload is skipped in sync */
                if (m->plen > 0) m->pld = malloc(m->plen);
                else return true;
            }
        } else {
            uint32_t need = m->plen - m->pgot, have = (uint32_t)(n - *pos);
            uint32_t take = need < have ? need : have;
            if (m->pld) memcpy(m->pld + m->pgot, buf + *pos, take);
            m->pgot += take; *pos += (int)take;
            if (m->pgot >= m->plen) return true;
        }
    }
    return false;
}
static void msg_reset(MsgState *m) { free(m->pld); m->pld = NULL; m->hgot = 0; m->plen = 0; m->pgot = 0; }

/* ── Tab switching ───────────────────────────────────────────────────────── */
static void switch_tab(int t) {
    if (t < 0 || t >= N_TABS) return;
    g_tab = t;
    g_font_dd = 0;               /* close any open font dropdown */
    g_pw_mode = false; g_pw_len = 0;
    if (t == TAB_WIFI && !g_wifi_scanned) { g_wifi_scanned = true; wifi_scan_start(); }
    if (t == TAB_NET)  net_update();
    if (t == TAB_SEC)  sec_update();
}

/* ── Personalize click handling ──────────────────────────────────────────── */
static void pers_click(int mx, int my) {
    /* A font dropdown is open: a click either picks a list item or closes it. */
    if (g_font_dd == 1) {
        if (mx >= g_dd_x && mx < g_dd_x + g_dd_w &&
            my >= g_dd_y && my < g_dd_y + g_dd_rowh * g_dd_vis) {
            int idx = g_font_dd_scroll + (my - g_dd_y) / g_dd_rowh;
            if (idx >= 0 && idx < g_font_n) {
                char pathbuf[96];
                snprintf(pathbuf, sizeof pathbuf, "/fonts/%s", g_font_files[idx]);
                cfg_set_str(FIFI_THEME_KEY_FONT_FILE, pathbuf); cfg_save();
            }
        }
        g_font_dd = 0; return;
    }
    if (g_font_dd == 2) {
        if (mx >= g_dd_x && mx < g_dd_x + g_dd_w &&
            my >= g_dd_y && my < g_dd_y + g_dd_rowh * g_dd_vis) {
            int r = (my - g_dd_y) / g_dd_rowh;
            if (r >= 0 && r < N_FONT_SIZES) { cfg_set_int(FIFI_THEME_KEY_FONT_PX, g_font_sizes[r]); cfg_save(); }
        }
        g_font_dd = 0; return;
    }
    for (int i = 0; i < g_nhots; i++) {
        Hot *h = &g_hots[i];
        if (mx < h->x || mx >= h->x + h->w || my < h->y || my >= h->y + h->h) continue;
        switch (h->act) {
            case ACT_ACCENT: cfg_set_uint(FIFI_THEME_KEY_ACCENT, g_accent_presets[h->arg]);
                             g_accent = g_accent_presets[h->arg]; break;
            case ACT_WALL:   cfg_set_int(FIFI_THEME_KEY_WALLPAPER, h->arg); break;
            case ACT_WALLFIT: cfg_set_int(FIFI_THEME_KEY_WALL_FIT, h->arg); break;
            case ACT_PANEL:  cfg_set_int(FIFI_THEME_KEY_PANEL_EDGE, h->arg); break;
            case ACT_GLASS:  cfg_set_int(FIFI_THEME_KEY_FX_GLASS, cfg_get_int(FIFI_THEME_KEY_FX_GLASS, FIFI_THEME_DEFAULT_FX_GLASS) ? 0 : 1); break;
            case ACT_SHADOW: cfg_set_int(FIFI_THEME_KEY_FX_SHADOWS, cfg_get_int(FIFI_THEME_KEY_FX_SHADOWS, FIFI_THEME_DEFAULT_FX_SHADOWS) ? 0 : 1); break;
            case ACT_DOCK:   cfg_set_int(FIFI_THEME_KEY_DOCK_FLOAT, cfg_get_int(FIFI_THEME_KEY_DOCK_FLOAT, FIFI_THEME_DEFAULT_DOCK_FLOAT) ? 0 : 1); break;
            case ACT_STATUS: cfg_set_int(FIFI_THEME_KEY_STATUSBAR, cfg_get_int(FIFI_THEME_KEY_STATUSBAR, FIFI_THEME_DEFAULT_STATUSBAR) ? 0 : 1); break;
            case ACT_DESKINFO: cfg_set_int(FIFI_THEME_KEY_DESKTOP_INFO, cfg_get_int(FIFI_THEME_KEY_DESKTOP_INFO, FIFI_THEME_DEFAULT_DESKTOP_INFO) ? 0 : 1); break;
            case ACT_AUTOHIDE: cfg_set_int(FIFI_THEME_KEY_PANEL_AUTOHIDE, cfg_get_int(FIFI_THEME_KEY_PANEL_AUTOHIDE, FIFI_THEME_DEFAULT_PANEL_AUTOHIDE) ? 0 : 1); break;
            case ACT_ALIGN:  cfg_set_int(FIFI_THEME_KEY_PANEL_ALIGN, h->arg); break;
            case ACT_CLOCK:  cfg_set_int(FIFI_THEME_KEY_CLOCK_12H, cfg_get_int(FIFI_THEME_KEY_CLOCK_12H, FIFI_THEME_DEFAULT_CLOCK_12H) ? 0 : 1); break;
            case ACT_TBSIZE: { int s = cfg_get_int(FIFI_THEME_KEY_PANEL_SIZE, FIFI_THEME_DEFAULT_PANEL_SIZE) + h->arg;
                               if (s < 0) s = 0; if (s > 48) s = 48;
                               cfg_set_int(FIFI_THEME_KEY_PANEL_SIZE, s); } break;
            case ACT_RADIUS: { int r = cfg_get_int(FIFI_THEME_KEY_CORNER_RADIUS, FIFI_THEME_DEFAULT_CORNER_RADIUS) + h->arg;
                               if (r < 0) r = 0;
                               if (r > 12) r = 12;
                               cfg_set_int(FIFI_THEME_KEY_CORNER_RADIUS, r); } break;
            case ACT_FONT_FAM: {              /* open family dropdown */
                if (g_font_n == 0) return;
                g_font_dd = 1;
                int cur = font_cur_index();
                g_font_dd_scroll = (cur > 5) ? cur - 5 : 0;
                return;                        /* no cfg_save — just opening */
            }
            case ACT_FONT_SZ:                  /* open size dropdown */
                g_font_dd = 2;
                return;
        }
        cfg_save();
        return;
    }
}

/* ── Security click handling ─────────────────────────────────────────────── */
static void secctl(const char *what, int on) {
    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 3; i < 64; i++) close(i);
        execl("/bin/fifi-admin", "fifi-admin", "security", what,
              on ? "on" : "off", (char *)NULL);
        _exit(1);
    }
}
static void sec_click(int mx, int my) {
    for (int i = 0; i < g_nhots; i++) {
        Hot *h = &g_hots[i];
        if (mx < h->x || mx >= h->x + h->w || my < h->y || my >= h->y + h->h) continue;
        switch (h->act) {
            case ACT_FW:  secctl("firewall", h->arg); break;
            case ACT_DOH: secctl("doh",      h->arg); break;
            case ACT_VPN: secctl("vpn",      h->arg); break;
            case ACT_TOR: secctl("tor",      h->arg); break;
            default: continue;
        }
        return;
    }
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc > 1) {
        if      (!strcmp(argv[1], "wifi"))        g_tab = TAB_WIFI;
        else if (!strcmp(argv[1], "network"))     g_tab = TAB_NET;
        else if (!strcmp(argv[1], "system"))      g_tab = TAB_SYS;
        else if (!strcmp(argv[1], "security"))    g_tab = TAB_SEC;
        else if (!strcmp(argv[1], "about"))       g_tab = TAB_ABOUT;
        else if (!strcmp(argv[1], "personalize") ||
                 !strcmp(argv[1], "personalization")) g_tab = TAB_PERS;
    }

    if (!font_load("/fifi-data/fonts/ter16b.psf"))
        fifi_ui_font_init_blank(&g_font, 256, 8, 16);
    alsa_init();
    cfg_load();
    font_list_scan();
    font_previews_build();
    g_accent = cfg_get_uint(FIFI_THEME_KEY_ACCENT, FIFI_THEME_DEFAULT_ACCENT);
    net_update();
    sec_update();

    uint32_t *fb = malloc((size_t)g_win_w * g_win_h * 4);
    if (!fb) return 1;

    int sock = fifi_app_ipc_connect(WIN_W, WIN_H, "Settings");
    if (sock < 0) return 1;

    { uint8_t hdr[8]; if (read(sock, hdr, 8) == 8) {
        uint32_t pl; memcpy(&pl, hdr+4, 4);
        if (pl > 0 && pl < 64) { uint8_t r[64]; read(sock, r, pl); } } }

    signal(SIGPIPE, SIG_IGN);
    if (g_tab == TAB_WIFI && !g_wifi_scanned) { g_wifi_scanned = true; wifi_scan_start(); }
    render(fb); send_frame(sock, fb);
    fcntl(sock, F_SETFL, O_NONBLOCK);

    MsgState ms = {0};
    bool running = true, prev_lb = false;
    time_t last_refresh = 0;
    struct timespec last_conn = {0,0}; clock_gettime(CLOCK_MONOTONIC, &last_conn);

    while (running) {
        struct pollfd pfds[2];
        pfds[0].fd = sock;                                    pfds[0].events = POLLIN;
        pfds[1].fd = g_scan_pipe >= 0 ? g_scan_pipe : -1;     pfds[1].events = POLLIN;
        poll(pfds, 2, 150);

        bool dirty = false, font_dropdown_scrolled = false;

        /* Wi-Fi scan pipe */
        if (g_scan_pipe >= 0 && (pfds[1].revents & (POLLIN|POLLHUP|POLLERR))) {
            bool was = (g_wstate == ST_SCANNING);
            wifi_scan_poll();
            if (was && g_wstate != ST_SCANNING) dirty = true;
        }

        /* Wi-Fi connection progress */
        if (g_wstate == ST_CONNECTING) {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            long ms_el = (now.tv_sec-last_conn.tv_sec)*1000 + (now.tv_nsec-last_conn.tv_nsec)/1000000;
            if (ms_el >= 1000) { last_conn = now; wifi_check_conn(); dirty = true; }
        }

        /* Socket messages */
        if (pfds[0].revents & POLLIN) {
            uint8_t tbuf[2048];
            ssize_t n = read(sock, tbuf, sizeof tbuf);
            if (n == 0) break;
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
            if (n > 0) {
                int pos = 0;
                while (pos < (int)n) {
                    if (!msg_feed(&ms, tbuf, (int)n, &pos)) break;
                    if (ms.type == IPC_INPUT_KEY && ms.plen >= 1 && ms.pld) {
                        uint8_t k = ms.pld[0];
                        if (g_tab == TAB_WIFI && g_pw_mode) {
                            if (k == 0x1Bu) { g_pw_mode = false; g_pw_len = 0; }
                            else if (k == 0x0Du || k == '\n') {
                                g_pw_buf[g_pw_len] = '\0'; g_pw_mode = false;
                                if (g_sel < g_net_count) wifi_connect(g_nets[g_sel].ssid, g_pw_buf);
                                clock_gettime(CLOCK_MONOTONIC, &last_conn);
                            } else if ((k == 0x08u || k == 0x7Fu) && g_pw_len > 0) g_pw_len--;
                            else if (k >= 0x20u && k < 0x7Fu && g_pw_len < 127) g_pw_buf[g_pw_len++] = (char)k;
                            dirty = true;
                        } else if (k >= '1' && k <= '6') {
                            switch_tab(k - '1'); dirty = true;
                        } else if (k == 'q' || k == 'Q' || k == 0x1Bu) {
                            running = false;
                        } else if (g_tab == TAB_WIFI) {
                            if (k == 'r' || k == 'R') { if (g_wstate != ST_SCANNING) wifi_scan_start(); dirty = true; }
                            else if (k == 0x82u) { if (g_net_count) { if (--g_sel < 0) g_sel = g_net_count-1; dirty = true; } }
                            else if (k == 0x83u) { if (g_net_count) { if (++g_sel >= g_net_count) g_sel = 0; dirty = true; } }
                            else if (k == 0x0Du || k == '\n') {
                                if (g_sel < g_net_count) {
                                    if (!strcmp(g_nets[g_sel].security, "Open")) {
                                        wifi_connect(g_nets[g_sel].ssid, "");
                                        clock_gettime(CLOCK_MONOTONIC, &last_conn);
                                    } else { g_pw_mode = true; g_pw_len = 0; memset(g_pw_buf, 0, sizeof g_pw_buf); }
                                    dirty = true;
                                }
                            } else if (k == 'd' || k == 'D') {
                                pid_t pid = fork();
                                if (pid == 0) { for(int i=3;i<64;i++) close(i);
                                    execl("/bin/fifi-admin","fifi-admin","wifi","disconnect",g_wif,NULL); _exit(1); }
                                if (pid > 0) { int st; waitpid(pid, &st, 0); }
                                g_wstate = ST_IDLE; g_conn_ssid[0] = '\0';
                                snprintf(g_wstatus, sizeof g_wstatus, "Disconnected -- press R to scan");
                                dirty = true;
                            }
                        }
                    } else if (ms.type == IPC_INPUT_MOUSE && ms.plen >= 9 && ms.pld) {
                        int32_t mx, my; memcpy(&mx, ms.pld, 4); memcpy(&my, ms.pld+4, 4);
                        uint8_t btns = ms.pld[8];
                        int8_t wheel = ms.plen >= 10 ? (int8_t)ms.pld[9] : 0;
                        bool lb = (btns & 1);

                        /* Sidebar tab selection */
                        if (lb && !prev_lb && mx < SIDEBAR_W && my >= SIDE_TOP) {
                            int t = (my - SIDE_TOP) / SIDE_ROW;
                            if (t >= 0 && t < N_TABS) { switch_tab(t); dirty = true; }
                        } else if (mx >= SIDEBAR_W) {
                            /* Content-area input, per active tab */
                            if (g_tab == TAB_PERS) {
                                if (wheel != 0 && g_font_dd == 1) {
                                    g_font_dd_scroll -= wheel * 5;
                                    font_dropdown_scrolled = true;
                                    dirty = true;
                                }
                                else if (wheel != 0 && g_font_dd == 0) {
                                    g_pers_scroll -= wheel * 48;
                                    if (g_pers_scroll < 0) g_pers_scroll = 0;
                                    if (g_pers_scroll > g_pers_max_scroll)
                                        g_pers_scroll = g_pers_max_scroll;
                                    dirty = true;
                                }
                                if (lb && !prev_lb) { pers_click(mx, my); dirty = true; }
                            }
                            else if (g_tab == TAB_SEC && lb && !prev_lb) { sec_click(mx, my); dirty = true; }
                            else if (g_tab == TAB_SYS && lb) {
                                if (my >= g_sl_y - 6 && my <= g_sl_y + g_sl_h + 6 &&
                                    mx >= g_sl_x && mx < g_sl_x + g_sl_w) {
                                    int nv = (mx - g_sl_x) * 100 / g_sl_w;
                                    alsa_set_vol(nv < 0 ? 0 : nv > 100 ? 100 : nv);
                                    dirty = true;
                                } else if (my >= g_mouse_sl_y - 6 &&
                                           my <= g_mouse_sl_y + g_pointer_sl_h + 6 &&
                                           mx >= g_mouse_sl_x &&
                                           mx < g_mouse_sl_x + g_pointer_sl_w) {
                                    int nv = (mx - g_mouse_sl_x) * 200 /
                                             g_pointer_sl_w - 100;
                                    cfg_set_int(FIFI_INPUT_KEY_MOUSE_SPEED, nv);
                                    cfg_save(); dirty = true;
                                } else if (my >= g_touchpad_sl_y - 6 &&
                                           my <= g_touchpad_sl_y + g_pointer_sl_h + 6 &&
                                           mx >= g_touchpad_sl_x &&
                                           mx < g_touchpad_sl_x + g_pointer_sl_w) {
                                    int nv = (mx - g_touchpad_sl_x) * 200 /
                                             g_pointer_sl_w - 100;
                                    cfg_set_int(FIFI_INPUT_KEY_TOUCHPAD_SPEED, nv);
                                    cfg_save(); dirty = true;
                                }
                            }
                            else if (g_tab == TAB_WIFI) {
                                if (wheel != 0 && g_net_count > 0) {
                                    g_sel -= wheel;
                                    if (g_sel < 0) g_sel = 0;
                                    if (g_sel >= g_net_count) g_sel = g_net_count-1;
                                    dirty = true;
                                }
                                if (lb && !prev_lb && g_net_count > 0 && g_list_top > 0 &&
                                    my >= g_list_top && !g_pw_mode) {
                                    int clicked = (my - g_list_top) / ROW_H + g_wscroll;
                                    if (clicked >= 0 && clicked < g_net_count) {
                                        if (clicked == g_sel) {
                                            if (!strcmp(g_nets[g_sel].security, "Open")) {
                                                wifi_connect(g_nets[g_sel].ssid, "");
                                                clock_gettime(CLOCK_MONOTONIC, &last_conn);
                                            } else { g_pw_mode = true; g_pw_len = 0; memset(g_pw_buf, 0, sizeof g_pw_buf); }
                                        } else g_sel = clicked;
                                        dirty = true;
                                    }
                                }
                            }
                        }
                        prev_lb = lb;
                    } else if (ms.type == IPC_WIN_RESIZE && ms.plen >= 4 && ms.pld) {
                        uint16_t nw, nh; memcpy(&nw, ms.pld, 2); memcpy(&nh, ms.pld+2, 2);
                        if (nw >= 480 && nh >= 320 && nw <= 8192 && nh <= 8192) {
                            uint32_t *nb = realloc(fb, (size_t)nw * nh * 4);
                            if (nb) { fb = nb; g_win_w = nw; g_win_h = nh; }
                        }
                        dirty = true;
                    } else if (ms.type == IPC_INVALIDATE) {
                        dirty = true;
                    } else if (ms.type == IPC_APP_CLOSE) {
                        running = false;
                    }
                    msg_reset(&ms);
                }
            }
        }

        /* Periodic refresh of live panes */
        time_t now = time(NULL);
        if (now != last_refresh) {
            last_refresh = now;
            if (g_tab == TAB_NET) net_update();
            if (g_tab == TAB_SEC) sec_update();
            if (g_tab == TAB_SYS || g_tab == TAB_NET || g_tab == TAB_SEC || g_tab == TAB_WIFI)
                dirty = true;
        }

        if (dirty && running) {
            render(fb);
            if (font_dropdown_scrolled && g_font_dd == 1)
                send_font_dropdown(sock, fb);
            else
                send_frame(sock, fb);
        }
    }

    if (g_scan_pid > 0) kill(g_scan_pid, SIGTERM);
    (void)fifi_app_ipc_send(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock);
    free(fb);
    fifi_ui_font_destroy(&g_font);
    return 0;
}
