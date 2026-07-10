#include "gui_internal.h"
#ifdef __linux__
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#endif

/* ── Context menu width helpers (font-scaled) ───────────────────────── */
/* "Show Desktop"/"File Browser" = 12 chars, "Add to Desktop" = 14, "Select All" = 10 */
uint64_t ctx_w(void)     { uint64_t f = console_font_width(); return f ? 12u*f+24u : 168u; }
uint64_t fb_ctx_w(void)  { uint64_t f = console_font_width(); return f ? 14u*f+24u : 192u; }
uint64_t txt_ctx_w(void) { uint64_t f = console_font_width(); return f ? 10u*f+24u : 144u; }

/* ── Kickoff launcher (searchable app menu) ──────────────────────────────
 * A dynamic, filterable menu: a search box on top and a scrollable list of
 * every launchable thing — built-in windows, standalone FiFi apps, and any
 * App Store app installed under /fifi-data/apps (each has a <Name>.sh). Type
 * to filter, up/down to move, Enter to launch, right-click to pin to desktop.
 * g_launcher_hover doubles as the selected filtered-row index. */

launch_entry_t g_launch[LAUNCH_MAX];
int            g_launch_n;
int            g_launch_filt[LAUNCH_MAX];
int            g_launch_filt_n;
char           g_launch_q[40];
int            g_launch_qlen;
int            g_launcher_scroll;
/* Frosted-glass backdrop capture for the launcher (see launcher_draw). */
uint32_t      *g_launcher_bg = 0;
uint64_t       g_lbg_x = 0, g_lbg_y = 0, g_lbg_w = 0, g_lbg_h = 0;

extern void gui_desktop_save(void);

static char lw_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static bool lw_substr_ci(const char *hay, const char *needle) {
    if (!needle[0]) return true;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && lw_lc(*a) == lw_lc(*b)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

/* App-icon cache: an installed app's launcher /fifi-data/apps/<Name>.sh has a
 * sibling <Name>.png (written by appstore-install.sh). Decode once, keyed by the
 * entry's exec path, and blit it in place of the colored dot. Linux-only — the
 * PNG loader is a weak symbol absent on bare-metal. */
#ifdef __linux__
__attribute__((weak)) uint32_t *fifi_load_png(const char *path, uint32_t *w, uint32_t *h);

/* Resolve an app icon for an exec path, trying two locations in order:
 *   1. sibling <stem>.png  (installed AppImage apps ship this next to <Name>.sh)
 *   2. /fifi-data/icons/<basename>.png  (bundled icons for built-in/standalone apps)
 * Returns a freshly-loaded ARGB buffer (caller caches it) or NULL. */
uint32_t *app_load_icon_png(const char *exec, uint32_t *w, uint32_t *h) {
    if (!fifi_load_png || !exec || !exec[0]) return NULL;
    char png[256];
    size_t l = strlen(exec);
    const char *dot = strrchr(exec, '.');
    size_t stem = dot ? (size_t)(dot - exec) : l;
    if (stem > sizeof(png) - 5) stem = sizeof(png) - 5;
    memcpy(png, exec, stem);
    memcpy(png + stem, ".png", 5);
    uint32_t *img = fifi_load_png(png, w, h);
    if (img) return img;
    const char *base = exec;
    for (const char *p = exec; *p; p++) if (*p == '/') base = p + 1;
    size_t bl = strlen(base);
    const char *bdot = strrchr(base, '.');
    if (bdot) bl = (size_t)(bdot - base);
    static const char pre[] = "/fifi-data/icons/";
    if (sizeof(pre) - 1 + bl + 5 < sizeof(png)) {
        memcpy(png, pre, sizeof(pre) - 1);
        memcpy(png + sizeof(pre) - 1, base, bl);
        memcpy(png + sizeof(pre) - 1 + bl, ".png", 5);
        img = fifi_load_png(png, w, h);
    }
    return img;
}

typedef struct { char path[192]; uint32_t *img; uint32_t w, h; bool tried; } licon_t;
static licon_t g_licons[LAUNCH_MAX];

static licon_t *launch_icon(int i) {
    if (!fifi_load_png || i < 0 || i >= g_launch_n) return NULL;
    const char *ex = g_launch[i].exec;
    if (!ex[0]) return NULL;   /* power actions: no icon file */
    licon_t *c = &g_licons[i];
    if (strncmp(c->path, ex, sizeof(c->path)) != 0) {
        if (c->img) { free(c->img); c->img = NULL; }
        c->tried = false;
        strncpy(c->path, ex, sizeof(c->path) - 1);
        c->path[sizeof(c->path) - 1] = '\0';
    }
    if (!c->tried) {
        c->tried = true;
        c->img = app_load_icon_png(ex, &c->w, &c->h);
    }
    return c->img ? c : NULL;
}
#endif

static const char *launch_builtin_path(int slot) {
    switch (slot) {
        case 0: return "/bin/fifi-terminal";
        case 1: return "/bin/fifi-filebrowser";
        case 2: return "/bin/fifi-settings";
        case 3: return "/bin/fifi-imageviewer";
        default: return NULL;
    }
}

static void launch_add(const char *label, const char *exec, int8_t builtin, uint8_t power) {
    if (g_launch_n >= LAUNCH_MAX) return;
    launch_entry_t *e = &g_launch[g_launch_n++];
    int i = 0; for (; label[i] && i < 39; i++) e->label[i] = label[i]; e->label[i] = '\0';
    int j = 0; if (exec) { for (; exec[j] && j < 191; j++) e->exec[j] = exec[j]; } e->exec[j] = '\0';
    e->builtin = builtin; e->power = power;
}

void launcher_open_reset(void) {
    g_launch_n = 0;
    g_launch_q[0] = '\0'; g_launch_qlen = 0;
    g_launcher_hover = 0; g_launcher_scroll = 0;
    g_launchctx_row = -1; g_launchctx_hover = -1;
    /* Drop any stale frosted-glass backdrop so this open recaptures a fresh one. */
    if (g_launcher_bg) { kfree(g_launcher_bg); g_launcher_bg = 0; }

    /* Built-in windows: exec is set only so the icon resolver finds a logo;
     * launcher_do_launch checks .builtin first, so these still open the window. */
    launch_add("Terminal",        "/bin/fifi-terminal",    0, 0);
    launch_add("Files",           "/bin/fifi-filebrowser", 1, 0);
    launch_add("Settings",        "/bin/fifi-settings",    2, 0);
    launch_add("Image Viewer",    "/bin/fifi-imageviewer", 3, 0);
    /* App Store ships in the OS image at /bin; older deploys have it on the
     * data partition only, so keep that as a fallback. */
    {
        FILE *asf = fopen("/bin/fifi-appstore", "rb");
        if (asf) fclose(asf);
        launch_add("App Store",
                   asf ? "/bin/fifi-appstore" : "/fifi-data/apps/fifi-appstore",
                   -1, 0);
    }
    launch_add("Text Editor",     "/bin/fifi-editor",    -1, 0);
    launch_add("Calculator",      "/bin/fifi-calc",      -1, 0);
    launch_add("System Monitor",  "/bin/fifi-sysmon",    -1, 0);
    launch_add("Network Monitor", "/bin/fifi-netmon",    -1, 0);
    launch_add("Security",        "/bin/fifi-security",  -1, 0);
    launch_add("WiFi",            "/bin/fifi-wifi",      -1, 0);
    launch_add("Gamepad",         "/bin/fifi-gamepad",   -1, 0);
    launch_add("Proton Config",   "/bin/fifi-proton",    -1, 0);

#ifdef __linux__
    /* Installed App Store apps: each is /fifi-data/apps/<Name>.sh */
    DIR *d = opendir("/fifi-data/apps");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            const char *nm = de->d_name;
            int l = 0; while (nm[l]) l++;
            if (l < 4) continue;
            if (!(nm[l-3] == '.' && nm[l-2] == 's' && nm[l-1] == 'h')) continue;
            /* Skip the App Store's own helper scripts (appstore-*.sh) — not apps. */
            if (strncmp(nm, "appstore-", 9) == 0) continue;
            if (strncmp(nm, "fifi-",     5) == 0) continue;
            char base[40]; int bl = l - 3; if (bl > 39) bl = 39;
            for (int k = 0; k < bl; k++) base[k] = nm[k]; base[bl] = '\0';
            char path[192]; snprintf(path, sizeof(path), "/fifi-data/apps/%s", nm);
            launch_add(base, path, -1, 0);
        }
        closedir(d);
    }
#endif

    launch_add("Sleep",    NULL, -1, 1);
    launch_add("Restart",  NULL, -1, 2);
    launch_add("Shutdown", NULL, -1, 3);

    launcher_filter();
}

void launcher_filter(void) {
    g_launch_filt_n = 0;
    for (int i = 0; i < g_launch_n; i++)
        if (lw_substr_ci(g_launch[i].label, g_launch_q))
            g_launch_filt[g_launch_filt_n++] = i;
    if (g_launcher_hover >= g_launch_filt_n) g_launcher_hover = g_launch_filt_n - 1;
    if (g_launcher_hover < 0) g_launcher_hover = (g_launch_filt_n > 0) ? 0 : -1;
    int rv = (int)launcher_rows_visible();
    if (g_launcher_hover >= 0) {
        if (g_launcher_hover < g_launcher_scroll) g_launcher_scroll = g_launcher_hover;
        if (g_launcher_hover >= g_launcher_scroll + rv) g_launcher_scroll = g_launcher_hover - rv + 1;
    }
    if (g_launcher_scroll > g_launch_filt_n - rv) g_launcher_scroll = g_launch_filt_n - rv;
    if (g_launcher_scroll < 0) g_launcher_scroll = 0;
}

/* ── Geometry ──────────────────────────────────────────────────────────── */
uint64_t launcher_row_h(void)  { return console_font_height() + 8u; }
uint64_t launcher_item_h(void) { return launcher_row_h(); }  /* compat */
static uint64_t launcher_search_h(void) { return console_font_height() + 14u; }
static uint64_t launcher_header_h(void) { return launcher_search_h() + 16u; }

uint64_t launcher_panel_w(void) {
    uint64_t fw = console_font_width();
    uint64_t maxl = 12;
    for (int i = 0; i < g_launch_n; i++) {
        uint64_t l = (uint64_t)gui_strlen(g_launch[i].label);
        if (l > maxl) maxl = l;
    }
    uint64_t w = maxl * fw + 72u;
    if (w < 300u) w = 300u;
    uint64_t scr = console_fb_width();
    if (w > scr * 2u / 3u) w = scr * 2u / 3u;
    return w;
}
uint64_t launcher_eff_w(void) { return launcher_panel_w(); }  /* compat */

uint64_t launcher_rows_visible(void) {
    uint64_t rh  = launcher_row_h();
    uint64_t top = STATUS_H + 8u;
    uint64_t bot = console_fb_height() - TASKBAR_H;
    uint64_t avail = (bot > top) ? bot - top : rh;
    uint64_t body  = (avail > launcher_header_h() + 12u) ? avail - launcher_header_h() - 12u : rh;
    uint64_t rows = body / rh;
    if (rows < 4u)  rows = 4u;
    if (rows > 16u) rows = 16u;
    return rows;
}
uint64_t launcher_panel_h(void) {
    return launcher_header_h() + launcher_rows_visible() * launcher_row_h() + 8u;
}
/* The launcher opens adjacent to the panel's logo, toward the desktop. */
uint64_t launcher_lx(void) {
    if (g_theme.panel_edge == PANEL_RIGHT) {
        uint64_t w = launcher_panel_w(), r = desk_right();
        return r > w ? r - w : 0u;                 /* left of the right dock */
    }
    if (g_theme.panel_edge == PANEL_LEFT) return desk_left();  /* right of the left dock */
    return desk_left() + LOGO_X;                    /* bottom/top: near the logo */
}
uint64_t launcher_ly(void) {
    uint64_t h = launcher_panel_h();
    if (g_theme.panel_edge == PANEL_TOP ||
        g_theme.panel_edge == PANEL_LEFT ||
        g_theme.panel_edge == PANEL_RIGHT) return desk_top();  /* below top panel / by the dock */
    uint64_t b = desk_bot();                        /* BOTTOM: above the panel */
    return b > h ? b - h : 0u;
}
uint64_t launcher_body_y(void) { return launcher_ly() + launcher_header_h(); }

/* ── Hit testing ───────────────────────────────────────────────────────── */
bool launcher_in_search(int32_t mx, int32_t my) {
    uint64_t lx = launcher_lx(), ly = launcher_ly(), w = launcher_panel_w();
    uint64_t sy = ly + 8u, sh = launcher_search_h();
    return ((uint64_t)mx >= lx + 10u && (uint64_t)mx < lx + w - 10u &&
            (uint64_t)my >= sy && (uint64_t)my < sy + sh);
}
int launcher_hit_row(int32_t mx, int32_t my) {
    uint64_t lx = launcher_lx(), w = launcher_panel_w();
    uint64_t by = launcher_body_y(), rh = launcher_row_h(), rv = launcher_rows_visible();
    if ((uint64_t)mx < lx || (uint64_t)mx >= lx + w) return -1;
    if ((uint64_t)my < by || (uint64_t)my >= by + rv * rh) return -1;
    int idx = g_launcher_scroll + (int)(((uint64_t)my - by) / rh);
    if (idx < 0 || idx >= g_launch_filt_n) return -1;
    return idx;
}

/* ── Launch / pin ──────────────────────────────────────────────────────── */
void launcher_do_launch(int filt_row) {
    if (filt_row < 0 || filt_row >= g_launch_filt_n) return;
    launch_entry_t *e = &g_launch[g_launch_filt[filt_row]];
    if (e->builtin >= 0) {
        int s = e->builtin;
        raise_win(s);
        if (g_wins[s].state == WIN_HIDDEN) win_show(&g_wins[s], s); else full_redraw();
        return;
    }
    if (e->power) {
        __attribute__((weak)) void gui_exec_silent(const char *p, const char *a1, const char *a2);
        if (gui_exec_silent) {
            if      (e->power == 1) gui_exec_silent("/bin/sh", "-c", "echo mem > /sys/power/state");
            else if (e->power == 2) gui_exec_silent("/bin/sh", "-c", "reboot");
            else                    gui_exec_silent("/bin/sh", "-c", "poweroff");
        }
        return;
    }
    __attribute__((weak)) void gui_spawn_app(const char *path);
    if (gui_spawn_app && e->exec[0]) gui_spawn_app(e->exec);
    full_redraw();
}

void launcher_add_desktop(int filt_row) {
    if (filt_row < 0 || filt_row >= g_launch_filt_n) return;
    launch_entry_t *e = &g_launch[g_launch_filt[filt_row]];
    const char *path = e->exec[0] ? e->exec : launch_builtin_path(e->builtin);
    if (!path) return;  /* power actions can't be pinned */
    gui_add_desktop_icon(path, e->label);
    gui_desktop_save();
    gui_toast("Added to Desktop", 0x0060a0e0u);
}

/* Right-clicking a launcher item pins it to the taskbar favorites strip. */
void launcher_pin_taskbar(int filt_row) {
    if (filt_row < 0 || filt_row >= g_launch_filt_n) return;
    launch_entry_t *e = &g_launch[g_launch_filt[filt_row]];
    const char *path = e->exec[0] ? e->exec : launch_builtin_path(e->builtin);
    if (!path) { gui_toast("Can't pin that", 0x00e08060u); return; }
    if (gui_fav_add(path, e->label)) {
        gui_fav_save();
        gui_toast("Pinned to Taskbar", 0x0060a0e0u);
    } else {
        gui_toast("Already pinned", 0x00708090u);
    }
}

/* ── Launcher item context menu (right-click: pin / add to desktop) ────── */
int g_launchctx_row   = -1;   /* filtered row the menu targets; -1 = closed */
int g_launchctx_hover = -1;
static int32_t g_lcx = 0, g_lcy = 0;

static uint64_t launchctx_w(void)      { return 16u * console_font_width() + 24u; }
static uint64_t launchctx_item_h(void) { return console_font_height() + 12u; }

void launchctx_open(int filt_row, int32_t mx, int32_t my) {
    g_launchctx_row = filt_row;
    g_launchctx_hover = -1;
    uint64_t w = launchctx_w(), h = 2u * launchctx_item_h() + 8u;
    uint64_t fbw = console_fb_width(), fbh = console_fb_height();
    g_lcx = mx; g_lcy = my;
    if ((uint64_t)g_lcx + w >= fbw) g_lcx = (int32_t)(fbw - w - 2u);
    if ((uint64_t)g_lcy + h >= fbh) g_lcy = (int32_t)(fbh - h - 2u);
}

/* -1 = outside, 0 = Pin to Taskbar, 1 = Add to Desktop */
int launchctx_hit(int32_t mx, int32_t my) {
    if (g_launchctx_row < 0) return -1;
    uint64_t w = launchctx_w(), ih = launchctx_item_h();
    if ((uint64_t)mx < (uint64_t)g_lcx || (uint64_t)mx >= (uint64_t)g_lcx + w) return -1;
    if ((uint64_t)my < (uint64_t)g_lcy + 4u ||
        (uint64_t)my >= (uint64_t)g_lcy + 4u + 2u * ih) return -1;
    return (int)(((uint64_t)my - (uint64_t)g_lcy - 4u) / ih);
}

void launchctx_draw(void) {
    if (g_launchctx_row < 0) return;
    uint64_t fh = console_font_height();
    uint64_t w = launchctx_w(), ih = launchctx_item_h(), h = 2u * ih + 8u;
    uint64_t x = (uint64_t)g_lcx, y = (uint64_t)g_lcy;
    console_fill_vgrad(x, y, w, h, 0x001a2338u, 0x000e1220u);
    console_fill_rect(x, y, w, 1u, 0x003a5688u);
    console_fill_rect(x, y + h - 1u, w, 1u, 0x00223048u);
    console_fill_rect(x, y, 1u, h, 0x00223048u);
    console_fill_rect(x + w - 1u, y, 1u, h, 0x00223048u);
    static const char *items[2] = { "Pin to Taskbar", "Add to Desktop" };
    for (int i = 0; i < 2; i++) {
        uint64_t iy = y + 4u + (uint64_t)i * ih;
        if (i == g_launchctx_hover)
            console_fill_rect(x + 4u, iy, w - 8u, ih, 0x002a4a80u);
        gui_draw_str_fg(x + 12u, iy + (ih - fh) / 2u, items[i], 0x00dce8f8u);
    }
}

/* ── Draw ──────────────────────────────────────────────────────────────── */
void launcher_draw(void) {
    uint64_t lx = launcher_lx(), ly = launcher_ly();
    uint64_t fw = console_font_width(), fh = console_font_height();
    uint64_t w  = launcher_panel_w(), ph = launcher_panel_h();
    uint64_t rh = launcher_row_h(), rv = launcher_rows_visible();
    uint64_t sh = launcher_search_h();
    uint64_t by = launcher_body_y();

    /* Panel + outline. Frosted glass: the launcher isn't redrawn over a fresh
     * full_redraw every frame (only on open/hover/search), so a naive blend
     * would accumulate alpha. Instead capture the CLEAN desktop behind the panel
     * once per open (on the first draw, before anything is painted here), then
     * restore that backdrop and blend a translucent panel over it each draw. */
    if (g_theme.fx_glass) {
        if (!g_launcher_bg || g_lbg_w != w || g_lbg_h != ph ||
            g_lbg_x != lx || g_lbg_y != ly) {
            if (g_launcher_bg) { kfree(g_launcher_bg); g_launcher_bg = 0; }
            g_launcher_bg = kmalloc(w * ph * 4u);
            if (g_launcher_bg) {
                console_capture_rect(g_launcher_bg, lx, ly, w, ph);
                g_lbg_x = lx; g_lbg_y = ly; g_lbg_w = w; g_lbg_h = ph;
            }
        }
        if (g_launcher_bg) console_paste_rect(g_launcher_bg, lx, ly, w, ph);
        console_blend_rect(lx, ly, w, ph, 0x0016223au, 206u);   /* frosted panel */
        console_blend_rect(lx, ly + 1u, w, 1u, 0x00ffffffu, 22u); /* top sheen */
    } else {
        console_fill_vgrad(lx, ly, w, ph, 0x00161d30u, 0x000d111du);
    }
    console_fill_rect(lx, ly, w, 1u, 0x003a5688u);
    console_fill_rect(lx, ly + ph - 1u, w, 1u, 0x00223048u);
    console_fill_rect(lx, ly, 1u, ph, 0x00223048u);
    console_fill_rect(lx + w - 1u, ly, 1u, ph, 0x00223048u);

    /* Search box */
    uint64_t sx = lx + 10u, sy = ly + 8u, sw = w - 20u;
    console_fill_rect(sx, sy, sw, sh, 0x000c1220u);
    console_fill_rect(sx, sy, sw, 1u, 0x00304a70u);
    console_fill_rect(sx, sy + sh - 1u, sw, 1u, 0x00223048u);
    console_fill_rect(sx, sy, 1u, sh, 0x00223048u);
    console_fill_rect(sx + sw - 1u, sy, 1u, sh, 0x00223048u);
    /* magnifier glyph */
    uint64_t gx = sx + 8u, gy = sy + sh / 2u;
    console_fill_rect(gx,      gy - 3u, 6u, 1u, 0x005f7fb0u);
    console_fill_rect(gx,      gy + 2u, 6u, 1u, 0x005f7fb0u);
    console_fill_rect(gx - 1u, gy - 2u, 1u, 4u, 0x005f7fb0u);
    console_fill_rect(gx + 6u, gy - 2u, 1u, 4u, 0x005f7fb0u);
    console_fill_rect(gx + 6u, gy + 3u, 3u, 1u, 0x005f7fb0u);
    uint64_t tx = sx + 22u, tyy = sy + (sh > fh ? (sh - fh) / 2u : 0u);
    if (g_launch_qlen == 0) {
        gui_draw_str_fg(tx, tyy, "Search apps...", 0x00566276u);
    } else {
        gui_draw_str_fg(tx, tyy, g_launch_q, 0x00e6ecf7u);
        uint64_t cxp = tx + (uint64_t)g_launch_qlen * fw + 1u;
        if ((g_gui_tick / 8u) % 2u == 0) console_fill_rect(cxp, tyy, 2u, fh, 0x0080b4ffu);
    }

    /* Body rows */
    if (g_launch_filt_n == 0) {
        const char *nr = "No matching apps";
        uint64_t nl = (uint64_t)gui_strlen(nr) * fw;
        uint64_t nx = lx + (w > nl ? (w - nl) / 2u : 0u);
        gui_draw_str_fg(nx, by + rh, nr, 0x00566276u);
    }
    for (uint64_t r = 0; r < rv; r++) {
        int idx = g_launcher_scroll + (int)r;
        if (idx < 0 || idx >= g_launch_filt_n) break;
        launch_entry_t *e = &g_launch[g_launch_filt[idx]];
        uint64_t ry = by + r * rh;
        bool sel = (idx == g_launcher_hover);
        if (sel)
            console_fill_vgrad(lx + 3u, ry + 1u, w - 6u, rh - 2u, 0x003a6cc8u, 0x002a4f9cu);
        /* Icon column: real app logo when present, colored dot otherwise. */
        uint64_t iconsz = (rh > 20u) ? rh - 10u : 12u;
        uint64_t icx = lx + 12u, icy = ry + (rh > iconsz ? (rh - iconsz) / 2u : 0u);
        bool drew_icon = false;
#ifdef __linux__
        {
            licon_t *ic = launch_icon(g_launch_filt[idx]);
            if (ic) {
                console_blit_scaled_alpha(ic->img, ic->w, ic->h, icx, icy, iconsz, iconsz);
                drew_icon = true;
            }
        }
#endif
        if (!drew_icon) {
            uint32_t dot = e->power     ? 0x00e05050u :
                           e->builtin >= 0 ? 0x0060c8a0u : g_theme.accent;
            uint64_t dcx = icx + iconsz / 2u - 3u, dcy = ry + rh / 2u - 3u;
            console_fill_rect(dcx,      dcy + 1u, 6u, 4u, dot);
            console_fill_rect(dcx + 1u, dcy,      4u, 6u, dot);
        }
        uint32_t fg = sel ? 0x00f2f7ffu : (e->power ? 0x00e88a80u : COL_LAUNCH_FG);
        uint64_t spx = icx + iconsz + 8u, spy = ry + (rh > fh ? (rh - fh) / 2u : 0u);
        uint64_t lead = spx - lx + 14u;
        uint64_t maxc = (w > lead && fw) ? (w - lead) / fw : 8u;
        gui_draw_str_clip_fg(spx, spy, e->label, fg, maxc);
    }

    /* Scrollbar */
    if (g_launch_filt_n > (int)rv) {
        uint64_t trh = rv * rh;
        uint64_t th  = trh * rv / (uint64_t)g_launch_filt_n; if (th < 14u) th = 14u;
        uint64_t maxs = (uint64_t)g_launch_filt_n - rv;
        uint64_t ty2 = by + (maxs ? (uint64_t)g_launcher_scroll * (trh - th) / maxs : 0u);
        console_fill_rect(lx + w - 5u, by,  2u, trh, 0x0018202eu);
        console_fill_rect(lx + w - 5u, ty2, 2u, th,  0x003a5c90u);
    }

    /* Item context menu on top of everything */
    launchctx_draw();
}

/* ── Context menu ────────────────────────────────────────────────────── */

void ctx_draw(void) {
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();
    uint64_t cw = ctx_w();
    /* 0-3: built-in windows; 4: sep; 5-9: IPC apps; 10: sep; 11-12: desktop actions */
    static const char *ctx_items[CTX_ITEMS] = {
        "Terminal", "Files", "Settings", "Viewer",
        NULL,               /* separator */
        "File Browser", "Sys Monitor", "Net Monitor", "New Term", "Editor",
        NULL,               /* separator */
        "Lock Screen", "Show Desktop",
    };
    int32_t cx = g_ctx_x;
    int32_t cy = g_ctx_y;

    /* Dynamic total height: sum items (CTX_ITEM_H each) + separators (8px each) + 2 border */
    uint64_t total_h = 2u;
    for (int _i = 0; _i < (int)CTX_ITEMS; _i++)
        total_h += ctx_items[_i] ? CTX_ITEM_H : 8u;
    console_fill_vgrad((uint64_t)cx, (uint64_t)cy, cw, total_h, 0x00161d30u, 0x000d111du);
    console_fill_rect((uint64_t)cx, (uint64_t)cy, cw, 1u, 0x003a5688u);
    console_fill_rect((uint64_t)cx, (uint64_t)cy + total_h - 1u, cw, 1u, 0x00223048u);
    console_fill_rect((uint64_t)cx, (uint64_t)cy, 1u, total_h, 0x00223048u);
    console_fill_rect((uint64_t)cx + cw - 1u, (uint64_t)cy, 1u, total_h, 0x00223048u);

    uint64_t ry = (uint64_t)cy + 1u;
    for (int i = 0; i < (int)CTX_ITEMS; i++) {
        if (ctx_items[i] == NULL) {
            console_fill_rect((uint64_t)cx + 8u, ry + 3u, cw - 16u, 1u, 0x00263248u);
            ry += 8u;
            continue;
        }
        bool hov = (g_ctx_hover == i);
        if (hov)
            console_fill_vgrad((uint64_t)cx + 3u, ry + 1u, cw - 6u, CTX_ITEM_H - 2u,
                               0x003a6cc8u, 0x002a4f9cu);
        uint64_t spx  = (uint64_t)cx + 14u;
        uint64_t spy  = ry + (CTX_ITEM_H > fh ? (CTX_ITEM_H - fh) / 2u : 0u);
        gui_draw_str_fg(spx, spy, ctx_items[i], hov ? 0x00f2f7ffu : COL_LAUNCH_FG);
        ry += CTX_ITEM_H;
    }
    (void)fw;
}

/* ── Text editor context menu draw ──────────────────────────────────── */
void txt_ctx_draw(void) {
    if (!g_txt_ctx_open) return;
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();
    uint64_t tcw = txt_ctx_w();
    static const char *txt_ctx_items[] = { "Select All", "Copy", "Cut", "Paste", "Find..." };
    int32_t cx = g_txt_ctx_x;
    int32_t cy = g_txt_ctx_y;
    console_fill_rect((uint64_t)cx, (uint64_t)cy, tcw, TXT_CTX_ITEMS * CTX_ITEM_H + 2u, COL_LAUNCH_BG);
    console_fill_rect((uint64_t)cx, (uint64_t)cy, tcw, 1u, COL_LAUNCH_HL);
    console_fill_rect((uint64_t)cx, (uint64_t)cy + TXT_CTX_ITEMS * CTX_ITEM_H + 1u, tcw, 1u, COL_LAUNCH_HL);
    console_fill_rect((uint64_t)cx, (uint64_t)cy, 1u, TXT_CTX_ITEMS * CTX_ITEM_H + 2u, COL_LAUNCH_HL);
    console_fill_rect((uint64_t)cx + tcw - 1u, (uint64_t)cy, 1u, TXT_CTX_ITEMS * CTX_ITEM_H + 2u, COL_LAUNCH_HL);
    for (int i = 0; i < TXT_CTX_ITEMS; i++) {
        uint64_t ry = (uint64_t)cy + 1u + (uint64_t)i * CTX_ITEM_H;
        bool hov    = (g_txt_ctx_hover == i);
        uint32_t bg = hov ? COL_LAUNCH_HL : COL_LAUNCH_BG;
        console_fill_rect((uint64_t)cx + 1u, ry, tcw - 2u, CTX_ITEM_H, bg);
        uint64_t slen = (uint64_t)gui_strlen(txt_ctx_items[i]);
        uint64_t spx  = (uint64_t)cx + (tcw > slen * fw ? (tcw - slen * fw) / 2u : 0u);
        uint64_t spy  = ry + (CTX_ITEM_H > fh ? (CTX_ITEM_H - fh) / 2u : 0u);
        gui_draw_str(spx, spy, txt_ctx_items[i], COL_LAUNCH_FG, bg);
    }
}

/* ── File browser context menu draw ─────────────────────────────────── */

static const char *fb_ctx_labels[] = {
    "Open", "Edit", "Rename", "Delete", "New File", "New Folder", "Refresh",
    "Copy", "Cut", "Paste", "Copy Path", "Add to Desktop"
};

void fb_ctx_draw(void) {
    if (!g_fb_ctx_open || g_fb_ctx_n <= 0) return;
    uint64_t fh  = console_font_height();
    uint64_t fcw = fb_ctx_w();
    uint64_t cx = (uint64_t)g_fb_ctx_x;
    uint64_t cy = (uint64_t)g_fb_ctx_y;
    int n = g_fb_ctx_n;

    console_fill_rect(cx, cy, fcw, (uint64_t)n * CTX_ITEM_H + 2u, COL_LAUNCH_BG);
    console_fill_rect(cx, cy, fcw, 1u, COL_LAUNCH_HL);
    console_fill_rect(cx, cy + (uint64_t)n * CTX_ITEM_H + 1u, fcw, 1u, COL_LAUNCH_HL);
    console_fill_rect(cx, cy, 1u, (uint64_t)n * CTX_ITEM_H + 2u, COL_LAUNCH_HL);
    console_fill_rect(cx + fcw - 1u, cy, 1u, (uint64_t)n * CTX_ITEM_H + 2u, COL_LAUNCH_HL);

    for (int i = 0; i < n; i++) {
        int act   = g_fb_ctx_acts[i];
        uint64_t ry  = cy + 1u + (uint64_t)i * CTX_ITEM_H;
        bool hov     = (g_fb_ctx_hover == i);
        uint32_t bg  = hov ? COL_LAUNCH_HL : COL_LAUNCH_BG;
        uint32_t fg  = (act == FB_CTX_ACT_DELETE)    ? 0x00e87060u :
                       (act == FB_CTX_ACT_CUT)       ? 0x00e8b060u :
                       (act == FB_CTX_ACT_PASTE)     ? 0x0080c8e8u :
                       (act == FB_CTX_ACT_EDIT)      ? 0x0080d8a0u :
                       (act == FB_CTX_ACT_COPY_PATH) ? 0x00a0b8d0u :
                       (act == FB_CTX_ACT_ADD_DESK)  ? 0x0060c880u : COL_LAUNCH_FG;
        console_fill_rect(cx + 1u, ry, fcw - 2u, CTX_ITEM_H, bg);
        const char *lbl  = fb_ctx_labels[act];
        uint64_t spx     = cx + 10u;
        uint64_t spy     = ry + (CTX_ITEM_H > fh ? (CTX_ITEM_H - fh) / 2u : 0u);
        gui_draw_str(spx, spy, lbl, fg, bg);
    }
}

/* Open the FB context menu at (x, y) for the given window/row */
void fb_ctx_open_at(int win_slot, int row, bool is_dir, int32_t x, int32_t y) {
    g_fb_ctx_win    = win_slot;
    g_fb_ctx_row    = row;
    g_fb_ctx_is_dir = is_dir;
    g_fb_ctx_hover  = -1;
    g_fb_ctx_n      = 0;

    if (row >= 0) {
        /* File or directory row */
        g_fb_ctx_acts[g_fb_ctx_n++] = FB_CTX_ACT_OPEN;
        if (!is_dir)
            g_fb_ctx_acts[g_fb_ctx_n++] = FB_CTX_ACT_EDIT;
        g_fb_ctx_acts[g_fb_ctx_n++] = FB_CTX_ACT_RENAME;
        if (!is_dir) {
            g_fb_ctx_acts[g_fb_ctx_n++] = FB_CTX_ACT_COPY;
            g_fb_ctx_acts[g_fb_ctx_n++] = FB_CTX_ACT_CUT;
            g_fb_ctx_acts[g_fb_ctx_n++] = FB_CTX_ACT_DELETE;
        }
        g_fb_ctx_acts[g_fb_ctx_n++] = FB_CTX_ACT_COPY_PATH;
        if (!is_dir)
            g_fb_ctx_acts[g_fb_ctx_n++] = FB_CTX_ACT_ADD_DESK;
        if (g_fb_clip_path[0])
            g_fb_ctx_acts[g_fb_ctx_n++] = FB_CTX_ACT_PASTE;
        g_fb_ctx_acts[g_fb_ctx_n++] = FB_CTX_ACT_NEW_FILE;
        g_fb_ctx_acts[g_fb_ctx_n++] = FB_CTX_ACT_NEW_DIR;
    } else {
        /* Empty area */
        if (g_fb_clip_path[0])
            g_fb_ctx_acts[g_fb_ctx_n++] = FB_CTX_ACT_PASTE;
        g_fb_ctx_acts[g_fb_ctx_n++] = FB_CTX_ACT_NEW_FILE;
        g_fb_ctx_acts[g_fb_ctx_n++] = FB_CTX_ACT_NEW_DIR;
        g_fb_ctx_acts[g_fb_ctx_n++] = FB_CTX_ACT_REFRESH;
    }

    /* Clamp to screen */
    uint64_t fb_w2 = console_fb_width();
    uint64_t ty2   = console_fb_height() - TASKBAR_H;
    uint64_t menu_h = (uint64_t)g_fb_ctx_n * CTX_ITEM_H + 2u;
    if ((uint64_t)x + fb_ctx_w() > fb_w2) x = (int32_t)(fb_w2 - fb_ctx_w());
    if ((uint64_t)y + menu_h > ty2)      y = (int32_t)(ty2 - menu_h);
    if (x < 0) x = 0;
    if (y < (int32_t)STATUS_H) y = (int32_t)STATUS_H;

    g_fb_ctx_x    = x;
    g_fb_ctx_y    = y;
    g_fb_ctx_open = true;
    fb_ctx_draw();
}

/* Execute file browser context menu action */
void fb_ctx_run(int item) {
    if (item < 0 || item >= g_fb_ctx_n || g_fb_ctx_win < 0 || g_fb_ctx_win >= MAX_WINS) return;
    int act = g_fb_ctx_acts[item];
    window_t *fw2 = &g_wins[g_fb_ctx_win];
    int row2 = g_fb_ctx_row;
    switch (act) {
    case FB_CTX_ACT_OPEN:
        if (row2 >= 0 && row2 < fw2->fb.entry_count) {
            if (fw2->fb.is_dir[row2]) {
                char np2[256]; fb_path_join(np2, fw2->fb.path, fw2->fb.entries[row2]);
                fb_navigate(&fw2->fb, np2); fb_render(fw2);
            } else {
                char fp2[256]; fb_path_join(fp2, fw2->fb.path, fw2->fb.entries[row2]);
                text_open(&g_wins[3], fp2); win_show(&g_wins[3], 3);
            }
        }
        break;
    case FB_CTX_ACT_EDIT:
        if (row2 >= 0 && row2 < fw2->fb.entry_count && !fw2->fb.is_dir[row2]) {
            char ep2[256]; fb_path_join(ep2, fw2->fb.path, fw2->fb.entries[row2]);
            text_open(&g_wins[3], ep2); win_show(&g_wins[3], 3);
            text_enter_edit(&g_wins[3]);
            if (g_wins[3].text.edit_mode) gui_toast("Edit mode", 0x0080c8a0u);
            text_render(&g_wins[3]);
        }
        break;
    case FB_CTX_ACT_RENAME:
        if (row2 >= 0 && row2 < fw2->fb.entry_count) {
            const char *ename2 = fw2->fb.entries[row2];
            fw2->fb.input_active = true; fw2->fb.input_is_rename = true; fw2->fb.input_isdir = false;
            int elen2 = 0;
            while (ename2[elen2] && elen2 < 127) { fw2->fb.input_buf[elen2] = ename2[elen2]; elen2++; }
            fw2->fb.input_buf[elen2] = '\0'; fw2->fb.input_len = elen2; fw2->fb.input_cursor = elen2;
            for (int _k2 = 0; _k2 <= elen2; _k2++) fw2->fb.input_orig[_k2] = fw2->fb.input_buf[_k2];
            fb_render(fw2);
        }
        break;
    case FB_CTX_ACT_DELETE:
        if (row2 >= 0 && row2 < fw2->fb.entry_count && !fw2->fb.is_dir[row2]) {
            char dp2[256]; fb_path_join(dp2, fw2->fb.path, fw2->fb.entries[row2]);
            vfs_delete(dp2); gui_toast("File deleted", 0x00e88060u);
            fb_navigate(&fw2->fb, fw2->fb.path);
            if (fw2->fb.sel_row >= fw2->fb.entry_count) fw2->fb.sel_row = fw2->fb.entry_count - 1;
            full_redraw();
        }
        break;
    case FB_CTX_ACT_NEW_FILE:
        fw2->fb.input_active = true; fw2->fb.input_isdir = false; fw2->fb.input_is_rename = false;
        fw2->fb.input_len = 0; fw2->fb.input_cursor = 0; fw2->fb.input_buf[0] = '\0';
        fb_render(fw2); break;
    case FB_CTX_ACT_NEW_DIR:
        fw2->fb.input_active = true; fw2->fb.input_isdir = true; fw2->fb.input_is_rename = false;
        fw2->fb.input_len = 0; fw2->fb.input_cursor = 0; fw2->fb.input_buf[0] = '\0';
        fb_render(fw2); break;
    case FB_CTX_ACT_REFRESH:
        fb_navigate(&fw2->fb, fw2->fb.path); fb_render(fw2); break;
    case FB_CTX_ACT_COPY:
    case FB_CTX_ACT_CUT:
        if (row2 >= 0 && row2 < fw2->fb.entry_count && !fw2->fb.is_dir[row2]) {
            fb_path_join(g_fb_clip_path, fw2->fb.path, fw2->fb.entries[row2]);
            g_fb_clip_is_cut = (act == FB_CTX_ACT_CUT);
            gui_toast(g_fb_clip_is_cut ? "Marked for move" : "File copied", 0x0080c8a0u);
            full_redraw();
        }
        break;
    case FB_CTX_ACT_COPY_PATH:
        if (row2 >= 0 && row2 < fw2->fb.entry_count) {
            char _cpath[256]; fb_path_join(_cpath, fw2->fb.path, fw2->fb.entries[row2]);
            edit_set_clipboard((const uint8_t *)_cpath, (uint32_t)gui_strlen(_cpath));
            gui_toast("Path copied", 0x0080c8a0u); full_redraw();
        }
        break;
    case FB_CTX_ACT_PASTE:
        if (g_fb_clip_path[0]) {
            const char *_fn3 = g_fb_clip_path;
            for (const char *_p3 = g_fb_clip_path; *_p3; _p3++) if (*_p3 == '/') _fn3 = _p3 + 1;
            char _dst3[256]; fb_path_join(_dst3, fw2->fb.path, _fn3);
            if (gui_streq(g_fb_clip_path, _dst3)) {
                gui_toast("Already here", 0x00708090u);
            } else {
                const void *_d3 = NULL; uint64_t _sz3 = 0;
                int _rv3 = vfs_read(g_fb_clip_path, &_d3, &_sz3);
                if (_rv3 == 0 && _d3) {
                    uint8_t *_b3 = (uint8_t *)kmalloc(_sz3 + 1u);
                    if (_b3) {
                        for (uint64_t _i3 = 0; _i3 < _sz3; _i3++) _b3[_i3] = ((uint8_t *)_d3)[_i3];
                        vfs_write(_dst3, _b3, _sz3); kfree(_b3);
                        if (g_fb_clip_is_cut) {
                            vfs_delete(g_fb_clip_path); g_fb_clip_path[0] = '\0';
                            gui_toast("Moved", 0x0080e8b0u);
                        } else { gui_toast("Pasted", 0x0080e8b0u); }
                    } else { gui_toast("Out of memory", 0x00e08060u); }
                } else { gui_toast("Read failed", 0x00e08060u); g_fb_clip_path[0] = '\0'; }
                fb_navigate(&fw2->fb, fw2->fb.path); full_redraw();
            }
        }
        break;
    case FB_CTX_ACT_ADD_DESK:
        if (row2 >= 0 && row2 < fw2->fb.entry_count && !fw2->fb.is_dir[row2]) {
            char _dpath[256];
            fb_path_join(_dpath, fw2->fb.path, fw2->fb.entries[row2]);
            gui_add_desktop_icon(_dpath, NULL);
            gui_toast("Added to Desktop", 0x0060c880u);
        }
        break;
    default: break;
    }
}
