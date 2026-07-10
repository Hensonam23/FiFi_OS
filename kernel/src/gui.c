#include "gui_internal.h"

/* Forward declarations for public API functions used inside gui_on_tick */
void gui_show_desktop(void);
void gui_snap_focused(int zone);
void gui_term_scroll_page(int dir);

/* ── Constants used in gui_on_tick that are local to their modules ───── */
/* Must match the definitions in gui_text.c and gui_taskbar.c */
#define TV_MINIMAP_W   50u   /* minimap panel width in pixels (gui_text.c) */
#define VOL_POP_W     170u   /* volume popup width in pixels (gui_taskbar.c) */

/* ── Global variable definitions ─────────────────────────────────────── */

gui_theme_t g_theme = {
    0x003060c0u, WALLPAPER_GRADIENT, true, true, true, true, 0,
    /* panel: bottom edge, left-aligned, always visible, base thickness */
    PANEL_BOTTOM, PALIGN_START, false, 0,
    /* effects: frosted glass panels ON, window shadows ON, 5px corners */
    true, true, 5,
};

uint32_t *g_wall_img   = NULL;
uint32_t  g_wall_img_w = 0;
uint32_t  g_wall_img_h = 0;

desk_icon_t g_desk_icons[DESK_ICON_MAX];
int         g_desk_icon_hover = -1;
int         g_desk_icon_sel   = -1;
int         g_desk_icon_dbl   = -1;
uint64_t    g_desk_icon_click_t = 0;

/* weak: real impl in platform.c for linux-desktop */
__attribute__((weak)) bool platform_load_image(const char *path __attribute__((unused)),
    uint32_t **px __attribute__((unused)),
    uint32_t *w  __attribute__((unused)),
    uint32_t *h  __attribute__((unused))) { return false; }

const uint32_t g_accent_presets[ACCENT_PRESET_COUNT] = {
    0x003060c0u,   /* FiFi Blue (default)  */
    0x00307830u,   /* Forest Green         */
    0x00802060u,   /* Violet               */
    0x00b04010u,   /* Rust Orange          */
    0x00408080u,   /* Teal                 */
    0x00606020u,   /* Olive                */
    0x00204060u,   /* Navy                 */
    0x00803030u,   /* Crimson              */
    0x00906010u,   /* Gold                 */
    0x00208060u,   /* Emerald              */
    0x00601880u,   /* Purple               */
    0x00107888u,   /* Cyan                 */
    0x008040a0u,   /* Mauve                */
    0x00505050u,   /* Graphite             */
    0x00285870u,   /* Steel Blue           */
    0x006a1a1au,   /* Dark Red             */
};

const int g_anim_open_scale[ANIM_TICKS]  = { 36, 64, 84, 96, 100 };
const int g_anim_close_scale[ANIM_TICKS] = { 64, 36, 16,  4,   0 };

window_t g_wins[MAX_WINS];
int      g_z[MAX_WINS];

bool      g_dragging    = false;
int       g_drag_win    = -1;
bool      g_text_drag_sel = false;
int       g_text_drag_win = -1;
uint64_t  g_text_drag_scroll_tick = 0;
int32_t   g_drag_off_x  = 0;
int32_t   g_drag_off_y  = 0;
uint32_t *g_drag_shadow = NULL;
uint64_t  g_drag_shad_w = 0;
uint64_t  g_drag_shad_h = 0;
uint8_t  *g_clipboard     = NULL;
uint32_t  g_clipboard_len = 0;
bool      g_prev_lbtn   = false;
int       g_snap_preview = 0;

bool     g_sb_drag      = false;
int      g_sb_drag_win  = -1;
int32_t  g_sb_drag_y0   = 0;
int32_t  g_sb_drag_x0   = 0;
int      g_sb_drag_s0   = 0;
uint64_t g_sb_drag_range = 0;
int      g_sb_drag_max  = 0;
bool     g_sb_drag_text = false;
bool     g_sb_drag_horiz = false;

bool     g_term_sb_drag   = false;
int32_t  g_term_sb_drag_y0 = 0;
int      g_term_sb_drag_s0 = 0;
uint64_t g_term_sb_drag_range = 0;
int      g_term_sb_drag_max   = 0;

bool         g_resizing   = false;
int          g_resize_win = -1;
resize_dir_t g_resize_dir = RES_NONE;
int32_t      g_resize_ox  = 0;
int32_t      g_resize_oy  = 0;
uint64_t     g_resize_wx0 = 0;
uint64_t     g_resize_wy0 = 0;
uint64_t     g_resize_ww0 = 0;
uint64_t     g_resize_wh0 = 0;

bool g_launcher_open = false;
bool g_help_open     = false;   /* Super+/ keyboard-shortcuts overlay */

int      g_settings_scroll    = 0;
uint32_t g_gui_raise_z        = 2;

int      g_settings_total_h   = 0;
bool     g_sb_drag_settings   = false;

int g_term_scroll = 0;

int g_font_idx = 0;
const char *g_font_paths[] = {
    "/fonts/ter16b.psf", "/fonts/ter20b.psf", "/fonts/ter24b.psf",
    "/fonts/ter28b.psf", "/fonts/ter32b.psf",
    "/fonts/default.psf", NULL
};
const char *g_font_labels[] = {
    "Terminus 8x16", "Terminus 10x20", "Terminus 12x24",
    "Terminus 14x28", "Terminus 16x32",
    "Default 8x16", NULL
};
uint64_t g_font_prev_bx = 0, g_font_next_bx = 0;
uint64_t g_font_btn_by  = 0, g_font_btn_bw  = 28u, g_font_btn_bh = 0u;
uint64_t g_utc_minus_bx = 0, g_utc_plus_bx = 0;
uint64_t g_utc_btn_by   = 0, g_utc_btn_bh  = 0u;
uint64_t g_vol_minus_bx = 0, g_vol_plus_bx  = 0;
uint64_t g_vol_btn_by   = 0, g_vol_btn_bh   = 0u;
uint64_t g_vol_chime_bx = 0, g_vol_chime_by = 0;
uint64_t g_vol_chime_bw = 0, g_vol_chime_bh = 0u;
uint64_t g_gaming_btn_bx = 0, g_gaming_btn_by = 0;
uint64_t g_gaming_btn_bw = 0, g_gaming_btn_bh = 0u;
uint64_t g_gaming_mode_bx = 0, g_gaming_mode_by = 0;
uint64_t g_gaming_mode_bw = 0, g_gaming_mode_bh = 0u;
uint64_t g_fw_btn_bx = 0, g_fw_btn_by = 0;
uint64_t g_fw_btn_bw = 0, g_fw_btn_bh = 0u;
int      g_fw_state  = -1;
uint64_t g_dns_btn_bx = 0, g_dns_btn_by = 0;
uint64_t g_dns_btn_bw = 0, g_dns_btn_bh = 0u;
int      g_dns_mode   = 0;
uint64_t g_vpn_btn_bx = 0, g_vpn_btn_by = 0;
uint64_t g_vpn_btn_bw = 0, g_vpn_btn_bh = 0u;
uint64_t g_vpn_auto_bx = 0, g_vpn_auto_by = 0;
uint64_t g_vpn_auto_bw = 0, g_vpn_auto_bh = 0u;
uint64_t g_lto_btn_bx = 0, g_lto_btn_by = 0;
uint64_t g_lto_btn_bw = 0, g_lto_btn_bh = 0u;
int      g_lto_idx    = 0;

bool     g_vol_popup_open  = false;
uint64_t g_vol_tray_x      = 0;
uint64_t g_vol_tray_w      = 0;
uint64_t g_vol_pop_x       = 0;
uint64_t g_vol_pop_y       = 0;
uint64_t g_vol_pop_h       = 0;
uint64_t g_vol_pop_minus_x = 0;
uint64_t g_vol_pop_plus_x  = 0;
uint64_t g_vol_pop_btn_y   = 0;
uint64_t g_vol_pop_btn_w   = 0;
uint64_t g_vol_pop_btn_h   = 0;
uint64_t g_vol_pop_slid_x  = 0;
uint64_t g_vol_pop_slid_w  = 0;

int g_chrome_win = -1;
int g_chrome_btn = 0;

uint64_t g_last_click_tick  = 0;
int      g_last_click_win   = -1;
int      g_last_click_count = 0;

bool    g_ctx_open = false;
int32_t g_ctx_x = 0;
int32_t g_ctx_y = 0;

int g_launcher_hover = -1;
int g_taskbtn_hover = -1;
int g_ctx_hover = -1;

bool    g_txt_ctx_open  = false;
int32_t g_txt_ctx_x    = 0;
int32_t g_txt_ctx_y    = 0;
int     g_txt_ctx_win  = -1;
int     g_txt_ctx_hover = -1;

bool    g_fb_ctx_open  = false;
int32_t g_fb_ctx_x  = 0;
int32_t g_fb_ctx_y  = 0;
int     g_fb_ctx_win   = -1;
int     g_fb_ctx_row   = -1;
bool    g_fb_ctx_is_dir = false;
int     g_fb_ctx_hover = -1;
int     g_fb_ctx_n     = 0;
int     g_fb_ctx_acts[FB_CTX_MAX_ITEMS];
char    g_fb_clip_path[256] = "";
bool    g_fb_clip_is_cut    = false;

int          g_resize_hover_win     = -1;
resize_dir_t g_resize_hover_dir     = RES_NONE;
int          g_resize_pending_win   = -1;
resize_dir_t g_resize_pending_dir   = RES_NONE;
int          g_resize_pending_ticks = 0;

uint64_t g_theme_accent_bx[ACCENT_PRESET_COUNT];
uint64_t g_theme_accent_by;
uint64_t g_theme_swatch_sz;
uint64_t g_theme_accent_by2;
uint64_t g_theme_wall_bx[WALLPAPER_COUNT];
uint64_t g_theme_wall_by_arr[WALLPAPER_COUNT];
uint64_t g_theme_wall_by;
uint64_t g_theme_wall_bw;
uint64_t g_theme_wall_bh;
uint64_t g_theme_toggle_x[THEME_TOGGLE_COUNT], g_theme_toggle_y[THEME_TOGGLE_COUNT];
uint64_t g_theme_panel_bx[PANEL_POS_COUNT], g_theme_panel_by[PANEL_POS_COUNT];
uint64_t g_theme_panel_bw, g_theme_panel_bh;
uint64_t g_theme_align_bx[PANEL_ALIGN_COUNT], g_theme_align_by[PANEL_ALIGN_COUNT];
uint64_t g_theme_align_bw, g_theme_align_bh;
bool g_panel_revealed = false;
uint64_t g_theme_toggle_w, g_theme_toggle_h;

uint64_t g_gui_tick = 0;

char     g_toast_msg[64];
uint32_t g_toast_color;
int      g_toast_ticks;

char g_recent[RECENT_MAX][128];
int  g_recent_count = 0;

/* ── Redraw source tag (for debug) ───────────────────────────────────── */
int g_redraw_src = 0;

/* ── Launcher items ──────────────────────────────────────────────────── */
const char * const g_launcher_items[LAUNCHER_ITEMS] = {
    "Terminal", "Files", "Settings", "Viewer",
    "File Browser", "Sys Info", "Gamepad", "Sys Monitor", "Net Monitor", "New Term", "Editor", "Calculator", "Image Viewer",
    "Security", "WiFi", "Browser", "Steam", "Proton Config", "App Store",
    "---",          /* separator -- not clickable */
    "Sleep",
    "Restart",
    "Shutdown",
};



/* Map a launcher item index to the standalone binary that implements it, or
 * NULL if the item can't become a shortcut (separator / Sleep / Restart /
 * Shutdown). Built-in windows (items 0-3) map to their standalone equivalents
 * so a desktop shortcut launches them the same way as any other app. */
const char *gui_launcher_app_path(int item) {
    switch (item) {
        case 0: return "/bin/fifi-terminal";     /* Terminal */
        case 1: return "/bin/fifi-filebrowser";  /* Files    */
        case 2: return "/bin/fifi-settings";     /* Settings */
        case 3: return "/bin/fifi-imageviewer";  /* Viewer   */
        default: break;
    }
    static const char *paths[] = {
        "/bin/fifi-filebrowser", "/bin/fifi-settings", "/bin/fifi-gamepad",
        "/bin/fifi-sysmon", "/bin/fifi-netmon", "/bin/fifi-terminal",
        "/bin/fifi-editor", "/bin/fifi-calc", "/bin/fifi-imageviewer",
        "/bin/fifi-security", "/bin/fifi-wifi", "/bin/fifi-browser",
        "/usr/bin/steam", "/bin/fifi-proton", "/fifi-data/apps/fifi-appstore",
    };
    int idx = item - 4;
    if (idx >= 0 && idx < (int)(sizeof(paths) / sizeof(paths[0]))) return paths[idx];
    return NULL;
}

/* ── Z-order public API ──────────────────────────────────────────────── */

uint32_t gui_next_z(void) { return g_gui_raise_z++; }

/* The Wayland window (browser) participates in the same raise_z ordering as the
 * built-in windows so it stacks like any other window. gui_wl_raise() puts it on
 * top (called when it's clicked/focused); gui_overdraw_top() re-draws built-in
 * windows whose raise_z beats it so they can sit above the browser. */
uint32_t g_wl_raise_z = 0;
void     gui_wl_raise(void)     { g_wl_raise_z = gui_next_z(); }
uint32_t gui_wl_z(void)         { return g_wl_raise_z; }

uint32_t gui_topmost_z_at(int32_t mx, int32_t my) {
    uint32_t best = 0;
    for (int i = 0; i < MAX_WINS; i++) {
        window_t *w = &g_wins[i];
        if (!w->active || w->state == WIN_HIDDEN || w->anim_phase == ANIM_CLOSE) continue;
        if (mx < (int32_t)w->x || mx >= (int32_t)(w->x + w->w)) continue;
        if (my < (int32_t)w->y || my >= (int32_t)(w->y + w->h)) continue;
        if (w->raise_z > best) best = w->raise_z;
    }
    return best;
}

uint32_t gui_topmost_z_at_nonterm(int32_t mx, int32_t my) {
    uint32_t best = 0;
    for (int i = 0; i < MAX_WINS; i++) {
        window_t *w = &g_wins[i];
        if (!w->active || w->state == WIN_HIDDEN || w->anim_phase == ANIM_CLOSE) continue;
        if (w->type == WIN_TERM) continue;
        if (mx < (int32_t)w->x || mx >= (int32_t)(w->x + w->w)) continue;
        if (my < (int32_t)w->y || my >= (int32_t)(w->y + w->h)) continue;
        if (w->raise_z > best) best = w->raise_z;
    }
    return best;
}

/* ── gui_init ────────────────────────────────────────────────────────── */

/* ── Settings persistence (linux-desktop only) ──────────────────────────────
 * The OS root is RAM and wiped each boot; /fifi-data survives. Persist the user's
 * theme/customizations there and reload them at startup. */
#ifdef __linux__
#define FIFI_SETTINGS_PATH "/fifi-data/fifi-settings.conf"
void gui_settings_save(void) {
    FILE *f = fopen(FIFI_SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "accent=%u\nwallpaper=%d\nclock_12h=%d\nanimations=%d\n"
               "statusbar=%d\ndesktop_info=%d\nutc_offset=%d\n"
               "panel_edge=%d\npanel_align=%d\npanel_autohide=%d\npanel_size=%d\n"
               "fx_glass=%d\nfx_shadows=%d\ncorner_radius=%d\n",
            (unsigned)g_theme.accent, g_theme.wallpaper, (int)g_theme.clock_12h,
            (int)g_theme.animations, (int)g_theme.statusbar,
            (int)g_theme.desktop_info, (int)g_theme.utc_offset,
            (int)g_theme.panel_edge, (int)g_theme.panel_align,
            (int)g_theme.panel_autohide, (int)g_theme.panel_size,
            (int)g_theme.fx_glass, (int)g_theme.fx_shadows, (int)g_theme.corner_radius);
    fclose(f);
}
void gui_settings_load(void) {
    FILE *f = fopen(FIFI_SETTINGS_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        int v; unsigned uv;
        if      (sscanf(line, "accent=%u", &uv) == 1)       g_theme.accent = uv;
        else if (sscanf(line, "wallpaper=%d", &v) == 1)     { if (v >= 0 && v < WALLPAPER_COUNT) g_theme.wallpaper = v; }
        else if (sscanf(line, "clock_12h=%d", &v) == 1)     g_theme.clock_12h = (v != 0);
        else if (sscanf(line, "animations=%d", &v) == 1)    g_theme.animations = (v != 0);
        else if (sscanf(line, "statusbar=%d", &v) == 1)     g_theme.statusbar = (v != 0);
        else if (sscanf(line, "desktop_info=%d", &v) == 1)  g_theme.desktop_info = (v != 0);
        else if (sscanf(line, "utc_offset=%d", &v) == 1)    { if (v >= -12 && v <= 14) g_theme.utc_offset = (int8_t)v; }
        else if (sscanf(line, "panel_edge=%d", &v) == 1)    { if (v >= 0 && v <= 3) g_theme.panel_edge = (uint8_t)v; }
        else if (sscanf(line, "panel_align=%d", &v) == 1)   { if (v >= 0 && v <= 2) g_theme.panel_align = (uint8_t)v; }
        else if (sscanf(line, "panel_autohide=%d", &v) == 1) g_theme.panel_autohide = (v != 0);
        else if (sscanf(line, "panel_size=%d", &v) == 1)    { if (v >= 0 && v <= 64) g_theme.panel_size = (uint8_t)v; }
        else if (sscanf(line, "fx_glass=%d", &v) == 1)      g_theme.fx_glass = (v != 0);
        else if (sscanf(line, "fx_shadows=%d", &v) == 1)    g_theme.fx_shadows = (v != 0);
        else if (sscanf(line, "corner_radius=%d", &v) == 1) { if (v >= 0 && v <= 12) g_theme.corner_radius = (uint8_t)v; }
    }
    fclose(f);
}

/* ── Desktop shortcut persistence ────────────────────────────────────────
 * User-added desktop shortcuts survive reboots via /fifi-data. The ephemeral
 * "Install FiFi OS" icon (/bin/fifi-installer) is deliberately NOT persisted:
 * it is re-added conditionally at boot only while the system is a live USB. */
#define FIFI_DESKTOP_PATH "/fifi-data/fifi-desktop.conf"
void gui_desktop_save(void) {
    FILE *f = fopen(FIFI_DESKTOP_PATH, "w");
    if (!f) return;
    for (int i = 0; i < DESK_ICON_MAX; i++) {
        if (!g_desk_icons[i].active) continue;
        if (strcmp(g_desk_icons[i].path, "/bin/fifi-installer") == 0) continue;
        fprintf(f, "icon=%s\t%s\n", g_desk_icons[i].path, g_desk_icons[i].label);
    }
    fclose(f);
}
void gui_desktop_load(void) {
    FILE *f = fopen(FIFI_DESKTOP_PATH, "r");
    if (!f) return;
    char line[320];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "icon=", 5) != 0) continue;
        char *path = line + 5;
        /* Split on TAB: path\tlabel; strip trailing newline. */
        char *tab = strchr(path, '\t');
        char *label = "";
        if (tab) { *tab = '\0'; label = tab + 1; }
        char *nl = strchr(label, '\n'); if (nl) *nl = '\0';
        nl = strchr(path, '\n'); if (nl) *nl = '\0';
        if (path[0]) gui_add_desktop_icon(path, label);
    }
    fclose(f);
}
#define FIFI_FAV_PATH "/fifi-data/fifi-favorites.conf"
void gui_fav_save(void) {
    FILE *f = fopen(FIFI_FAV_PATH, "w");
    if (!f) return;
    for (int i = 0; i < g_fav_count; i++)
        if (g_favs[i].active)
            fprintf(f, "fav=%s\t%s\n", g_favs[i].path, g_favs[i].label);
    fclose(f);
}
void gui_fav_load(void) {
    FILE *f = fopen(FIFI_FAV_PATH, "r");
    if (!f) return;
    char line[320];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "fav=", 4) != 0) continue;
        char *path = line + 4;
        char *tab = strchr(path, '\t');
        char *label = "";
        if (tab) { *tab = '\0'; label = tab + 1; }
        char *nl = strchr(label, '\n'); if (nl) *nl = '\0';
        nl = strchr(path, '\n'); if (nl) *nl = '\0';
        if (path[0]) gui_fav_add(path, label);
    }
    fclose(f);
}
#else
void gui_settings_save(void) {}
void gui_settings_load(void) {}
void gui_desktop_save(void) {}
void gui_desktop_load(void) {}
void gui_fav_save(void) {}
void gui_fav_load(void) {}
#endif

/* ── Taskbar favorites model (shared, both platforms) ─────────────────── */
fav_t g_favs[FAV_MAX];
int   g_fav_count = 0;
int   g_fav_hover = -1;
/* drag-to-reorder state */
static int     g_fav_drag_idx    = -1;    /* favorite pressed/dragged (-1 none) */
static bool    g_fav_drag_active = false; /* moved past threshold = real drag */
static int32_t g_fav_press_x     = 0;

bool gui_fav_add(const char *path, const char *label) {
    if (!path || !path[0]) return false;
    for (int i = 0; i < g_fav_count; i++)
        if (g_favs[i].active && strcmp(g_favs[i].path, path) == 0) return false; /* dup */
    if (g_fav_count >= FAV_MAX) return false;
    fav_t *e = &g_favs[g_fav_count++];
    int i = 0; for (; path[i] && i < 191; i++) e->path[i] = path[i]; e->path[i] = '\0';
    int j = 0; if (label) { for (; label[j] && j < 39; j++) e->label[j] = label[j]; } e->label[j] = '\0';
    e->active = true;
    return true;
}
void gui_fav_remove_at(int idx) {
    if (idx < 0 || idx >= g_fav_count) return;
    for (int i = idx; i < g_fav_count - 1; i++) g_favs[i] = g_favs[i + 1];
    g_fav_count--;
    g_favs[g_fav_count].active = false;
    if (g_fav_hover >= g_fav_count) g_fav_hover = -1;
}

void gui_init(void) {
    uint64_t fb_w = console_fb_width();

    /* Load persisted user settings (theme/colors/toggles) over the defaults. */
    gui_settings_load();

    /* Pick font based on display resolution so text is readable at any DPI. */
    if      (fb_w >= 3840) g_font_idx = 4;  /* ter32b -- 4K */
    else if (fb_w >= 2560) g_font_idx = 3;  /* ter28b -- 2.5K (e.g. 2560x1600) */
    else if (fb_w >= 1920) g_font_idx = 2;  /* ter24b -- 1080p */
    else if (fb_w >= 1280) g_font_idx = 1;  /* ter20b -- 720p */
    else                   g_font_idx = 0;  /* ter16b -- small/QEMU */
    if (!console_load_psf(g_font_paths[g_font_idx]))
        console_load_psf("/fonts/ter16b.psf");

    /* Initialize z-order: 0=bottom ... MAX_WINS-1=top */
    for (int i = 0; i < MAX_WINS; i++) g_z[i] = i;

    g_wins[0].active = true;
    g_wins[0].type   = WIN_TERM;
    g_wins[0].title  = "Terminal";
    g_wins[0].state  = WIN_HIDDEN;

    g_wins[1].active = true;
    g_wins[1].type   = WIN_FILES;
    /* Title points into fb.path -- auto-updates as user navigates */
    g_wins[1].title  = g_wins[1].fb.path;
    g_wins[1].state  = WIN_HIDDEN;

    g_wins[2].active = true;
    g_wins[2].type   = WIN_SETTINGS;
    g_wins[2].title  = "Settings";
    g_wins[2].state  = WIN_HIDDEN;

    g_wins[3].active                = true;
    g_wins[3].type                  = WIN_TEXT;
    g_wins[3].title                 = "Viewer";
    g_wins[3].state                 = WIN_HIDDEN;
    g_wins[3].text.srch_match_line  = -1;
    g_wins[3].text.h_scroll         = 0;
    g_wins[3].text.welcome_hover    = -1;

    console_fill_rect(0, desk_top(), fb_w, desk_avail(), COL_DESKTOP);
    taskbar_draw();
    fb_load(&g_wins[1].fb, "/");
    /* Boot to a clean desktop — no window auto-opens. The terminal (and every
     * other built-in) stays hidden until launched from the taskbar/launcher. */

#ifdef __linux__
    /* Restore user-added desktop shortcuts + taskbar favorites from storage. */
    gui_desktop_load();
    gui_fav_load();

    /* On a live USB (not yet installed to disk), show an "Install FiFi OS"
     * icon on the desktop. The installer creates /fifi-data/installed when
     * done, so the icon disappears on the next boot into the installed system. */
    if (vfs_filesize("installed") < 0) {
        __attribute__((weak)) void gui_add_desktop_icon(const char *path, const char *label);
        if (gui_add_desktop_icon)
            gui_add_desktop_icon("/bin/fifi-installer", "Install FiFi OS");
    }
#endif
}

/* ── Forward declarations for public API functions defined later ─────── */
void gui_add_desktop_icon(const char *path, const char *label);
void gui_set_wallpaper_image(const char *path);

/* ── File browser context menu executor (shared by keyboard + mouse) ── */

void gui_on_tick(void) {
    g_gui_tick++;
#ifdef __linux__
    struct timespec _st0, _stA, _stB, _stC;
    clock_gettime(CLOCK_MONOTONIC, &_st0);
#define _SUB_MS(a,b) (((b).tv_sec-(a).tv_sec)*1000L+((b).tv_nsec-(a).tv_nsec)/1000000L)
#endif

    /* ── Once-per-second: redraw status bar and live settings if open ── */
    {
        static uint64_t s_last_sec = (uint64_t)-1;
        uint64_t hz = pit_get_hz();
        if (!hz) hz = 100;
        uint64_t now_sec = pit_ticks() / hz;
        if (now_sec != s_last_sec) {
            s_last_sec = now_sec;
            /* tick_redraw repaints only the two status strips + settings window if open.
             * Confirmed 0ms render cost on real hardware — much cheaper than full_redraw. */
            g_redraw_src = 1; tick_redraw();
        }
    }

    /* ── IPC window close: trigger full redraw so desktop bg covers the closed region ── */
    {
        __attribute__((weak)) bool ipc_needs_redraw(void);
        if (ipc_needs_redraw && ipc_needs_redraw()) {
            g_redraw_src = 99; full_redraw();
        }
    }

    /* ── Toast countdown ── */
    {
        if (g_toast_ticks > 0) {
            g_toast_ticks--;
            if (g_toast_ticks == 0) {
                g_toast_msg[0] = '\0';
                full_redraw();
            }
        }
    }

    /* ── Window open/close animation ── */
    {
        bool any_anim = false;
        for (int _ai = 0; _ai < MAX_WINS; _ai++) {
            window_t *aw = &g_wins[_ai];
            if (aw->anim_phase == ANIM_NONE) continue;
            aw->anim_step++;
            any_anim = true;
            if (aw->anim_step > ANIM_TICKS) {
                if (aw->anim_phase == ANIM_OPEN) {
                    aw->anim_phase = ANIM_NONE;
                } else { /* ANIM_CLOSE */
                    aw->anim_phase = ANIM_NONE;
                    aw->state = WIN_HIDDEN;
                }
            }
        }
        if (any_anim) { g_redraw_src = 2; full_redraw(); }
    }

    /* ── Cursor blink: trigger full redraw if any window has active search or edit mode ── */
    if ((g_gui_tick % 25u) == 0u) {
        for (int i = 0; i < MAX_WINS; i++) {
            window_t *bw = &g_wins[i];
            if (!bw->active || bw->state == WIN_HIDDEN || bw->anim_phase != ANIM_NONE) continue;
            if ((bw->type == WIN_FILES && bw->fb.search_active) ||
                (bw->type == WIN_TEXT  && (bw->text.srch_active || bw->text.edit_mode))) {
                g_redraw_src = 3; full_redraw();
                break;
            }
        }
    }

    int32_t mx, my;
    bool lbtn, rbtn;
    mouse_get_state(&mx, &my, &lbtn, &rbtn);

    bool btn_pressed  = lbtn && !g_prev_lbtn;
    bool btn_released = !lbtn && g_prev_lbtn;
    g_prev_lbtn = lbtn;

    /* Right-click: consumed from ring buffer so fast press/release isn't missed */
    int32_t rcx, rcy;
    bool rbtn_pressed = mouse_consume_rclick(&rcx, &rcy);
    if (rbtn_pressed) { mx = rcx; my = rcy; }

    /* Synthetic click from mouse_click()/mclick: lbtn stays false so btn_pressed
     * won't fire — consume it here and treat it as a real press. */
    if (!btn_pressed) {
        int32_t sx, sy;
        if (mouse_consume_click(&sx, &sy)) {
            btn_pressed = true;
            mx = sx;
            my = sy;
        }
    }

    uint64_t fb_h = console_fb_height();
    uint64_t fb_w = console_fb_width();
    uint64_t ty   = panel_y();               /* panel top (edge-aware) */
    uint64_t ty_end = ty + TASKBAR_H;        /* panel bottom */
    /* "in the panel strip" — a vertical panel occupies an X strip, a horizontal
     * one a Y strip. */
    bool in_strip;
    if (panel_is_vertical()) {
        uint64_t px = panel_x();
        in_strip = ((uint64_t)mx >= px && (uint64_t)mx < px + TASKBAR_H);
    } else {
        in_strip = ((uint64_t)my >= ty && (uint64_t)my < ty_end);
    }
    /* Auto-hide: reveal when the cursor hits the panel's screen edge; stay shown
     * while it's over the panel; hide when it leaves. A hidden panel is not
     * clickable (in_panel false) so clicks pass through to the window beneath. */
    if (g_theme.panel_autohide) {
        bool at_edge;
        switch (g_theme.panel_edge) {
            case PANEL_TOP:   at_edge = ((uint64_t)my <= ty + 2u); break;
            case PANEL_LEFT:  at_edge = ((uint64_t)mx <= 2u); break;
            case PANEL_RIGHT: at_edge = ((uint64_t)mx + 3u >= fb_w); break;
            default:          at_edge = ((uint64_t)my + 3u >= fb_h); break;  /* BOTTOM */
        }
        bool reveal = g_panel_revealed ? (at_edge || in_strip) : at_edge;
        if (reveal != g_panel_revealed) { g_panel_revealed = reveal; full_redraw(); }
    }
    /* "in the panel strip", edge-aware and (when auto-hiding) only while shown. */
    bool in_panel = (!g_theme.panel_autohide || g_panel_revealed) && in_strip;
    (void)ty_end;

    /* ── Taskbar favorite drag-to-reorder / click-to-launch ──────────────
     * A left-press on a favorite records it (g_fav_drag_idx) and defers the
     * action to release: past a small threshold it becomes a drag that
     * reorders the strip live; otherwise release launches the app. */
    /* Only user favorites (unified index >= FAVBAR_BUILTINS) drag/launch here;
     * the built-in launchers toggle their window immediately on press. */
    if (g_fav_drag_idx >= FAVBAR_BUILTINS) {
        if (lbtn) {
            if (!g_fav_drag_active && (mx > g_fav_press_x + 8 || mx < g_fav_press_x - 8))
                g_fav_drag_active = true;
            if (g_fav_drag_active && g_fav_count > 1) {
                uint64_t sx  = favbar_start_x();
                uint64_t fbw = fav_btn_w() + TASKBTN_GAP;
                int tgt = ((uint64_t)mx >= sx) ? (int)(((uint64_t)mx - sx) / fbw) : 0;
                if (tgt < FAVBAR_BUILTINS) tgt = FAVBAR_BUILTINS;   /* can't move before built-ins */
                if (tgt >= favbar_count()) tgt = favbar_count() - 1;
                if (tgt != g_fav_drag_idx) {
                    int a = g_fav_drag_idx - FAVBAR_BUILTINS;  /* source user-fav index */
                    int b = tgt - FAVBAR_BUILTINS;             /* dest user-fav index */
                    fav_t tmp = g_favs[a];
                    if (b > a) for (int i = a; i < b; i++) g_favs[i] = g_favs[i + 1];
                    else       for (int i = a; i > b; i--) g_favs[i] = g_favs[i - 1];
                    g_favs[b] = tmp;
                    g_fav_drag_idx = tgt;
                    g_fav_hover = tgt;
                    taskbar_draw();
                }
            }
            return;   /* hold the drag; ignore other handlers this tick */
        } else {
            int  idx      = g_fav_drag_idx;
            bool was_drag = g_fav_drag_active;
            g_fav_drag_idx = -1;
            g_fav_drag_active = false;
            if (was_drag) {
                gui_fav_save();
                gui_toast("Favorites reordered", 0x0060a0e0u);
            } else {
                int j = idx - FAVBAR_BUILTINS;
                if (j >= 0 && j < g_fav_count) {
                    __attribute__((weak)) void gui_spawn_app(const char *path);
                    if (gui_spawn_app) gui_spawn_app(g_favs[j].path);
                }
            }
            full_redraw();
            return;
        }
    }

    /* ── Hover tracking for file browser (z-order top-to-bottom) ── */
    /* Only call fb_on_motion for the files window if it is the topmost window
     * at the cursor position — prevents it rendering over higher-z windows. */
    if (!g_dragging && !g_resizing && !g_launcher_open) {
        for (int zi = MAX_WINS - 1; zi >= 0; zi--) {
            int si = g_z[zi];
            window_t *w = &g_wins[si];
            if (!w->active || w->state == WIN_HIDDEN) continue;
            /* Stop at first window whose bounds contain the cursor */
            if ((uint64_t)mx >= w->x && (uint64_t)mx < w->x + w->w &&
                (uint64_t)my >= w->y && (uint64_t)my < w->y + w->h) {
                /* Only the globally-topmost window reacts to hover. */
                if (w->type == WIN_FILES && gui_is_topmost(si)) fb_on_motion(w, mx, my);
                break;
            }
        }
    }

    /* ── Hover tracking for text viewer welcome screen ── */
    if (!g_dragging && !g_resizing && !g_launcher_open) {
        for (int zi = MAX_WINS - 1; zi >= 0; zi--) {
            int si = g_z[zi];
            window_t *w = &g_wins[si];
            if (!w->active || w->state == WIN_HIDDEN) continue;
            /* Stop at first window whose bounds contain cursor */
            if ((uint64_t)mx >= w->x && (uint64_t)mx < w->x + w->w &&
                (uint64_t)my >= w->y && (uint64_t)my < w->y + w->h) {
                if (w->type == WIN_TEXT && gui_is_topmost(si) &&
                    !w->text.edit_mode && !w->text.data && w->text.size == 0
                    && !w->text.path[0] && g_recent_count > 0) {
                    uint64_t fh2 = console_font_height();
                    uint64_t ix2 = w->x + BORDER, iy2 = w->y + TITLE_H;
                    uint64_t iw2 = w->w - 2u * BORDER, ih2 = w->h - TITLE_H - BORDER;
                    int nrec2 = g_recent_count;
                    uint64_t block_h2 = (uint64_t)(8 + nrec2 + 2) * fh2 + 4u;
                    uint64_t top_y2 = iy2 + (ih2 > block_h2 + 8u ? (ih2 - block_h2) / 2u : 4u);
                    uint64_t rec_y2 = top_y2 + (uint64_t)(8 + 2) * fh2;
                    int new_hov = -1;
                    if ((uint64_t)mx >= ix2 && (uint64_t)mx < ix2 + iw2 &&
                        (uint64_t)my >= rec_y2 && (uint64_t)my < rec_y2 + (uint64_t)nrec2 * fh2) {
                        new_hov = (int)((uint64_t)my - rec_y2) / (int)fh2;
                        if (new_hov >= nrec2) new_hov = -1;
                    }
                    (void)iw2;
                    if (new_hov != w->text.welcome_hover) {
                        w->text.welcome_hover = new_hov;
                        text_render(w);
                    }
                }
                break;
            }
        }
    }

    /* ── Resolve topmost visible window (z-order top) ── */
    int top_vis = -1;
    for (int zi = MAX_WINS - 1; zi >= 0; zi--) {
        int si = g_z[zi];
        if (g_wins[si].active && g_wins[si].state != WIN_HIDDEN) { top_vis = si; break; }
    }

    /* ── Chrome hover tracking — topmost focused window only ── */
    if (!g_dragging && !g_resizing) {
        int new_chrome_win = -1;
        int new_chrome_btn = 0;

        if (top_vis >= 0 && gui_is_topmost(top_vis)) {
            window_t *w = &g_wins[top_vis];
            int32_t wy = (int32_t)w->y;
            int32_t wx = (int32_t)w->x;
            int32_t we = wx + (int32_t)w->w;
            if (mx >= wx && mx < we && my >= wy && my < wy + (int32_t)TITLE_H) {
                int32_t clx = (int32_t)w->btn_cls_x;
                int32_t mxx = (int32_t)w->btn_max_x;
                int32_t mnx = (int32_t)w->btn_min_x;
                new_chrome_win = top_vis;
                if (mx >= clx && mx < clx + (int32_t)BTN_W)
                    new_chrome_btn = 1;
                else if (mx >= mxx && mx < mxx + (int32_t)BTN_W)
                    new_chrome_btn = 2;
                else if (mx >= mnx && mx < mnx + (int32_t)BTN_W)
                    new_chrome_btn = 3;
            }
        }

        /* Partial repaint: only redraw title bars that changed hover state.
         * Avoids full_redraw() (~9ms) on every button hover transition. */
        {
            int _old_cw = g_chrome_win;
            int _old_cb = g_chrome_btn;
            g_chrome_win = new_chrome_win;
            if (new_chrome_btn != _old_cb || new_chrome_win != _old_cw) {
                g_chrome_btn = new_chrome_btn;
                if (_old_cw >= 0 && _old_cw < MAX_WINS && g_wins[_old_cw].active)
                    win_draw_chrome(&g_wins[_old_cw], false);
                if (new_chrome_win >= 0 && new_chrome_win < MAX_WINS &&
                        new_chrome_win != _old_cw && g_wins[new_chrome_win].active)
                    win_draw_chrome(&g_wins[new_chrome_win], false);
            }
        }
    }

    /* ── Resize edge hover tracking — topmost focused window only ── */
    if (!g_dragging && !g_resizing && !g_launcher_open) {
        int          new_rw = -1;
        resize_dir_t new_rd = RES_NONE;
        if (top_vis >= 0 && gui_is_topmost(top_vis)) {
            resize_dir_t rd = hit_resize(&g_wins[top_vis], mx, my);
            if (rd != RES_NONE) { new_rw = top_vis; new_rd = rd; }
        }
        /* Require 3 consecutive ticks in resize zone before committing — prevents
         * drive-by cursor flicker when passing near edges during normal movement.
         * Exiting the zone always applies immediately so the cursor restores fast. */
        if (new_rd == RES_NONE) {
            g_resize_pending_win   = -1;
            g_resize_pending_dir   = RES_NONE;
            g_resize_pending_ticks = 0;
            if (g_resize_hover_win != -1 || g_resize_hover_dir != RES_NONE) {
                g_resize_hover_win = -1;
                g_resize_hover_dir = RES_NONE;
            }
        } else {
            if (new_rw == g_resize_pending_win && new_rd == g_resize_pending_dir) {
                g_resize_pending_ticks++;
            } else {
                g_resize_pending_win   = new_rw;
                g_resize_pending_dir   = new_rd;
                g_resize_pending_ticks = 1;
            }
            if (g_resize_pending_ticks >= 3 &&
                    (new_rw != g_resize_hover_win || new_rd != g_resize_hover_dir)) {
                g_resize_hover_win = new_rw;
                g_resize_hover_dir = new_rd;
            }
        }
    }

    /* ── Cursor shape context ── */
    if (!g_dragging && !g_resizing) {
        cursor_type_t want = CURSOR_ARROW;
        /* Resize edge → resize cursors */
        if (g_resize_hover_dir != RES_NONE) {
            switch (g_resize_hover_dir) {
                case RES_E: case RES_W:
                    want = CURSOR_RESIZE_H; break;
                case RES_N: case RES_S:
                    want = CURSOR_RESIZE_V; break;
                case RES_NE: case RES_SW:
                    want = CURSOR_RESIZE_V; break;
                case RES_NW: case RES_SE:
                    want = CURSOR_RESIZE_H; break;
                default: break;
            }
        } else {
            /* Check topmost visible window under cursor */
            for (int zi = MAX_WINS - 1; zi >= 0; zi--) {
                int si = g_z[zi];
                window_t *wc = &g_wins[si];
                if (!wc->active || wc->state == WIN_HIDDEN) continue;
                if ((uint64_t)mx >= wc->x && (uint64_t)mx < wc->x + wc->w &&
                    (uint64_t)my >= wc->y && (uint64_t)my < wc->y + wc->h) {
                    /* Title bar drag area → move cursor */
                    if ((uint64_t)my < wc->y + TITLE_H &&
                        (uint64_t)mx < wc->x + wc->w - 3u * BTN_W) {
                        want = CURSOR_MOVE;
                    } else if (wc->type == WIN_TEXT) {
                        /* Text content area → I-beam */
                        want = CURSOR_TEXT;
                    } else if (wc->type == WIN_FILES) {
                        /* File list area → hand */
                        want = CURSOR_HAND;
                    }
                    break;
                }
            }
        }
        mouse_set_cursor(want);
    }

    /* ── Launcher hover tracking (mouse moves the selection) ── */
    if (g_launcher_open) {
        if (g_launchctx_row >= 0) {
            /* context menu open: track its hover instead of the row hover */
            int h = launchctx_hit(mx, my);
            if (h != g_launchctx_hover) { g_launchctx_hover = h; full_redraw(); }
        } else {
            int new_hover = launcher_hit_row(mx, my);
            if (new_hover >= 0 && new_hover != g_launcher_hover) {
                g_launcher_hover = new_hover;
                launcher_draw();
            }
        }
    }

    /* ── Taskbar favorite hover tracking ── */
    if (!g_launcher_open) {
        int new_fhov = favbar_hit(mx, my);
        if (new_fhov != g_fav_hover) {
            g_fav_hover = new_fhov;
            favbar_draw();
        }
    }

    /* (Built-in window buttons are now favorites — handled by favbar hover above.) */

    /* ── Tray icon hover tooltips (battery/cpu/net/mem/vol/clock) ── */
    {
        int it = g_launcher_open ? TRAY_NONE : tray_item_at(mx, my);
        if (it != g_tray_hover) {
            g_tray_hover = it;
            full_redraw();   /* render path draws the tooltip when g_tray_hover >= 0 */
        }
    }

    /* ── Context menu hover tracking ── */
    if (g_ctx_open) {
        static const char *_ci_arr[CTX_ITEMS] = {
            "Terminal", "Files", "Settings", "Viewer",
            NULL, "File Browser", "Sys Monitor", "Net Monitor", "New Term", "Editor",
            NULL, "Lock Screen", "Show Desktop",
        };
        uint64_t ctx_x = (uint64_t)g_ctx_x;
        uint64_t ctx_y = (uint64_t)g_ctx_y;
        uint64_t tot_h = 2u;
        for (int _k = 0; _k < (int)CTX_ITEMS; _k++)
            tot_h += _ci_arr[_k] ? CTX_ITEM_H : 8u;
        int new_chov = -1;
        if ((uint64_t)mx >= ctx_x && (uint64_t)mx < ctx_x + ctx_w() &&
            (uint64_t)my >= ctx_y + 1u && (uint64_t)my < ctx_y + tot_h) {
            uint64_t dy = (uint64_t)my - (ctx_y + 1u);
            uint64_t yoff = 0;
            for (int _k = 0; _k < (int)CTX_ITEMS; _k++) {
                uint64_t item_h = _ci_arr[_k] ? CTX_ITEM_H : 8u;
                if (dy < yoff + item_h) {
                    if (_ci_arr[_k]) new_chov = _k;
                    break;
                }
                yoff += item_h;
            }
        }
        if (new_chov != g_ctx_hover) {
            g_ctx_hover = new_chov;
            ctx_draw();
        }
    }

    /* ── FB context menu hover tracking ── */
    if (g_fb_ctx_open) {
        uint64_t fb_cx = (uint64_t)g_fb_ctx_x;
        uint64_t fb_cy = (uint64_t)g_fb_ctx_y;
        int new_fhov = -1;
        if ((uint64_t)mx >= fb_cx && (uint64_t)mx < fb_cx + fb_ctx_w() &&
            (uint64_t)my >= fb_cy + 1u &&
            (uint64_t)my < fb_cy + 1u + (uint64_t)g_fb_ctx_n * CTX_ITEM_H) {
            new_fhov = (int)((uint64_t)my - (fb_cy + 1u)) / (int)CTX_ITEM_H;
        }
        if (new_fhov != g_fb_ctx_hover) {
            g_fb_ctx_hover = new_fhov;
            fb_ctx_draw();
        }
    }

    /* ── Text context menu hover tracking ── */
    if (g_txt_ctx_open) {
        uint64_t tc_x = (uint64_t)g_txt_ctx_x;
        uint64_t tc_y = (uint64_t)g_txt_ctx_y;
        int new_thov = -1;
        if ((uint64_t)mx >= tc_x && (uint64_t)mx < tc_x + txt_ctx_w() &&
            (uint64_t)my >= tc_y + 1u &&
            (uint64_t)my < tc_y + 1u + (uint64_t)(TXT_CTX_ITEMS * CTX_ITEM_H)) {
            new_thov = (int)((uint64_t)my - (tc_y + 1u)) / (int)CTX_ITEM_H;
        }
        if (new_thov != g_txt_ctx_hover) {
            g_txt_ctx_hover = new_thov;
            txt_ctx_draw();
        }
    }


#ifdef __linux__
    clock_gettime(CLOCK_MONOTONIC, &_stA);
#endif

    /* ── Keyboard capture + input for focused non-terminal window ── */
    {
        /* Find frontmost non-terminal visible window using z-order */
        window_t *focused = NULL;
        for (int zi = MAX_WINS - 1; zi >= 0; zi--) {
            int ki = g_z[zi];
            window_t *kw = &g_wins[ki];
            if (kw->active && kw->state != WIN_HIDDEN
                && kw->anim_phase != ANIM_CLOSE && kw->type != WIN_TERM) {
                focused = kw; break;
            }
        }

        /* Manage GUI keyboard capture based on window visibility.
         * Also capture when terminal is hidden — prevents keystrokes from
         * reaching the shell's stdin when the terminal window is closed. */
        bool term_vis = g_wins[0].active && g_wins[0].state != WIN_HIDDEN
                        && g_wins[0].anim_phase != ANIM_CLOSE;
        static bool s_gui_cap = false;
        bool want_cap = (focused != NULL && top_vis != 0) || !term_vis;
        if (want_cap != s_gui_cap) {
            keyboard_set_gui_capture(want_cap);
            s_gui_cap = want_cap;
        }

        /* Drain GUI key buffer — KEY_ALTTAB arrives here regardless of capture mode */
        {
            int ch;
            bool changed = false;
            bool closed  = false;
            while ((ch = keyboard_gui_try_getchar()) != -1) {
                /* ── F1-F4: toggle Terminal / Files / Settings / Viewer ── */
                if ((uint8_t)ch >= KEY_F1 && (uint8_t)ch <= KEY_F4) {
                    int slot = (uint8_t)ch - KEY_F1;
                    if (slot < MAX_WINS && (slot < 3 || g_wins[3].active)) {
                        window_t *fw = &g_wins[slot];
                        if (fw->state == WIN_HIDDEN) {
                            raise_win(slot);
                            win_show(fw, slot);
                        } else {
                            /* Toggle behaviour: if this window is the GLOBALLY topmost
                             * window, hide it; otherwise bring it to the front. "Globally
                             * topmost" compares raise_z across every built-in AND IPC window. */
                            extern uint32_t ipc_topmost_z(void);
                            uint32_t my_z = g_wins[slot].raise_z;
                            uint32_t top = ipc_topmost_z();
                            for (int _j = 0; _j < MAX_WINS; _j++) {
                                window_t *_ow = &g_wins[_j];
                                if (!_ow->active || _ow->state == WIN_HIDDEN || _ow->anim_phase == ANIM_CLOSE) continue;
                                if (_ow->raise_z > top) top = _ow->raise_z;
                            }
                            if (my_z >= top) {
                                win_hide(fw, slot);
                            } else {
                                raise_win(slot);
                                full_redraw();
                                full_redraw();
                            }
                        }
                    }
                    continue;
                }
                /* ── F5: launch Sys Monitor; F6: launch Net Monitor ── */
                if ((uint8_t)ch == KEY_F5) {
                    __attribute__((weak)) void gui_spawn_app(const char *path);
                    if (gui_spawn_app) gui_spawn_app("/bin/fifi-sysmon");
                    continue;
                }
                if ((uint8_t)ch == KEY_F6) {
                    __attribute__((weak)) void gui_spawn_app(const char *path);
                    if (gui_spawn_app) gui_spawn_app("/bin/fifi-netmon");
                    continue;
                }
                /* ── F7: launch Calculator ── */
                if ((uint8_t)ch == KEY_F7) {
                    __attribute__((weak)) void gui_spawn_app(const char *path);
                    if (gui_spawn_app) gui_spawn_app("/bin/fifi-calc");
                    continue;
                }
                /* ── F11/F12: Volume Down / Up ── */
                if ((uint8_t)ch == KEY_F11) {
                    int v = hda_get_volume() - 5; if (v < 0) v = 0;
                    hda_set_volume(v);
                    char _vm[24]; snprintf(_vm, sizeof(_vm), "Volume: %d%%", v);
                    gui_toast_extern(_vm, 0x0060a0e0u);
                    if (g_wins[2].active && g_wins[2].state != WIN_HIDDEN) tick_redraw();
                    continue;
                }
                if ((uint8_t)ch == KEY_F12) {
                    int v = hda_get_volume() + 5; if (v > 100) v = 100;
                    hda_set_volume(v);
                    char _vm[24]; snprintf(_vm, sizeof(_vm), "Volume: %d%%", v);
                    gui_toast_extern(_vm, 0x0060a0e0u);
                    if (g_wins[2].active && g_wins[2].state != WIN_HIDDEN) tick_redraw();
                    continue;
                }
                /* NOTE: Super+arrow snapping and Super+D/+L are consumed by the
                 * Linux main loop (main.c key drains) before reaching this ring —
                 * ipc_snap_focused / wayland_snap_focused / gui_show_desktop /
                 * compositor_lock. Only the bare Super tap is handled here. */
                /* ── Super+/: keyboard shortcuts overlay; any key closes it ── */
                if ((uint8_t)ch == KEY_SUPER_HELP) {
                    g_help_open = !g_help_open;
                    full_redraw();
                    continue;
                }
                if (g_help_open) {
                    g_help_open = false;
                    full_redraw();
                    continue;
                }
                /* ── Super tap: toggle the app launcher (like other desktops) ── */
                if ((uint8_t)ch == KEY_SUPER) {
                    g_vol_popup_open = false;
                    g_cal_popup_open = false;
                    g_launcher_open = !g_launcher_open;
                    g_launcher_hover = -1;
                    if (g_launcher_open) {
                        launcher_open_reset();
                        taskbar_draw();
                        launcher_draw();
                    } else {
                        full_redraw();
                    }
                    continue;
                }
                /* ── Launcher: type to search, arrows to move, Enter to launch ── */
                if (g_launcher_open) {
                    if ((uint8_t)ch == KEY_UP) {
                        if (g_launch_filt_n > 0) {
                            if (--g_launcher_hover < 0) g_launcher_hover = g_launch_filt_n - 1;
                            int rv = (int)launcher_rows_visible();
                            if (g_launcher_hover < g_launcher_scroll) g_launcher_scroll = g_launcher_hover;
                            if (g_launcher_hover >= g_launcher_scroll + rv) g_launcher_scroll = g_launcher_hover - rv + 1;
                        }
                        launcher_draw(); continue;
                    } else if ((uint8_t)ch == KEY_DOWN) {
                        if (g_launch_filt_n > 0) {
                            if (++g_launcher_hover >= g_launch_filt_n) g_launcher_hover = 0;
                            int rv = (int)launcher_rows_visible();
                            if (g_launcher_hover < g_launcher_scroll) g_launcher_scroll = g_launcher_hover;
                            if (g_launcher_hover >= g_launcher_scroll + rv) g_launcher_scroll = g_launcher_hover - rv + 1;
                        }
                        launcher_draw(); continue;
                    } else if (ch == '\r' || ch == '\n') {
                        int _sel = g_launcher_hover;
                        g_launcher_open = false; g_launcher_hover = -1;
                        launcher_do_launch(_sel);
                        continue;
                    } else if (ch == 0x1b) {   /* Escape */
                        g_launcher_open = false; g_launcher_hover = -1; full_redraw(); continue;
                    } else if (ch == 0x08 || ch == 0x7f) {   /* Backspace */
                        if (g_launch_qlen > 0) {
                            g_launch_q[--g_launch_qlen] = '\0';
                            launcher_filter();
                        }
                        launcher_draw(); continue;
                    } else if ((uint8_t)ch >= 0x20 && (uint8_t)ch < 0x7f) {   /* printable → query */
                        if (g_launch_qlen < (int)sizeof(g_launch_q) - 1) {
                            g_launch_q[g_launch_qlen++] = (char)ch;
                            g_launch_q[g_launch_qlen] = '\0';
                            launcher_filter();
                        }
                        launcher_draw(); continue;
                    }
                    continue;   /* swallow other keys while launcher is open */
                }
                /* ── Context menu keyboard navigation ── */
                if (g_ctx_open) {
                    if ((uint8_t)ch == KEY_UP) {
                        if (--g_ctx_hover < 0) g_ctx_hover = (int)CTX_ITEMS - 1;
                        ctx_draw(); continue;
                    } else if ((uint8_t)ch == KEY_DOWN) {
                        if (++g_ctx_hover >= (int)CTX_ITEMS) g_ctx_hover = 0;
                        ctx_draw(); continue;
                    } else if ((ch == '\r' || ch == '\n' || ch == ' ') && g_ctx_hover >= 0) {
                        int _ci = g_ctx_hover;
                        g_ctx_open = false; g_ctx_hover = -1;
                        if (_ci < MAX_WINS) {
                            window_t *_cw = &g_wins[_ci];
                            raise_win(_ci);
                            if (_cw->state == WIN_HIDDEN) win_show(_cw, _ci); else full_redraw();
                        } else if (_ci >= 5 && _ci <= 9) {
                            static const char *_cc[] = {
                                "/bin/fifi-filebrowser",
                                "/bin/fifi-sysmon",
                                "/bin/fifi-netmon",
                                "/bin/fifi-terminal",
                                "/bin/fifi-editor",
                            };
                            __attribute__((weak)) void gui_spawn_app(const char *path);
                            if (gui_spawn_app) gui_spawn_app(_cc[_ci - 5]);
                            full_redraw();
                        } else if (_ci == 11) {
                            __attribute__((weak)) void compositor_lock(void);
                            if (compositor_lock) compositor_lock();
                            full_redraw();
                        } else if (_ci == 12) {
                            gui_show_desktop();
                        }
                        continue;
                    } else { g_ctx_open = false; g_ctx_hover = -1; full_redraw(); continue; }
                }
                if (g_txt_ctx_open && g_txt_ctx_win >= 0) {
                    if ((uint8_t)ch == KEY_UP) {
                        if (--g_txt_ctx_hover < 0) g_txt_ctx_hover = TXT_CTX_ITEMS - 1;
                        txt_ctx_draw(); continue;
                    } else if ((uint8_t)ch == KEY_DOWN) {
                        if (++g_txt_ctx_hover >= TXT_CTX_ITEMS) g_txt_ctx_hover = 0;
                        txt_ctx_draw(); continue;
                    } else if ((ch == '\r' || ch == '\n' || ch == ' ') && g_txt_ctx_hover >= 0) {
                        int _ti = g_txt_ctx_hover;
                        window_t *_tw = &g_wins[g_txt_ctx_win];
                        text_state_t *_tts = &_tw->text;
                        g_txt_ctx_open = false; g_txt_ctx_hover = -1;
                        if (_ti == 0) { /* Select All */
                            _tts->sel_anchor = 0; _tts->sel_end = (int32_t)_tts->edit_size;
                            _tts->edit_cur = _tts->edit_size; edit_sync_pos(_tts);
                        } else if (_ti == 1) { /* Copy */
                            edit_copy_to_clip(_tts); gui_toast("Copied", 0x0080c8a0u);
                        } else if (_ti == 2) { /* Cut */
                            edit_push_undo(_tts); edit_copy_to_clip(_tts);
                            edit_delete_selection(_tts); edit_recount(_tw);
                            edit_scroll_to_cursor(_tw); gui_toast("Cut", 0x0080c8a0u);
                        } else if (_ti == 3) { /* Paste */
                            if (g_clipboard && g_clipboard_len > 0) {
                                edit_push_undo(_tts); edit_paste(_tw);
                                edit_recount(_tw); edit_scroll_to_cursor(_tw);
                                gui_toast("Pasted", 0x0080c8a0u);
                            }
                        } else if (_ti == 4) { /* Find */
                            _tts->srch_active = true; _tts->srch_is_goto = false;
                            _tts->srch_is_repl = false; _tts->srch_len = 0;
                        }
                        text_render(_tw); full_redraw(); continue;
                    } else { g_txt_ctx_open = false; g_txt_ctx_hover = -1; full_redraw(); continue; }
                }
                if (g_fb_ctx_open) {
                    if ((uint8_t)ch == KEY_UP) {
                        if (--g_fb_ctx_hover < 0) g_fb_ctx_hover = g_fb_ctx_n - 1;
                        fb_ctx_draw(); continue;
                    } else if ((uint8_t)ch == KEY_DOWN) {
                        if (++g_fb_ctx_hover >= g_fb_ctx_n) g_fb_ctx_hover = 0;
                        fb_ctx_draw(); continue;
                    } else if ((ch == '\r' || ch == '\n' || ch == ' ') && g_fb_ctx_hover >= 0) {
                        int _fki = g_fb_ctx_hover;
                        g_fb_ctx_open = false; g_fb_ctx_hover = -1;
                        fb_ctx_run(_fki); continue;
                    } else if (ch == 27) {
                        g_fb_ctx_open = false; g_fb_ctx_hover = -1; full_redraw(); continue;
                    } else {
                        /* Any other key: close ctx menu and re-process */
                        g_fb_ctx_open = false; full_redraw();
                        /* fall through to normal key handling below */
                    }
                }
                /* ── Global: Alt+Tab cycles visible windows ── */
                if ((uint8_t)ch == KEY_ALTTAB) {
                    int vis[MAX_WINS], vc = 0;
                    for (int zi = 0; zi < MAX_WINS; zi++) {
                        int si = g_z[zi];
                        if (g_wins[si].active && g_wins[si].state != WIN_HIDDEN)
                            vis[vc++] = si;
                    }
                    if (vc >= 2) { raise_win(vis[vc - 2]); full_redraw(); }
                    continue;
                }
                if (!focused) continue;
                /* ── Terminal scrollback keyboard controls ── */
                if (focused->type == WIN_TERM) {
                    int tot_tsb = console_tsb_count_lines();
                    if ((uint8_t)ch == KEY_PGUP) {
                        /* PgUp: scroll back one page */
                        uint64_t fh2 = focused->h > TITLE_H + BORDER + 2u * PAD
                                       ? focused->h - TITLE_H - BORDER - 2u * PAD : 1u;
                        int page = (int)(fh2 / console_font_height());
                        if (page < 1) page = 1;
                        g_term_scroll += page;
                        if (g_term_scroll > tot_tsb) g_term_scroll = tot_tsb;
                        console_set_suppress_draw(g_term_scroll > 0);
                        full_redraw();
                        continue;
                    } else if ((uint8_t)ch == KEY_PGDN) {
                        /* PgDn: scroll forward one page */
                        uint64_t fh3 = focused->h > TITLE_H + BORDER + 2u * PAD
                                       ? focused->h - TITLE_H - BORDER - 2u * PAD : 1u;
                        int page = (int)(fh3 / console_font_height());
                        if (page < 1) page = 1;
                        g_term_scroll -= page;
                        if (g_term_scroll < 0) g_term_scroll = 0;
                        console_set_suppress_draw(g_term_scroll > 0);
                        full_redraw();
                        continue;
                    } else if (g_term_scroll > 0 && (uint8_t)ch != KEY_PGUP && (uint8_t)ch != KEY_PGDN) {
                        /* Any non-scroll key: snap back to live view */
                        g_term_scroll = 0;
                        console_set_suppress_draw(false);
                        full_redraw();
                        /* fall through to shell key delivery */
                    }
                }
                if (focused->type == WIN_FILES) {
                    if (focused->fb.input_active) {
                        /* Create file / dir input mode */
                        if (ch == 27) {
                            focused->fb.input_active    = false;
                            focused->fb.input_is_rename = false;
                            focused->fb.input_len       = 0;
                            focused->fb.input_cursor    = 0;
                            focused->fb.input_buf[0]    = '\0';
                            changed = true;
                        } else if (ch == '\r' || ch == '\n') {
                            if (focused->fb.input_len > 0) {
                                char fpath[256];
                                fb_path_join(fpath, focused->fb.path, focused->fb.input_buf);
                                if (focused->fb.input_is_rename) {
                                    char opath[256];
                                    fb_path_join(opath, focused->fb.path, focused->fb.input_orig);
                                    vfs_rename(opath, fpath);
                                    gui_toast("Renamed", 0x0080e8b0u);
                                } else if (focused->fb.input_isdir) {
                                    vfs_mkdir(fpath);
                                    gui_toast("Directory created", 0x0080e8b0u);
                                } else {
                                    vfs_write(fpath, "", 0);
                                    gui_toast("File created", 0x0080e8b0u);
                                }
                                fb_navigate(&focused->fb, focused->fb.path);
                            }
                            focused->fb.input_active    = false;
                            focused->fb.input_is_rename = false;
                            focused->fb.input_len       = 0;
                            focused->fb.input_cursor    = 0;
                            focused->fb.input_buf[0]    = '\0';
                            changed = true;
                        } else if (ch == KEY_LEFT) {
                            if (kbd_ctrl_down()) {
                                /* Ctrl+Left: jump over word boundary */
                                int _ic = focused->fb.input_cursor;
                                while (_ic > 0) {
                                    uint8_t _c = (uint8_t)focused->fb.input_buf[_ic - 1];
                                    bool _ia = (_c>='a'&&_c<='z')||(_c>='A'&&_c<='Z')||(_c>='0'&&_c<='9')||_c=='_'||_c=='-'||_c=='.';
                                    if (_ia) break;
                                    _ic--;
                                }
                                while (_ic > 0) {
                                    uint8_t _c = (uint8_t)focused->fb.input_buf[_ic - 1];
                                    bool _ia = (_c>='a'&&_c<='z')||(_c>='A'&&_c<='Z')||(_c>='0'&&_c<='9')||_c=='_'||_c=='-'||_c=='.';
                                    if (!_ia) break;
                                    _ic--;
                                }
                                focused->fb.input_cursor = _ic;
                            } else if (focused->fb.input_cursor > 0) {
                                focused->fb.input_cursor--;
                            }
                            changed = true;
                        } else if (ch == KEY_RIGHT) {
                            if (kbd_ctrl_down()) {
                                /* Ctrl+Right: jump over word boundary */
                                int _ic = focused->fb.input_cursor;
                                int _il = focused->fb.input_len;
                                while (_ic < _il) {
                                    uint8_t _c = (uint8_t)focused->fb.input_buf[_ic];
                                    bool _ia = (_c>='a'&&_c<='z')||(_c>='A'&&_c<='Z')||(_c>='0'&&_c<='9')||_c=='_'||_c=='-'||_c=='.';
                                    if (_ia) break;
                                    _ic++;
                                }
                                while (_ic < _il) {
                                    uint8_t _c = (uint8_t)focused->fb.input_buf[_ic];
                                    bool _ia = (_c>='a'&&_c<='z')||(_c>='A'&&_c<='Z')||(_c>='0'&&_c<='9')||_c=='_'||_c=='-'||_c=='.';
                                    if (!_ia) break;
                                    _ic++;
                                }
                                focused->fb.input_cursor = _ic;
                            } else if (focused->fb.input_cursor < focused->fb.input_len) {
                                focused->fb.input_cursor++;
                            }
                            changed = true;
                        } else if (ch == KEY_HOME) {
                            if (focused->fb.input_cursor != 0) { focused->fb.input_cursor = 0; changed = true; }
                        } else if (ch == KEY_END) {
                            if (focused->fb.input_cursor != focused->fb.input_len) { focused->fb.input_cursor = focused->fb.input_len; changed = true; }
                        } else if ((ch == '\b' || ch == 127) && focused->fb.input_cursor > 0) {
                            int _ic = focused->fb.input_cursor;
                            for (int _k = _ic - 1; _k < focused->fb.input_len - 1; _k++)
                                focused->fb.input_buf[_k] = focused->fb.input_buf[_k + 1];
                            focused->fb.input_buf[--focused->fb.input_len] = '\0';
                            focused->fb.input_cursor--;
                            changed = true;
                        } else if (ch == KEY_DELETE && focused->fb.input_cursor < focused->fb.input_len) {
                            int _ic = focused->fb.input_cursor;
                            for (int _k = _ic; _k < focused->fb.input_len - 1; _k++)
                                focused->fb.input_buf[_k] = focused->fb.input_buf[_k + 1];
                            focused->fb.input_buf[--focused->fb.input_len] = '\0';
                            changed = true;
                        } else if (ch == 22 && g_clipboard && g_clipboard_len > 0) {
                            /* Ctrl+V: paste text clipboard into input */
                            for (uint32_t _pi = 0; _pi < g_clipboard_len && focused->fb.input_len < 127; _pi++) {
                                uint8_t _pc = g_clipboard[_pi];
                                if (_pc < 32 || _pc >= 127) continue;
                                int _ic = focused->fb.input_cursor;
                                for (int _k = focused->fb.input_len; _k > _ic; _k--)
                                    focused->fb.input_buf[_k] = focused->fb.input_buf[_k - 1];
                                focused->fb.input_buf[_ic] = (char)_pc;
                                focused->fb.input_buf[++focused->fb.input_len] = '\0';
                                focused->fb.input_cursor++;
                            }
                            changed = true;
                        } else if (ch >= 32 && ch < 127 && focused->fb.input_len < 127) {
                            int _ic = focused->fb.input_cursor;
                            for (int _k = focused->fb.input_len; _k > _ic; _k--)
                                focused->fb.input_buf[_k] = focused->fb.input_buf[_k - 1];
                            focused->fb.input_buf[_ic] = (char)ch;
                            focused->fb.input_buf[++focused->fb.input_len] = '\0';
                            focused->fb.input_cursor++;
                            changed = true;
                        }
                    } else if (focused->fb.search_active) {
                        if (ch == 27 || ch == '\t') {
                            focused->fb.search_active   = false;
                            focused->fb.search_query[0] = '\0';
                            focused->fb.search_len      = 0;
                            changed = true;
                        } else if (ch == '\r' || ch == '\n') {
                            focused->fb.search_active = false;
                            changed = true;
                        } else if ((ch == '\b' || ch == 127) && focused->fb.search_len > 0) {
                            focused->fb.search_query[--focused->fb.search_len] = '\0';
                            changed = true;
                        } else if (ch >= 32 && ch < 127 &&
                                   focused->fb.search_len < FB_SEARCH_MAX - 1) {
                            focused->fb.search_query[focused->fb.search_len++] = (char)ch;
                            focused->fb.search_query[focused->fb.search_len]   = '\0';
                            changed = true;
                        }
                    } else {
                        /* Navigation mode */
                        int n   = focused->fb.entry_count;
                        int sel = focused->fb.sel_row;
                        /* Compute visible row count from window geometry */
                        uint64_t lx2, ly2, lw2, lh2;
                        fb_list_region(focused, &lx2, &ly2, &lw2, &lh2);
                        int vis = (int)(lh2 / FB_ROW_H);
                        if (vis < 1) vis = 1;

                        if (ch == KEY_UP) {
                            if (sel < 0) sel = focused->fb.scroll;
                            else if (sel > 0) sel--;
                            if (kbd_shift_down() && focused->fb.sel_anchor >= 0) {
                                /* Shift+Up: extend range from anchor */
                                int lo = focused->fb.sel_anchor < sel ? focused->fb.sel_anchor : sel;
                                int hi = focused->fb.sel_anchor < sel ? sel : focused->fb.sel_anchor;
                                for (int _ri = 0; _ri < n; _ri++)
                                    focused->fb.multi_sel[_ri] = (_ri >= lo && _ri <= hi);
                            } else {
                                for (int _ri = 0; _ri < n; _ri++) focused->fb.multi_sel[_ri] = false;
                                focused->fb.sel_anchor = sel;
                            }
                            focused->fb.sel_row = sel;
                            if (sel < focused->fb.scroll)
                                focused->fb.scroll = sel;
                            changed = true;
                        } else if (ch == KEY_DOWN) {
                            if (sel < 0) sel = focused->fb.scroll;
                            else if (sel < n - 1) sel++;
                            if (kbd_shift_down() && focused->fb.sel_anchor >= 0) {
                                /* Shift+Down: extend range from anchor */
                                int lo = focused->fb.sel_anchor < sel ? focused->fb.sel_anchor : sel;
                                int hi = focused->fb.sel_anchor < sel ? sel : focused->fb.sel_anchor;
                                for (int _ri = 0; _ri < n; _ri++)
                                    focused->fb.multi_sel[_ri] = (_ri >= lo && _ri <= hi);
                            } else {
                                for (int _ri = 0; _ri < n; _ri++) focused->fb.multi_sel[_ri] = false;
                                focused->fb.sel_anchor = sel;
                            }
                            focused->fb.sel_row = sel;
                            if (sel >= focused->fb.scroll + vis)
                                focused->fb.scroll = sel - vis + 1;
                            changed = true;
                        } else if (ch == KEY_PGUP) {
                            focused->fb.scroll -= vis;
                            if (focused->fb.scroll < 0) focused->fb.scroll = 0;
                            if (focused->fb.sel_row >= 0) {
                                focused->fb.sel_row -= vis;
                                if (focused->fb.sel_row < 0) focused->fb.sel_row = 0;
                            }
                            changed = true;
                        } else if (ch == KEY_PGDN) {
                            focused->fb.scroll += vis;
                            if (focused->fb.scroll >= n) focused->fb.scroll = n > vis ? n - vis : 0;
                            if (focused->fb.sel_row >= 0) {
                                focused->fb.sel_row += vis;
                                if (focused->fb.sel_row >= n) focused->fb.sel_row = n - 1;
                            }
                            changed = true;
                        } else if (ch == KEY_HOME) {
                            focused->fb.scroll  = 0;
                            focused->fb.sel_row = 0;
                            changed = true;
                        } else if (ch == KEY_END) {
                            focused->fb.sel_row = n > 0 ? n - 1 : 0;
                            int last = focused->fb.sel_row;
                            focused->fb.scroll  = last - vis + 1;
                            if (focused->fb.scroll < 0) focused->fb.scroll = 0;
                            changed = true;
                        } else if (ch == '\r' || ch == '\n') {
                            if (sel >= 0 && sel < n) {
                                if (focused->fb.is_dir[sel]) {
                                    char np[256];
                                    fb_path_join(np, focused->fb.path,
                                                 focused->fb.entries[sel]);
                                    fb_navigate(&focused->fb, np);
                                    changed = true;
                                } else {
                                    const char *name = focused->fb.entries[sel];
                                    if (fb_is_viewable(name)) {
                                        char full[256];
                                        fb_path_join(full, focused->fb.path, name);
                                        text_open(&g_wins[3], full);
                                        win_show(&g_wins[3], 3);
                                        changed = false;
                                    }
                                }
                            }
                        } else if (ch == 27) {
                            int slot = (int)(focused - g_wins);
                            win_hide(focused, slot);
                            focused = NULL; closed = true; break;
                        } else if (ch == 23) { /* Ctrl+W */
                            int slot = (int)(focused - g_wins);
                            win_hide(focused, slot);
                            focused = NULL; closed = true; break;
                        } else if (ch == '/' || ch == 'f') {
                            focused->fb.search_active   = true;
                            focused->fb.search_query[0] = '\0';
                            focused->fb.search_len      = 0;
                            focused->fb.scroll          = 0;
                            changed = true;
                        } else if (ch == 'v' || ch == 'V') { /* V: toggle list/icon view */
                            focused->fb.view_mode = (focused->fb.view_mode == FB_VIEW_LIST)
                                                    ? FB_VIEW_ICONS : FB_VIEW_LIST;
                            focused->fb.scroll = 0;
                            changed = true;
                        } else if (ch == 'h' || ch == 'H') { /* H: toggle hidden files */
                            focused->fb.show_hidden = !focused->fb.show_hidden;
                            fb_load(&focused->fb, focused->fb.path);
                            changed = true;
                        } else if (ch == 14) { /* Ctrl+N: new file */
                            focused->fb.input_active = true;
                            focused->fb.input_isdir  = false;
                            focused->fb.input_len    = 0;
                            focused->fb.input_cursor = 0;
                            focused->fb.input_buf[0] = '\0';
                            changed = true;
                        } else if (ch == 4) {  /* Ctrl+D: new directory */
                            focused->fb.input_active = true;
                            focused->fb.input_isdir  = true;
                            focused->fb.input_len    = 0;
                            focused->fb.input_cursor = 0;
                            focused->fb.input_buf[0] = '\0';
                            changed = true;
                        } else if (ch == 18) { /* Ctrl+R: rename selected entry */
                            if (sel >= 0 && sel < n) {
                                const char *ename = focused->fb.entries[sel];
                                focused->fb.input_active    = true;
                                focused->fb.input_is_rename = true;
                                focused->fb.input_isdir     = false;
                                /* Pre-fill with current name */
                                int elen = 0;
                                while (ename[elen] && elen < 127) {
                                    focused->fb.input_buf[elen] = ename[elen]; elen++;
                                }
                                focused->fb.input_buf[elen]  = '\0';
                                focused->fb.input_len        = elen;
                                focused->fb.input_cursor     = elen;
                                /* Remember original name for vfs_rename */
                                /* Remember original name for vfs_rename */
                                for (int _k = 0; _k <= elen; _k++)
                                    focused->fb.input_orig[_k] = focused->fb.input_buf[_k];
                                changed = true;
                            }
                        } else if (ch == 'y' || ch == 'Y') { /* Y: copy path to clipboard */
                            int _yi = focused->fb.sel_row;
                            if (_yi >= 0 && _yi < focused->fb.entry_count) {
                                char _ypath[256];
                                fb_path_join(_ypath, focused->fb.path, focused->fb.entries[_yi]);
                                edit_set_clipboard((const uint8_t *)_ypath, (uint32_t)gui_strlen(_ypath));
                                gui_toast("Path copied", 0x0080c8a0u);
                                changed = true;
                            }
                        } else if (ch == KEY_LEFT && kbd_alt_down()) {
                            if (focused->fb.hist_depth > 0) {
                                fb_back(&focused->fb); changed = true;
                            }
                        } else if (ch == KEY_RIGHT && kbd_alt_down()) {
                            if (focused->fb.fwd_depth > 0) {
                                fb_forward(&focused->fb); changed = true;
                            }
                        } else if (ch == '\b' || ch == 127) {
                            /* Backspace: go up one directory */
                            bool can_up2 = !(focused->fb.path[0]=='/'&&focused->fb.path[1]=='\0');
                            if (can_up2) {
                                char parent[128];
                                fb_path_parent(parent, focused->fb.path);
                                fb_navigate(&focused->fb, parent);
                                changed = true;
                            }
                        } else if (ch == KEY_DELETE) {
                            /* Count multi-selected files (skip dirs) */
                            int _del_cnt = 0;
                            for (int _di = 0; _di < n; _di++)
                                if ((_di == sel || focused->fb.multi_sel[_di]) && !focused->fb.is_dir[_di])
                                    _del_cnt++;
                            if (_del_cnt > 1) {
                                /* Delete all selected files */
                                for (int _di = 0; _di < n; _di++) {
                                    if ((_di == sel || focused->fb.multi_sel[_di]) && !focused->fb.is_dir[_di]) {
                                        char dpath[256];
                                        fb_path_join(dpath, focused->fb.path, focused->fb.entries[_di]);
                                        vfs_delete(dpath);
                                    }
                                }
                                char _tbuf[24]; int _ti = 0; const char *_tp;
                                char _dn[8]; gui_itoa(_del_cnt, _dn, 8);
                                for (_tp = _dn; *_tp && _ti < 8; ) _tbuf[_ti++] = *_tp++;
                                for (_tp = " files deleted"; *_tp && _ti < 22; ) _tbuf[_ti++] = *_tp++;
                                _tbuf[_ti] = '\0';
                                gui_toast(_tbuf, 0x00e88060u);
                                fb_navigate(&focused->fb, focused->fb.path);
                                if (focused->fb.sel_row >= focused->fb.entry_count)
                                    focused->fb.sel_row = focused->fb.entry_count - 1;
                                changed = true;
                            } else if (sel >= 0 && sel < n && !focused->fb.is_dir[sel]) {
                                char dpath[256];
                                fb_path_join(dpath, focused->fb.path, focused->fb.entries[sel]);
                                vfs_delete(dpath);
                                gui_toast("File deleted", 0x00e88060u);
                                fb_navigate(&focused->fb, focused->fb.path);
                                if (focused->fb.sel_row >= focused->fb.entry_count)
                                    focused->fb.sel_row = focused->fb.entry_count - 1;
                                changed = true;
                            }
                        } else if (ch == 1) { /* Ctrl+A: select all files */
                            for (int _ai = 0; _ai < n; _ai++)
                                focused->fb.multi_sel[_ai] = !focused->fb.is_dir[_ai];
                            if (n > 0) focused->fb.sel_row = 0;
                            focused->fb.sel_anchor = 0;
                            changed = true;
                        } else if (ch == 3 || ch == 24) { /* Ctrl+C / Ctrl+X: copy/cut file */
                            if (sel >= 0 && sel < n && !focused->fb.is_dir[sel]) {
                                fb_path_join(g_fb_clip_path, focused->fb.path, focused->fb.entries[sel]);
                                g_fb_clip_is_cut = (ch == 24);
                                gui_toast(g_fb_clip_is_cut ? "Marked for move" : "File copied", 0x0080c8a0u);
                                changed = true;
                            }
                        } else if (ch == 22) { /* Ctrl+V: paste copied/cut file */
                            if (g_fb_clip_path[0]) {
                                /* Extract just the filename from the source path */
                                const char *_fn = g_fb_clip_path;
                                for (const char *_p = g_fb_clip_path; *_p; _p++)
                                    if (*_p == '/') _fn = _p + 1;
                                char _dst[256];
                                fb_path_join(_dst, focused->fb.path, _fn);
                                if (gui_streq(g_fb_clip_path, _dst)) {
                                    gui_toast("Already here", 0x00708090u);
                                } else {
                                    const void *_data = NULL; uint64_t _sz2 = 0;
                                    int _rv = vfs_read(g_fb_clip_path, &_data, &_sz2);
                                    if (_rv == 0 && _data) {
                                        /* Copy data to temporary buffer so vfs_write doesn't alias */
                                        uint8_t *_buf = (uint8_t *)kmalloc(_sz2 + 1u);
                                        if (_buf) {
                                            for (uint64_t _bi = 0; _bi < _sz2; _bi++) _buf[_bi] = ((uint8_t *)_data)[_bi];
                                            vfs_write(_dst, _buf, _sz2);
                                            kfree(_buf);
                                            if (g_fb_clip_is_cut) {
                                                vfs_delete(g_fb_clip_path);
                                                g_fb_clip_path[0] = '\0';
                                                gui_toast("Moved", 0x0080e8b0u);
                                            } else {
                                                gui_toast("Pasted", 0x0080e8b0u);
                                            }
                                        } else {
                                            gui_toast("Out of memory", 0x00e08060u);
                                        }
                                    } else {
                                        gui_toast("Read failed", 0x00e08060u);
                                        g_fb_clip_path[0] = '\0';
                                    }
                                    fb_navigate(&focused->fb, focused->fb.path);
                                    changed = true;
                                }
                            } else {
                                gui_toast("Nothing to paste", 0x00708090u);
                                changed = true;
                            }
                        } else if (ch == 5) { /* Ctrl+E: open selected file in edit mode */
                            if (sel >= 0 && sel < n && !focused->fb.is_dir[sel]) {
                                char ep3[256];
                                fb_path_join(ep3, focused->fb.path, focused->fb.entries[sel]);
                                text_open(&g_wins[3], ep3);
                                win_show(&g_wins[3], 3);
                                text_enter_edit(&g_wins[3]);
                                if (g_wins[3].text.edit_mode) gui_toast("Edit mode", 0x0080c8a0u);
                                changed = false;
                            }
                        } else if (ch >= 32 && ch < 127) {
                            /* Any printable key: jump to first matching entry */
                            char lc = (char)ch;
                            if (lc >= 'A' && lc <= 'Z') lc += 32;
                            for (int ji = 0; ji < focused->fb.entry_count; ji++) {
                                char fc = focused->fb.entries[ji][0];
                                if (fc >= 'A' && fc <= 'Z') fc += 32;
                                if (fc == lc) {
                                    focused->fb.sel_row = ji;
                                    uint64_t jlx, jly, jlw, jlh;
                                    fb_list_region(focused, &jlx, &jly, &jlw, &jlh);
                                    int jvis = (int)(jlh / FB_ROW_H);
                                    if (jvis < 1) jvis = 1;
                                    if (ji < focused->fb.scroll)
                                        focused->fb.scroll = ji;
                                    else if (ji >= focused->fb.scroll + jvis)
                                        focused->fb.scroll = ji - jvis + 1;
                                    changed = true;
                                    break;
                                }
                            }
                        }
                    }
                } else if (focused->type == WIN_TEXT) {
                    text_state_t *ts = &focused->text;
                    if (ts->save_as_active) {
                        /* Save-as path input */
                        if (ch == 27 || ch == 23) { /* ESC / Ctrl+W: cancel */
                            ts->save_as_active = false;
                            changed = true;
                        } else if (ch == '\r' || ch == '\n') { /* Enter: confirm save */
                            if (ts->save_as_len > 0) {
                                for (int _si = 0; _si < ts->save_as_len && _si < TV_PATH_MAX-1; _si++)
                                    ts->path[_si] = ts->save_as_buf[_si];
                                ts->path[ts->save_as_len < TV_PATH_MAX-1 ? ts->save_as_len : TV_PATH_MAX-1] = '\0';
                                ts->lang = detect_lang(ts->path);
                                /* Update title bar */
                                const char *_base = ts->path;
                                for (const char *_p = ts->path; *_p; _p++) if (*_p=='/') _base=_p+1;
                                int _ti = 0;
                                while (_base[_ti] && _ti < 50) { ts->title_buf[_ti] = _base[_ti]; _ti++; }
                                ts->title_buf[_ti] = '*'; ts->title_buf[_ti+1] = '\0';
                                focused->title = ts->title_buf;
                                ts->save_as_active = false;
                                text_save(focused);
                            }
                            changed = true;
                        } else if (ch == '\t') { /* Tab: path completion */
                            char *sbuf = ts->save_as_buf;
                            int slen = ts->save_as_len;
                            /* Split into dir part and file prefix */
                            int last_slash = -1;
                            for (int _k = 0; _k < slen; _k++)
                                if (sbuf[_k] == '/') last_slash = _k;
                            char _dir[128], _pfx[128];
                            if (last_slash < 0) {
                                _dir[0] = '/'; _dir[1] = '\0';
                                for (int _k = 0; _k < slen && _k < 127; _k++) _pfx[_k] = sbuf[_k];
                                _pfx[slen < 127 ? slen : 127] = '\0';
                            } else {
                                int dl = last_slash + 1;
                                if (dl > 127) dl = 127;
                                for (int _k = 0; _k < dl; _k++) _dir[_k] = sbuf[_k];
                                _dir[dl] = '\0';
                                int pl = slen - last_slash - 1;
                                if (pl < 0) pl = 0;
                                for (int _k = 0; _k < pl && _k < 127; _k++) _pfx[_k] = sbuf[last_slash + 1 + _k];
                                _pfx[pl < 127 ? pl : 127] = '\0';
                            }
                            /* List directory and find matches */
                            static char _lbuf[2048];
                            size_t _lsz = vfs_listdir(_dir, _lbuf, sizeof(_lbuf));
                            (void)_lsz;
                            int _pfxlen = 0; while (_pfx[_pfxlen]) _pfxlen++;
                            /* Collect up to 16 matches */
                            const char *_matches[16]; int _nm = 0;
                            const char *_lp = _lbuf;
                            while (*_lp && _nm < 16) {
                                /* Each entry is a name followed by '\n' */
                                const char *_end = _lp;
                                while (*_end && *_end != '\n') _end++;
                                int _elen = (int)(_end - _lp);
                                /* Compare prefix case-sensitively */
                                bool _match = (_elen >= _pfxlen);
                                for (int _k = 0; _match && _k < _pfxlen; _k++)
                                    if (_lp[_k] != _pfx[_k]) _match = false;
                                if (_match) { _matches[_nm++] = _lp; }
                                _lp = *_end ? _end + 1 : _end;
                            }
                            if (_nm == 1) {
                                /* Single match: complete fully */
                                const char *_m = _matches[0];
                                const char *_me = _m; while (*_me && *_me != '\n') _me++;
                                /* Rebuild path: dir + matched name */
                                int dl = 0; while (_dir[dl]) dl++;
                                /* Remove trailing slash if dir isn't root */
                                int pl2 = dl;
                                if (pl2 > 1 && _dir[pl2-1] == '/') pl2--;
                                char _new[TV_PATH_MAX];
                                int _ni = 0;
                                for (int _k = 0; _k < pl2 && _ni < TV_PATH_MAX-2; _k++) _new[_ni++] = _dir[_k];
                                _new[_ni++] = '/';
                                for (const char *_k2 = _m; _k2 < _me && _ni < TV_PATH_MAX-2; _k2++) _new[_ni++] = *_k2;
                                _new[_ni] = '\0';
                                /* Check if result is a directory → add trailing slash */
                                if (vfs_isdir(_new)) { if (_ni < TV_PATH_MAX-2) { _new[_ni++]='/'; _new[_ni]='\0'; } }
                                for (int _k = 0; _k <= _ni && _k < TV_PATH_MAX-1; _k++) sbuf[_k] = _new[_k];
                                ts->save_as_len = _ni;
                            } else if (_nm > 1) {
                                /* Multiple: complete common prefix */
                                int _cp = 0;
                                const char *_m0 = _matches[0];
                                while (true) {
                                    char _c0 = '\0';
                                    const char *_me2 = _m0 + _cp;
                                    if (!*_me2 || *_me2 == '\n') break;
                                    _c0 = *_me2;
                                    bool _all = true;
                                    for (int _mi = 1; _mi < _nm; _mi++) {
                                        char _ci = _matches[_mi][_cp];
                                        if (!_ci || _ci == '\n' || _ci != _c0) { _all = false; break; }
                                    }
                                    if (!_all) break;
                                    _cp++;
                                }
                                /* Rebuild with common prefix */
                                int dl = 0; while (_dir[dl]) dl++;
                                int pl2 = dl;
                                if (pl2 > 1 && _dir[pl2-1] == '/') pl2--;
                                char _new[TV_PATH_MAX];
                                int _ni = 0;
                                for (int _k = 0; _k < pl2 && _ni < TV_PATH_MAX-2; _k++) _new[_ni++] = _dir[_k];
                                _new[_ni++] = '/';
                                for (int _k = 0; _k < _cp && _ni < TV_PATH_MAX-2; _k++) _new[_ni++] = _matches[0][_k];
                                _new[_ni] = '\0';
                                for (int _k = 0; _k <= _ni && _k < TV_PATH_MAX-1; _k++) sbuf[_k] = _new[_k];
                                ts->save_as_len = _ni;
                            }
                            changed = true;
                        } else if (ch == 8 || ch == 127) { /* Backspace */
                            if (ts->save_as_len > 0) ts->save_as_buf[--ts->save_as_len] = '\0';
                            changed = true;
                        } else if (ch >= 32 && ch < 127 && ts->save_as_len < TV_PATH_MAX - 2) {
                            ts->save_as_buf[ts->save_as_len++] = (char)ch;
                            ts->save_as_buf[ts->save_as_len]   = '\0';
                            changed = true;
                        }
                    } else if (ts->open_bar_active) {
                        /* Open-by-path bar input (Ctrl+O) */
                        if (ch == 27 || ch == 23) { /* ESC / Ctrl+W: cancel */
                            ts->open_bar_active = false;
                            changed = true;
                        } else if (ch == '\r' || ch == '\n') { /* Enter: open */
                            if (ts->open_bar_len > 0) {
                                char _op[TV_PATH_MAX];
                                for (int _oi = 0; _oi < ts->open_bar_len && _oi < TV_PATH_MAX-1; _oi++)
                                    _op[_oi] = ts->open_bar_buf[_oi];
                                _op[ts->open_bar_len < TV_PATH_MAX-1 ? ts->open_bar_len : TV_PATH_MAX-1] = '\0';
                                ts->open_bar_active = false;
                                text_open(focused, _op);
                            } else {
                                ts->open_bar_active = false;
                            }
                            changed = true;
                        } else if (ch == '\t') { /* Tab: path completion */
                            char *_obuf = ts->open_bar_buf;
                            int _olen = ts->open_bar_len;
                            int _slash = -1;
                            for (int _k = 0; _k < _olen; _k++) if (_obuf[_k] == '/') _slash = _k;
                            char _dir[128], _pfx[128];
                            if (_slash < 0) {
                                _dir[0]='/'; _dir[1]='\0';
                                for (int _k=0; _k<_olen && _k<127; _k++) _pfx[_k]=_obuf[_k];
                                _pfx[_olen < 127 ? _olen : 127] = '\0';
                            } else {
                                int _dl = _slash + 1; if (_dl > 127) _dl = 127;
                                for (int _k=0; _k<_dl; _k++) _dir[_k]=_obuf[_k]; _dir[_dl]='\0';
                                int _pl = _olen - _slash - 1;
                                for (int _k=0; _k<_pl && _k<127; _k++) _pfx[_k]=_obuf[_slash+1+_k];
                                _pfx[_pl < 127 ? _pl : 127] = '\0';
                            }
                            static char _olbuf[2048];
                            size_t _olsz = vfs_listdir(_dir, _olbuf, sizeof(_olbuf));
                            _olbuf[_olsz < sizeof(_olbuf)-1 ? _olsz : sizeof(_olbuf)-1] = '\0';
                            int _pfxl = (int)gui_strlen(_pfx);
                            char _matches[16][128]; int _nm = 0;
                            char *_op2 = _olbuf, *_oend = _olbuf + _olsz;
                            while (_op2 < _oend && _nm < 16) {
                                char *_onl = _op2;
                                while (_onl < _oend && *_onl != '\n') _onl++;
                                int _en = (int)(_onl - _op2);
                                if (_en > 0 && _en < 127) {
                                    bool _om = true;
                                    for (int _k=0; _k<_pfxl && _om; _k++)
                                        if (_op2[_k] != _pfx[_k]) _om = false;
                                    if (_om) {
                                        for (int _k=0; _k<_en; _k++) _matches[_nm][_k]=_op2[_k];
                                        _matches[_nm][_en]='\0'; _nm++;
                                    }
                                }
                                _op2 = _onl + 1;
                            }
                            if (_nm == 1) {
                                char _new[TV_PATH_MAX]; int _ni = 0;
                                for (int _k=0; _dir[_k] && _ni < TV_PATH_MAX-2; _k++) _new[_ni++]=_dir[_k];
                                /* Remove trailing slash duplication */
                                if (_ni > 1 && _new[_ni-1]=='/') _ni--;
                                _new[_ni++]='/';
                                for (int _k=0; _matches[0][_k] && _ni < TV_PATH_MAX-2; _k++) _new[_ni++]=_matches[0][_k];
                                _new[_ni]='\0';
                                /* Append '/' if it's a directory */
                                char _chkpath[256]; int _cp2=0;
                                for (int _k=0; _new[_k] && _cp2<254; _k++) _chkpath[_cp2++]=_new[_k];
                                _chkpath[_cp2]='\0';
                                if (vfs_isdir(_chkpath) == 1 && _ni < TV_PATH_MAX-2) {
                                    _new[_ni++]='/'; _new[_ni]='\0';
                                }
                                for (int _k=0; _k<=_ni && _k<TV_PATH_MAX-1; _k++) _obuf[_k]=_new[_k];
                                ts->open_bar_len = _ni;
                            } else if (_nm > 1) {
                                int _cp = (int)gui_strlen(_matches[0]);
                                for (int _mi=1; _mi<_nm; _mi++) {
                                    int _ml=(int)gui_strlen(_matches[_mi]);
                                    if (_ml < _cp) _cp = _ml;
                                    for (int _k=0; _k<_cp; _k++)
                                        if (_matches[0][_k]!=_matches[_mi][_k]) { _cp=_k; break; }
                                }
                                if (_cp > _pfxl) {
                                    char _new[TV_PATH_MAX]; int _ni=0;
                                    for (int _k=0; _dir[_k] && _ni<TV_PATH_MAX-2; _k++) _new[_ni++]=_dir[_k];
                                    if (_ni > 1 && _new[_ni-1]=='/') _ni--;
                                    _new[_ni++]='/';
                                    for (int _k=0; _k<_cp && _ni<TV_PATH_MAX-2; _k++) _new[_ni++]=_matches[0][_k];
                                    _new[_ni]='\0';
                                    for (int _k=0; _k<=_ni && _k<TV_PATH_MAX-1; _k++) _obuf[_k]=_new[_k];
                                    ts->open_bar_len = _ni;
                                }
                            }
                            changed = true;
                        } else if (ch == 8 || ch == 127) { /* Backspace */
                            if (ts->open_bar_len > 0) ts->open_bar_buf[--ts->open_bar_len] = '\0';
                            changed = true;
                        } else if (ch >= 32 && ch < 127 && ts->open_bar_len < TV_PATH_MAX - 2) {
                            ts->open_bar_buf[ts->open_bar_len++] = (char)ch;
                            ts->open_bar_buf[ts->open_bar_len]   = '\0';
                            changed = true;
                        }
                    } else if (ts->srch_active) {
                        /* Search / goto / replace bar input */
                        if (ch == 27) { /* ESC: close */
                            ts->srch_active = false; ts->srch_is_repl = false;
                            ts->repl_focused = false; ts->srch_match_line = -1;
                            changed = true;
                        } else if (ch == '\t' && ts->srch_is_repl) { /* Tab: switch field */
                            ts->repl_focused = !ts->repl_focused;
                            changed = true;
                        } else if (ch == '\t' && !ts->srch_is_repl && !ts->srch_is_goto) {
                            ts->srch_case_fold = !ts->srch_case_fold;
                            if (ts->srch_len > 0) text_search_next(focused, false);
                            changed = true;
                        } else if (ch == '\r') { /* Enter / Shift+Enter */
                            bool _sh = kbd_shift_down();
                            if (ts->srch_is_goto) {
                                int target = 0;
                                for (int k = 0; k < ts->srch_len; k++)
                                    if (ts->srch_buf[k] >= '0' && ts->srch_buf[k] <= '9')
                                        target = target * 10 + (ts->srch_buf[k] - '0');
                                if (target > 0) target--;
                                if (target < 0) target = 0;
                                if (target >= ts->total_lines) target = ts->total_lines - 1;
                                ts->scroll = target > 3 ? target - 3 : 0;
                                if (ts->edit_mode && ts->edit_buf) {
                                    ts->edit_cur = 0; int _ln = 0;
                                    while (ts->edit_cur < ts->edit_size && _ln < target) {
                                        if (ts->edit_buf[ts->edit_cur] == '\n') _ln++;
                                        ts->edit_cur++;
                                    }
                                    edit_sel_clear(ts); edit_sync_pos(ts); ts->edit_want_col = 0;
                                }
                                ts->srch_active = false;
                            } else if (ts->srch_is_repl && ts->repl_focused && !_sh) {
                                /* Replace current match + find next */
                                text_replace_one(focused);
                                edit_recount(focused);
                            } else if (_sh) {
                                /* Shift+Enter: previous match */
                                text_search_prev(focused);
                            } else {
                                text_search_next(focused, true);
                            }
                            changed = true;
                        } else if (ch == 1 && ts->srch_is_repl) { /* Ctrl+A: replace all */
                            int n = text_replace_all_impl(focused);
                            edit_recount(focused);
                            char _tb[24]; char _tn[12]; int _ti = 0; const char *_tp;
                            gui_itoa(n, _tn, 12);
                            for (_tp="Replaced "; *_tp && _ti<22; ) _tb[_ti++]=*_tp++;
                            for (_tp=_tn; *_tp && _ti<22; ) _tb[_ti++]=*_tp++;
                            _tb[_ti]='\0';
                            gui_toast(_tb, 0x0080c8a0u);
                            changed = true;
                        } else if (ch == 22) { /* Ctrl+V: paste into active field */
                            if (g_clipboard && g_clipboard_len > 0) {
                                bool _rf = ts->srch_is_repl && ts->repl_focused;
                                char *_b = _rf ? ts->repl_buf : ts->srch_buf;
                                int  *_l = _rf ? &ts->repl_len : &ts->srch_len;
                                bool _gt = ts->srch_is_goto && !_rf;
                                for (uint32_t _pi = 0; _pi < g_clipboard_len && *_l < 63; _pi++) {
                                    uint8_t _pc = g_clipboard[_pi];
                                    if (_pc < 32 || _pc >= 127) continue;
                                    if (_gt && !(_pc >= '0' && _pc <= '9')) continue;
                                    _b[(*_l)++] = (char)_pc;
                                }
                                _b[*_l] = '\0';
                                if (!ts->srch_is_goto) text_search_next(focused, false);
                                changed = true;
                            }
                        } else if (ch == 14) { /* Ctrl+N: find next */
                            if (!ts->srch_is_goto) { text_search_next(focused, true); changed = true; }
                        } else if (ch == 8 || ch == 127) { /* Backspace */
                            if (ts->srch_is_repl && ts->repl_focused) {
                                if (ts->repl_len > 0) ts->repl_buf[--ts->repl_len] = '\0';
                            } else {
                                if (ts->srch_len > 0) {
                                    ts->srch_buf[--ts->srch_len] = '\0';
                                    if (!ts->srch_is_goto) text_search_next(focused, false);
                                }
                            }
                            changed = true;
                        } else if (ch >= 32 && ch < 127) {
                            if (ts->srch_is_repl && ts->repl_focused) {
                                if (ts->repl_len < 63) {
                                    ts->repl_buf[ts->repl_len++] = (char)ch;
                                    ts->repl_buf[ts->repl_len]   = '\0';
                                }
                            } else if (ts->srch_len < 63) {
                                if (!ts->srch_is_goto || (ch >= '0' && ch <= '9')) {
                                    ts->srch_buf[ts->srch_len++] = (char)ch;
                                    ts->srch_buf[ts->srch_len]   = '\0';
                                    if (!ts->srch_is_goto) text_search_next(focused, false);
                                }
                            }
                            changed = true;
                        }
                    } else if (ts->edit_mode) {
                        /* ── Edit mode input ── */
                        if (ch == 27) { /* ESC: exit edit mode (auto-saves if modified) */
                            ts->undo_in_group = false;
                            if (ts->edit_modified) text_save(focused);
                            text_exit_edit(focused);
                            edit_recount(focused);
                            changed = true;
                        } else if (ch == 19) { /* Ctrl+S: save (or save-as if no path) */
                            ts->undo_in_group = false;
                            if (!ts->path[0]) {
                                /* No path set — open save-as bar */
                                ts->srch_active    = false;
                                ts->save_as_active = true;
                                ts->save_as_len    = 0;
                                ts->save_as_buf[0] = '\0';
                            } else if (ts->edit_modified) {
                                text_save(focused);
                                gui_toast("Saved", 0x0080c8a0u);
                            } else {
                                gui_toast("No changes", 0x00708090u);
                            }
                            changed = true;
                        } else if (ch == 15) { /* Ctrl+O: open file by path */
                            ts->undo_in_group = false;
                            ts->srch_active    = false;
                            ts->save_as_active = false;
                            ts->save_as_active = false;
                            ts->open_bar_active = true;
                            ts->open_bar_len    = 0;
                            ts->open_bar_buf[0] = '\0';
                            changed = true;
                        } else if (ch == 23) { /* Ctrl+W: save + close */
                            if (ts->edit_modified) text_save(focused);
                            text_exit_edit(focused);
                            int slot2 = (int)(focused - g_wins);
                            win_hide(focused, slot2);
                            focused = NULL; closed = true;
                        } else if (ch == 6) { /* Ctrl+F: open search (also in edit mode) */
                            ts->undo_in_group = false;
                            ts->save_as_active  = false;
                            ts->open_bar_active = false;
                            ts->srch_active  = true;
                            ts->srch_is_goto = false;
                            ts->srch_is_repl = false;
                            ts->repl_focused = false;
                            ts->srch_len     = 0;
                            ts->srch_buf[0]  = '\0';
                            ts->srch_match_line = -1;
                            changed = true;
                        } else if (ch == 7) { /* Ctrl+G: goto line (edit mode) */
                            ts->undo_in_group = false;
                            ts->save_as_active  = false;
                            ts->open_bar_active = false;
                            ts->srch_active  = true;
                            ts->srch_is_goto = true;
                            ts->srch_is_repl = false;
                            ts->srch_len     = 0;
                            ts->srch_buf[0]  = '\0';
                            changed = true;
                        } else if (ch == 18) { /* Ctrl+R: find & replace (edit mode) */
                            ts->undo_in_group = false;
                            ts->save_as_active = false;
                            ts->srch_active  = true;
                            ts->srch_is_goto = false;
                            ts->srch_is_repl = true;
                            ts->repl_focused = false;
                            ts->srch_len     = 0;
                            ts->srch_buf[0]  = '\0';
                            ts->repl_len     = 0;
                            ts->repl_buf[0]  = '\0';
                            ts->srch_match_line = -1;
                            changed = true;
                        } else if (ch == 12) { /* Ctrl+L: center cursor on screen */
                            ts->undo_in_group = false;
                            uint64_t _fh2 = console_font_height();
                            uint64_t _tvsh = _fh2 + 4u;
                            uint64_t _sbh  = ts->srch_active
                                             ? (ts->srch_is_repl ? 2u*(_fh2+8u) : _fh2+8u) : 0u;
                            uint64_t _sah  = (ts->save_as_active || ts->open_bar_active) ? (_fh2 + 8u) : 0u;
                            uint64_t _ih2  = focused->h > TITLE_H + BORDER
                                             ? focused->h - TITLE_H - BORDER : 1u;
                            uint64_t _iht  = _ih2 > _tvsh + _sbh + _sah ? _ih2 - _tvsh - _sbh - _sah : 1u;
                            int _mr = (int)((_iht > 2u*PAD ? _iht - 2u*PAD : 1u) / _fh2);
                            int _sc = ts->edit_cur_line - _mr / 2;
                            if (_sc < 0) _sc = 0;
                            ts->scroll = _sc;
                            changed = true;
                        } else if (ch == 2 && ts->path[0]) { /* Ctrl+B: reveal in Files */
                            ts->undo_in_group = false;
                            char _rdir2[128];
                            fb_path_parent(_rdir2, ts->path);
                            if (!g_wins[1].active || g_wins[1].state == WIN_HIDDEN)
                                win_show(&g_wins[1], 1);
                            if (!gui_streq(g_wins[1].fb.path, _rdir2))
                                fb_navigate(&g_wins[1].fb, _rdir2);
                            const char *_fn2 = ts->path;
                            for (const char *_p2 = ts->path; *_p2; _p2++)
                                if (*_p2 == '/') _fn2 = _p2 + 1;
                            for (int _fi2 = 0; _fi2 < g_wins[1].fb.entry_count; _fi2++) {
                                if (gui_streq(g_wins[1].fb.entries[_fi2], _fn2)) {
                                    g_wins[1].fb.sel_row = _fi2; break;
                                }
                            }
                            raise_win(1);
                            full_redraw();
                            focused = NULL; closed = true; break;
                        } else if (ch == 14) { /* Ctrl+N: find next (edit mode) */
                            if (ts->srch_len > 0) { text_search_next(focused, true); changed = true; }
                        } else if (ch == 1) { /* Ctrl+A: select all */
                            ts->undo_in_group = false;
                            ts->sel_anchor = 0;
                            ts->sel_end    = (int32_t)ts->edit_size;
                            changed = true;
                        } else if (ch == 3) { /* Ctrl+C: copy selection (or current line) */
                            ts->undo_in_group = false;
                            if (ts->sel_anchor >= 0 && ts->sel_anchor != ts->sel_end) {
                                edit_copy_to_clip(ts);
                                gui_toast("Copied", 0x0080c8a0u);
                            } else if (ts->edit_buf) {
                                /* No selection: copy current line including \n */
                                uint32_t _cl = ts->edit_cur;
                                while (_cl > 0 && ts->edit_buf[_cl-1] != '\n') _cl--;
                                uint32_t _ce = _cl;
                                while (_ce < ts->edit_size && ts->edit_buf[_ce] != '\n') _ce++;
                                if (_ce < ts->edit_size) _ce++;
                                uint32_t _len = _ce - _cl;
                                if (_len > 0) {
                                    edit_set_clipboard(ts->edit_buf + _cl, _len);
                                    gui_toast("Line copied", 0x0080c8a0u);
                                }
                            }
                            changed = true;
                        } else if (ch == 24) { /* Ctrl+X: cut selection */
                            ts->undo_in_group = false;
                            edit_push_undo(ts);
                            edit_copy_to_clip(ts);
                            edit_delete_selection(ts);
                            edit_recount(focused);
                            edit_scroll_to_cursor(focused);
                            gui_toast("Cut", 0x0080c8a0u);
                            changed = true;
                        } else if (ch == 22) { /* Ctrl+V: paste */
                            ts->undo_in_group = false;
                            edit_push_undo(ts);
                            if (g_clipboard && g_clipboard_len > 0) {
                                edit_paste(focused);
                                gui_toast("Pasted", 0x0080c8a0u);
                            } else {
                                gui_toast("Clipboard empty", 0x00708090u);
                            }
                            edit_recount(focused);
                            edit_scroll_to_cursor(focused);
                            changed = true;
                        } else if (ch == KEY_UP) {
                            ts->undo_in_group = false;
                            if (kbd_alt_down()) {
                                edit_push_undo(ts);
                                edit_move_line_up(focused);
                                edit_recount(focused);
                            } else {
                                bool sh = kbd_shift_down();
                                if (!sh) edit_sel_clear(ts);
                                else if (ts->sel_anchor < 0) ts->sel_anchor = (int32_t)ts->edit_cur;
                                edit_move_up(ts);
                                if (sh) ts->sel_end = (int32_t)ts->edit_cur;
                            }
                            edit_scroll_to_cursor(focused); changed = true;
                        } else if (ch == KEY_DOWN) {
                            ts->undo_in_group = false;
                            if (kbd_alt_down()) {
                                edit_push_undo(ts);
                                edit_move_line_down(focused);
                                edit_recount(focused);
                            } else {
                                bool sh = kbd_shift_down();
                                if (!sh) edit_sel_clear(ts);
                                else if (ts->sel_anchor < 0) ts->sel_anchor = (int32_t)ts->edit_cur;
                                edit_move_down(ts);
                                if (sh) ts->sel_end = (int32_t)ts->edit_cur;
                            }
                            edit_scroll_to_cursor(focused); changed = true;
                        } else if (ch == KEY_LEFT) {
                            ts->undo_in_group = false;
                            bool sh = kbd_shift_down();
                            bool ctrl = kbd_ctrl_down();
                            if (!sh) edit_sel_clear(ts);
                            else if (ts->sel_anchor < 0) ts->sel_anchor = (int32_t)ts->edit_cur;
                            if (ctrl) edit_move_word_left(ts); else edit_move_left(ts);
                            if (sh) ts->sel_end = (int32_t)ts->edit_cur;
                            edit_scroll_to_cursor(focused); changed = true;
                        } else if (ch == KEY_RIGHT) {
                            ts->undo_in_group = false;
                            bool sh = kbd_shift_down();
                            bool ctrl = kbd_ctrl_down();
                            if (!sh) edit_sel_clear(ts);
                            else if (ts->sel_anchor < 0) ts->sel_anchor = (int32_t)ts->edit_cur;
                            if (ctrl) edit_move_word_right(ts); else edit_move_right(ts);
                            if (sh) ts->sel_end = (int32_t)ts->edit_cur;
                            edit_scroll_to_cursor(focused); changed = true;
                        } else if (ch == KEY_HOME) {
                            ts->undo_in_group = false;
                            bool sh = kbd_shift_down();
                            if (!sh) edit_sel_clear(ts);
                            else if (ts->sel_anchor < 0) ts->sel_anchor = (int32_t)ts->edit_cur;
                            if (kbd_ctrl_down()) { /* Ctrl+Home: file start */
                                ts->edit_cur = 0;
                                edit_sync_pos(ts); ts->edit_want_col = 0;
                            } else {
                                /* Smart home: first non-ws col, then col 0 on second press */
                                uint32_t _ls = ts->edit_cur;
                                while (_ls > 0 && ts->edit_buf[_ls-1] != '\n') _ls--;
                                uint32_t _ind = _ls;
                                while (_ind < ts->edit_size &&
                                       (ts->edit_buf[_ind]==' '||ts->edit_buf[_ind]=='\t') &&
                                       ts->edit_buf[_ind] != '\n') _ind++;
                                if (ts->edit_cur != _ind && _ind > _ls)
                                    ts->edit_cur = _ind;
                                else
                                    ts->edit_cur = _ls;
                                edit_sync_pos(ts);
                                ts->edit_want_col = (uint32_t)ts->edit_cur_col;
                            }
                            if (sh) ts->sel_end = (int32_t)ts->edit_cur;
                            edit_scroll_to_cursor(focused); changed = true;
                        } else if (ch == KEY_END) {
                            ts->undo_in_group = false;
                            bool sh = kbd_shift_down();
                            if (!sh) edit_sel_clear(ts);
                            else if (ts->sel_anchor < 0) ts->sel_anchor = (int32_t)ts->edit_cur;
                            if (kbd_ctrl_down()) { /* Ctrl+End: file end */
                                ts->edit_cur = ts->edit_size;
                                edit_sync_pos(ts);
                                ts->edit_want_col = (uint32_t)ts->edit_cur_col;
                            } else {
                                edit_move_end(ts);
                            }
                            if (sh) ts->sel_end = (int32_t)ts->edit_cur;
                            edit_scroll_to_cursor(focused); changed = true;
                        } else if (ch == KEY_PGUP) {
                            ts->undo_in_group = false;
                            bool sh = kbd_shift_down();
                            if (!sh) edit_sel_clear(ts);
                            else if (ts->sel_anchor < 0) ts->sel_anchor = (int32_t)ts->edit_cur;
                            {
                                uint64_t fh3 = console_font_height();
                                uint64_t ih3 = focused->h - TITLE_H - BORDER;
                                int page = ih3 > 2u*PAD+fh3 ? (int)((ih3-2u*PAD)/fh3)-1 : 1;
                                if (page < 1) page = 1;
                                for (int pi = 0; pi < page; pi++) edit_move_up(ts);
                            }
                            if (sh) ts->sel_end = (int32_t)ts->edit_cur;
                            edit_scroll_to_cursor(focused); changed = true;
                        } else if (ch == KEY_PGDN) {
                            ts->undo_in_group = false;
                            bool sh = kbd_shift_down();
                            if (!sh) edit_sel_clear(ts);
                            else if (ts->sel_anchor < 0) ts->sel_anchor = (int32_t)ts->edit_cur;
                            {
                                uint64_t fh3 = console_font_height();
                                uint64_t ih3 = focused->h - TITLE_H - BORDER;
                                int page = ih3 > 2u*PAD+fh3 ? (int)((ih3-2u*PAD)/fh3)-1 : 1;
                                if (page < 1) page = 1;
                                for (int pi = 0; pi < page; pi++) edit_move_down(ts);
                            }
                            if (sh) ts->sel_end = (int32_t)ts->edit_cur;
                            edit_scroll_to_cursor(focused); changed = true;
                        } else if (ch == 26) { /* Ctrl+Z: undo */
                            ts->undo_in_group = false;
                            if (ts->undo_count > 0) {
                                edit_pop_undo(focused);
                                edit_recount(focused);
                                edit_scroll_to_cursor(focused);
                            } else {
                                gui_toast("Nothing to undo", 0x00708090u);
                            }
                            changed = true;
                        } else if (ch == 25) { /* Ctrl+Y: redo */
                            ts->undo_in_group = false;
                            if (ts->redo_count > 0) {
                                edit_pop_redo(focused);
                                edit_recount(focused);
                                edit_scroll_to_cursor(focused);
                            } else {
                                gui_toast("Nothing to redo", 0x00708090u);
                            }
                            changed = true;
                        } else if (ch == '\b' || ch == 127) { /* Backspace / Ctrl+Backspace */
                            ts->undo_in_group = false;
                            edit_push_undo(ts);
                            if (!edit_delete_selection(ts)) {
                                if (kbd_ctrl_down()) edit_del_word_before(ts);
                                else edit_del_before(ts);
                            }
                            edit_recount(focused);
                            edit_scroll_to_cursor(focused); changed = true;
                        } else if (ch == KEY_DELETE) { /* Delete / Ctrl+Delete */
                            ts->undo_in_group = false;
                            edit_push_undo(ts);
                            if (!edit_delete_selection(ts)) {
                                if (kbd_ctrl_down()) edit_del_word_at(ts);
                                else edit_del_at(ts);
                            }
                            edit_recount(focused);
                            edit_scroll_to_cursor(focused); changed = true;
                        } else if (ch == 4) { /* Ctrl+D: duplicate current line */
                            ts->undo_in_group = false;
                            edit_push_undo(ts);
                            edit_dup_line(focused);
                            edit_recount(focused);
                            edit_scroll_to_cursor(focused); changed = true;
                        } else if (ch == 11) { /* Ctrl+K: kill to end of line */
                            ts->undo_in_group = false;
                            edit_push_undo(ts);
                            if (!edit_delete_selection(ts)) edit_kill_line(ts);
                            edit_recount(focused);
                            edit_scroll_to_cursor(focused); changed = true;
                        } else if (ch == '\r' || ch == '\n') { /* Enter */
                            ts->undo_in_group = false;
                            edit_push_undo(ts);
                            edit_delete_selection(ts);
                            /* Auto-indent: match indentation of current line */
                            uint32_t _ls = ts->edit_cur;
                            while (_ls > 0 && ts->edit_buf[_ls - 1u] != '\n') _ls--;
                            uint32_t _ind = 0;
                            while (_ls + _ind < ts->edit_cur &&
                                   (ts->edit_buf[_ls + _ind] == ' ' ||
                                    ts->edit_buf[_ls + _ind] == '\t'))
                                _ind++;
                            edit_insert(ts, '\n');
                            for (uint32_t _ii = 0; _ii < _ind; _ii++)
                                edit_insert(ts, ts->edit_buf[_ls + _ii]);
                            edit_recount(focused);
                            edit_scroll_to_cursor(focused); changed = true;
                        } else if (ch == '\t') { /* Tab / Shift+Tab */
                            ts->undo_in_group = false;
                            edit_push_undo(ts);
                            bool sh_tab = kbd_shift_down();
                            if (sh_tab || (ts->sel_anchor >= 0 && ts->sel_anchor != ts->sel_end)) {
                                /* Block indent / unindent */
                                edit_indent_block(ts, !sh_tab);
                            } else {
                                /* Plain Tab: insert 4 spaces */
                                edit_delete_selection(ts);
                                for (int ti2 = 0; ti2 < 4; ti2++) edit_insert(ts, ' ');
                            }
                            edit_recount(focused);
                            edit_scroll_to_cursor(focused); changed = true;
                        } else if (ch == '/' && kbd_ctrl_down()) { /* Ctrl+/: toggle line comment */
                            ts->undo_in_group = false;
                            edit_push_undo(ts);
                            edit_toggle_comment(focused);
                            edit_recount(focused);
                            edit_scroll_to_cursor(focused); changed = true;
                        } else if (ch == ']' && kbd_ctrl_down()) { /* Ctrl+]: jump to matching bracket */
                            ts->undo_in_group = false;
                            if (ts->edit_buf && ts->edit_cur < ts->edit_size) {
                                uint8_t _bc = ts->edit_buf[ts->edit_cur];
                                bool _is_open  = (_bc=='('||_bc=='{'||_bc=='[');
                                bool _is_close = (_bc==')'||_bc=='}'||_bc==']');
                                uint32_t _bm = UINT32_MAX;
                                if (_is_open) {
                                    uint8_t _cl = (_bc=='(') ? ')' : (_bc=='{') ? '}' : ']';
                                    int _dep = 0;
                                    for (uint32_t _i = ts->edit_cur; _i < ts->edit_size; _i++) {
                                        if (ts->edit_buf[_i] == _bc)  _dep++;
                                        else if (ts->edit_buf[_i] == _cl) { _dep--; if (_dep==0) { _bm=_i; break; } }
                                    }
                                } else if (_is_close) {
                                    uint8_t _op = (_bc==')') ? '(' : (_bc=='}') ? '{' : '[';
                                    int _dep = 0;
                                    for (int32_t _i = (int32_t)ts->edit_cur; _i >= 0; _i--) {
                                        if (ts->edit_buf[_i] == _bc)  _dep++;
                                        else if (ts->edit_buf[_i] == _op) { _dep--; if (_dep==0) { _bm=(uint32_t)_i; break; } }
                                    }
                                }
                                if (_bm != UINT32_MAX) {
                                    edit_sel_clear(ts);
                                    ts->edit_cur = _bm;
                                    edit_sync_pos(ts);
                                    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
                                    edit_scroll_to_cursor(focused);
                                } else if (_is_open || _is_close) {
                                    gui_toast("No matching bracket", 0x00708090u);
                                }
                            }
                            changed = true;
                        } else if (ch >= 32 && ch < 127) { /* Printable character */
                            if (!ts->undo_in_group) { edit_push_undo(ts); ts->undo_in_group = true; }
                            /* Auto-close brackets: skip over closing bracket if already there */
                            bool _skip = false;
                            if ((ch == ')' || ch == '}' || ch == ']') &&
                                ts->sel_anchor < 0 &&
                                ts->edit_cur < ts->edit_size &&
                                ts->edit_buf[ts->edit_cur] == (uint8_t)ch) {
                                ts->edit_cur++;  /* skip over existing closer */
                                edit_sync_pos(ts);
                                _skip = true;
                            }
                            if (!_skip) {
                                edit_delete_selection(ts);
                                edit_insert(ts, (uint8_t)ch);
                                /* Auto-close: insert paired closer and leave cursor between */
                                uint8_t _pair = (ch=='(') ? ')' : (ch=='{') ? '}' : (ch=='[') ? ']' : 0;
                                if (_pair && ts->sel_anchor < 0) {
                                    uint32_t _saved = ts->edit_cur;
                                    edit_insert(ts, _pair);
                                    ts->edit_cur = _saved;
                                    edit_sync_pos(ts);
                                    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
                                }
                            }
                            edit_scroll_to_cursor(focused); changed = true;
                        }
                    } else {
                        /* Normal viewer navigation */
                        /* Number keys 1-8: open recent file (welcome screen only) */
                        if (!ts->data && ts->size == 0 && !ts->path[0] &&
                            ch >= '1' && ch <= '8') {
                            int ridx = ch - '1';
                            if (ridx < g_recent_count) {
                                text_open(focused, g_recent[ridx]);
                                changed = true;
                            }
                        } else if (ch == 5) { /* Ctrl+E: enter edit mode */
                            text_enter_edit(focused);
                            if (focused->text.edit_mode) gui_toast("Edit mode", 0x0080c8a0u);
                            changed = true;
                        } else if (ch == 15) { /* Ctrl+O: open file by path */
                            ts->srch_active     = false;
                            ts->save_as_active  = false;
                            ts->open_bar_active = true;
                            ts->open_bar_len    = 0;
                            ts->open_bar_buf[0] = '\0';
                            changed = true;
                        } else if (ch == 3) { /* Ctrl+C: copy selection or search match */
                            if (ts->sel_anchor >= 0 && ts->data && ts->size > 0) {
                                /* Selection takes priority */
                                int32_t _lo, _hi; edit_sel_range(ts, &_lo, &_hi);
                                uint32_t _len = (uint32_t)(_hi - _lo);
                                if (_len > 0 && (uint32_t)_lo < (uint32_t)ts->size) {
                                    if ((uint32_t)_lo + _len > (uint32_t)ts->size)
                                        _len = (uint32_t)ts->size - (uint32_t)_lo;
                                    edit_set_clipboard((const uint8_t *)ts->data + _lo, _len);
                                    gui_toast("Copied", 0x0080c8a0u);
                                }
                            } else if (ts->srch_active && !ts->srch_is_goto && ts->srch_len > 0
                                && ts->srch_match_line >= 0 && ts->data && ts->size > 0) {
                                /* Fall back to copying current search match */
                                const char *_d = (const char *)ts->data;
                                uint64_t _sz = ts->size;
                                int _ln = 0, _cl = 0;
                                uint64_t _bi = 0;
                                while (_bi < _sz) {
                                    if (_ln == ts->srch_match_line && _cl == ts->srch_match_col) break;
                                    if ((unsigned char)_d[_bi] == '\n') { _ln++; _cl = 0; } else _cl++;
                                    _bi++;
                                }
                                uint64_t _qlen = (uint64_t)ts->srch_len;
                                if (_bi + _qlen <= _sz) {
                                    edit_set_clipboard((const uint8_t *)_d + _bi, (uint32_t)_qlen);
                                    gui_toast("Match copied", 0x0080c8a0u);
                                }
                            }
                            changed = true;
                        } else if (ch == 6) { /* Ctrl+F: open search */
                            ts->save_as_active  = false;
                            ts->open_bar_active = false;
                            ts->srch_active  = true;
                            ts->srch_is_goto = false;
                            ts->srch_len     = 0;
                            ts->srch_buf[0]  = '\0';
                            ts->srch_match_line = -1;
                            changed = true;
                        } else if (ch == 7) { /* Ctrl+G: goto line */
                            ts->save_as_active  = false;
                            ts->open_bar_active = false;
                            ts->srch_active  = true;
                            ts->srch_is_goto = true;
                            ts->srch_len     = 0;
                            ts->srch_buf[0]  = '\0';
                            changed = true;
                        } else if (ch == 'w' || ch == 'W') { /* W: toggle word-wrap */
                            ts->word_wrap = !ts->word_wrap;
                            if (ts->word_wrap) ts->h_scroll = 0;
                            changed = true;
                        } else if (ch == 'r' || ch == 'R') { /* R: reload file */
                            if (ts->path[0]) {
                                int saved_scroll = ts->scroll;
                                text_open(focused, ts->path);
                                focused->text.scroll = saved_scroll;
                                gui_toast("File reloaded", 0x0080c8ffu);
                                changed = true;
                            }
                        } else if (ch == 'n' && ts->srch_len > 0) { /* n: next match */
                            text_search_next(focused, true);
                            changed = true;
                        } else if (ch == 'N' && ts->srch_len > 0) { /* N: prev match */
                            text_search_prev(focused);
                            changed = true;
                        } else if (ch == 'j' || ch == 'J' || ch == KEY_DOWN) { /* j/J/Down: scroll down */
                            ts->scroll++; changed = true;
                        } else if ((ch == 'k' || ch == 'K' || ch == KEY_UP) && ts->scroll > 0) { /* k/K/Up: scroll up */
                            ts->scroll--; changed = true;
                        } else if (ch == KEY_LEFT) {
                            if (ts->h_scroll > 0) { ts->h_scroll--; changed = true; }
                        } else if (ch == KEY_RIGHT) {
                            ts->h_scroll++; changed = true;
                        } else if (ch == KEY_PGUP) {
                            {
                                uint64_t fh2 = console_font_height();
                                uint64_t ih2 = focused->h - TITLE_H - BORDER;
                                int page = ih2 > 2u * PAD + fh2 ? (int)((ih2 - 2u*PAD) / fh2) - 1 : 1;
                                if (page < 1) page = 1;
                                ts->scroll -= page;
                            }
                            if (ts->scroll < 0) ts->scroll = 0;
                            changed = true;
                        } else if (ch == KEY_PGDN) {
                            {
                                uint64_t fh2 = console_font_height();
                                uint64_t ih2 = focused->h - TITLE_H - BORDER;
                                int page = ih2 > 2u * PAD + fh2 ? (int)((ih2 - 2u*PAD) / fh2) - 1 : 1;
                                if (page < 1) page = 1;
                                ts->scroll += page;
                            }
                            changed = true;
                        } else if (ch == KEY_HOME) {
                        } else if (ch == KEY_HOME) {
                            ts->scroll   = 0;
                            ts->h_scroll = 0;
                            changed = true;
                        } else if (ch == KEY_END) {
                            ts->scroll = ts->total_lines; changed = true;
                        } else if (ch == 2 && ts->path[0]) { /* Ctrl+B: reveal in Files */
                            char _rdir[128];
                            fb_path_parent(_rdir, ts->path);
                            if (!g_wins[1].active || g_wins[1].state == WIN_HIDDEN) {
                                win_show(&g_wins[1], 1);
                            }
                            if (!gui_streq(g_wins[1].fb.path, _rdir))
                                fb_navigate(&g_wins[1].fb, _rdir);
                            /* Select the file in the listing */
                            {
                                const char *_fname = ts->path;
                                for (const char *_p = ts->path; *_p; _p++)
                                    if (*_p == '/') _fname = _p + 1;
                                for (int _fi = 0; _fi < g_wins[1].fb.entry_count; _fi++) {
                                    if (gui_streq(g_wins[1].fb.entries[_fi], _fname)) {
                                        g_wins[1].fb.sel_row = _fi;
                                        break;
                                    }
                                }
                            }
                            raise_win(1);
                            full_redraw();
                            focused = NULL; closed = true; break;
                        } else if (ch == 27 || ch == 'q' || ch == 23) { /* ESC, q, Ctrl+W */
                            if (ch == 27 && ts->sel_anchor >= 0) {
                                ts->sel_anchor = -1; ts->sel_end = -1;
                                changed = true;
                            } else {
                                int slot = (int)(focused - g_wins);
                                win_hide(focused, slot);
                                focused = NULL; closed = true; break;
                            }
                        }
                    }
                } else if (focused->type == WIN_SETTINGS) {
                    if (ch == KEY_UP || ch == 'k') {
                        if (g_settings_scroll > 0) { g_settings_scroll -= 20; if (g_settings_scroll < 0) g_settings_scroll = 0; changed = true; }
                    } else if (ch == KEY_DOWN || ch == 'j') {
                        g_settings_scroll += 20; changed = true;
                    } else if (ch == KEY_PGUP) {
                        g_settings_scroll -= (int)(focused->h > TITLE_H ? focused->h - TITLE_H : 100u); if (g_settings_scroll < 0) g_settings_scroll = 0; changed = true;
                    } else if (ch == KEY_PGDN) {
                        g_settings_scroll += (int)(focused->h > TITLE_H ? focused->h - TITLE_H : 100u); changed = true;
                    } else if (ch == KEY_HOME) {
                        g_settings_scroll = 0; changed = true;
                    } else if (ch == KEY_END) {
                        g_settings_scroll = 999999; changed = true;
                    } else if (ch == 27 || ch == 23) { /* ESC or Ctrl+W */
                        int slot = (int)(focused - g_wins);
                        win_hide(focused, slot);
                        focused = NULL; closed = true; break;
                    }
                }
            }
            if (changed && !closed) {
                if (g_toast_ticks > 0) {
                    full_redraw();
                } else if (focused && focused->type == WIN_FILES) {
                    fb_render(focused);
                } else if (focused && focused->type == WIN_TEXT) {
                    text_render(focused);
                } else if (focused && focused->type == WIN_SETTINGS) {
                    settings_render(focused);
                }
            }
        }
    }
#ifdef __linux__
    clock_gettime(CLOCK_MONOTONIC, &_stB);
#endif

    /* ── Mouse scroll wheel ── */
    {
        /* Route the wheel to whatever is visually on top at the cursor. Compare the IPC
         * topmost-at-cursor against the topmost built-in at the cursor INCLUDING the
         * terminal (slot 0): if an IPC app is on top, leave the event unconsumed so the
         * compositor routes it to that app; otherwise (terminal or another built-in is on
         * top) consume it here. */
        extern uint32_t ipc_topmost_z_at(int32_t mx, int32_t my);
        uint32_t _ipc_z = ipc_topmost_z_at(mx, my);
        uint32_t _gui_z = 0;
        for (int _i = 0; _i < MAX_WINS; _i++) {
            window_t *_w = &g_wins[_i];
            if (!_w->active || _w->state == WIN_HIDDEN || _w->anim_phase == ANIM_CLOSE) continue;
            if (mx < (int32_t)_w->x || mx >= (int32_t)(_w->x + _w->w)) continue;
            if (my < (int32_t)_w->y || my >= (int32_t)(_w->y + _w->h)) continue;
            if (_w->raise_z > _gui_z) _gui_z = _w->raise_z;
        }
        bool _wl_focus = false;
#ifdef __linux__
        { extern bool wayland_has_focus(void); _wl_focus = wayland_has_focus(); }
#endif
        int8_t scroll = (_wl_focus || (_ipc_z > 0 && _ipc_z > _gui_z)) ? 0 : mouse_consume_scroll();
        if (scroll && g_launcher_open) {
            int rv = (int)launcher_rows_visible();
            if (g_launch_filt_n > rv) {
                g_launcher_scroll -= (int)scroll;
                if (g_launcher_scroll > g_launch_filt_n - rv) g_launcher_scroll = g_launch_filt_n - rv;
                if (g_launcher_scroll < 0) g_launcher_scroll = 0;
                launcher_draw();
            }
            scroll = 0;   /* consumed by the launcher */
        }
        if (scroll) {
            /* Close fb context menu on scroll */
            if (g_fb_ctx_open) { g_fb_ctx_open = false; full_redraw(); }
            /* Find topmost visible window under cursor to scroll */
            for (int zi = MAX_WINS - 1; zi >= 0; zi--) {
                int si = g_z[zi];
                window_t *w = &g_wins[si];
                if (!w->active || w->state == WIN_HIDDEN) continue;
                if (mx < (int32_t)w->x || mx >= (int32_t)(w->x + w->w) ||
                    my < (int32_t)w->y || my >= (int32_t)(w->y + w->h)) continue;
                if (w->type == WIN_TERM) {
                    int tot_sb = console_tsb_count_lines();
                    g_term_scroll += (int)scroll * 3;
                    if (g_term_scroll < 0) g_term_scroll = 0;
                    if (g_term_scroll > tot_sb) g_term_scroll = tot_sb;
                    console_set_suppress_draw(g_term_scroll > 0);
                    full_redraw();
                } else if (w->type == WIN_FILES) {
                    w->fb.scroll_vel -= (int32_t)scroll * 24;
                    if (w->fb.scroll_vel >  2048) w->fb.scroll_vel =  2048;
                    if (w->fb.scroll_vel < -2048) w->fb.scroll_vel = -2048;
                } else if (w->type == WIN_TEXT) {
                    w->text.scroll_vel -= (int32_t)scroll * 24;
                    if (w->text.scroll_vel >  2048) w->text.scroll_vel =  2048;
                    if (w->text.scroll_vel < -2048) w->text.scroll_vel = -2048;
                } else if (w->type == WIN_SETTINGS) {
                    g_settings_scroll -= (int)scroll * 40;
                    if (g_settings_scroll < 0) g_settings_scroll = 0;
                    win_draw_chrome(w, false);
                    settings_render(w);
                }
                break;
            }
        }
    }

    /* ── Taskbar clicks ── */
    if (btn_pressed && in_panel) {
        int32_t cx, cy;

        bool on_logo;
        if (panel_is_vertical()) {
            uint64_t ly = vpanel_logo_y();
            on_logo = ((uint64_t)my >= ly && (uint64_t)my < ly + (TASKBAR_H - 6u));
        } else {
            on_logo = (mx >= (int32_t)LOGO_X && mx < (int32_t)(LOGO_X + logo_eff_w()));
        }
        if (on_logo) {
            g_vol_popup_open = false;
            g_cal_popup_open = false;
            g_launcher_open = !g_launcher_open;
            g_launcher_hover = -1;
            if (g_launcher_open) {
                launcher_open_reset();
                taskbar_draw();
                launcher_draw();
            } else {
                full_redraw();
            }
            mouse_consume_click(&cx, &cy);
            return;
        }

        /* ── Volume tray icon click ── */
        if (g_vol_tray_w > 0u &&
            mx >= (int32_t)g_vol_tray_x &&
            mx <  (int32_t)(g_vol_tray_x + g_vol_tray_w)) {
            g_launcher_open  = false;
            g_launcher_hover = -1;
            g_cal_popup_open = false;
            g_vol_popup_open = !g_vol_popup_open;
            if (g_vol_popup_open) {
                taskbar_draw_tray();
                vol_popup_draw();
            } else {
                full_redraw();
            }
            mouse_consume_click(&cx, &cy);
            return;
        }

        /* ── Clock click → calendar popup ── */
        if (g_clk_w > 0u &&
            mx >= (int32_t)g_clk_x &&
            mx <  (int32_t)(g_clk_x + g_clk_w)) {
            g_launcher_open  = false;
            g_launcher_hover = -1;
            g_vol_popup_open = false;
            g_cal_popup_open = !g_cal_popup_open;
            if (g_cal_popup_open) {
                /* Seed the view to the current month and start on the day grid. */
                uint8_t _cd = 1, _cm = 1; uint16_t _cy16 = 2026;
                rtc_get_date(&_cd, &_cm, &_cy16);
                g_cal_view_mon  = (_cm >= 1 && _cm <= 12) ? _cm : 1;
                g_cal_view_year = _cy16 ? _cy16 : 2026;
                g_cal_pick_open = false;
                taskbar_draw_tray();
                cal_popup_draw();
            } else {
                full_redraw();
            }
            mouse_consume_click(&cx, &cy);
            return;
        }

        {
            /* ── Taskbar favorite pressed ──
             * Built-in launchers (first FAVBAR_BUILTINS) toggle their window
             * immediately; user favorites begin a drag (launch on release). */
            int fav_i = favbar_hit(mx, my);
            if (fav_i >= 0) {
                int bslot = favbar_builtin_slot(fav_i);
                if (bslot >= 0) {
                    window_t *w = &g_wins[bslot];
                    if (w->state == WIN_HIDDEN)      { raise_win(bslot); win_show(w, bslot); }
                    else if (g_z[MAX_WINS - 1] == bslot) win_hide(w, bslot);
                    else                             { raise_win(bslot); full_redraw(); }
                } else {
                    g_fav_drag_idx    = fav_i;
                    g_fav_press_x     = mx;
                    g_fav_drag_active = false;
                }
                mouse_consume_click(&cx, &cy);
                return;
            }

            uint64_t tbw = taskbtn_w();

            /* ── IPC window taskbar buttons (after the favorites strip) ── */
            __attribute__((weak)) int  ipc_window_count(void);
            __attribute__((weak)) void ipc_window_focus_slot(int slot);
            if (ipc_window_count && ipc_window_focus_slot) {
                int nipc = ipc_window_count();
                for (int wi = 0; wi < nipc && wi < 8; wi++) {
                    uint64_t bx = taskbtn_start_x() + (uint64_t)wi * (tbw + TASKBTN_GAP);
                    if (mx >= (int32_t)bx && mx < (int32_t)(bx + tbw)) {
                        ipc_window_focus_slot(wi);
                        full_redraw();
                        break;
                    }
                }
            }

            /* ── Wayland window task buttons (one per toplevel, after IPC) ── */
#ifdef __linux__
            {
                extern int  wayland_toplevel_count(void);
                extern void wayland_toplevel_activate(int);
                int nwl = wayland_toplevel_count();
                if (nwl > 0) {
                    int nipc2 = (ipc_window_count) ? ipc_window_count() : 0;
                    int base = (nipc2 < 8 ? nipc2 : 8);
                    for (int wi = 0; wi < nwl && base + wi < 12; wi++) {
                        uint64_t bx = taskbtn_start_x() + (uint64_t)(base + wi) * (tbw + TASKBTN_GAP);
                        if (mx >= (int32_t)bx && mx < (int32_t)(bx + tbw)) {
                            wayland_toplevel_activate(wi);   /* raise + focus that window */
                            full_redraw();
                            break;
                        }
                    }
                }
            }
#endif
        }

        mouse_consume_click(&cx, &cy);
        return;
    }

    /* ── Text context menu clicks ── */
    if (btn_pressed && g_txt_ctx_open) {
        int32_t cx2 = g_txt_ctx_x, cy2 = g_txt_ctx_y;
        bool hit_ctx = ((uint64_t)mx >= (uint64_t)cx2 &&
                        (uint64_t)mx <  (uint64_t)cx2 + txt_ctx_w() &&
                        (uint64_t)my >= (uint64_t)cy2 + 1u &&
                        (uint64_t)my <  (uint64_t)cy2 + 1u + (uint64_t)(TXT_CTX_ITEMS * CTX_ITEM_H));
        if (hit_ctx && g_txt_ctx_win >= 0) {
            int item = (int)((uint64_t)my - (uint64_t)(cy2 + 1)) / CTX_ITEM_H;
            window_t *tw = &g_wins[g_txt_ctx_win];
            text_state_t *tts = &tw->text;
            if (item == 0) { /* Select All */
                tts->sel_anchor = 0;
                tts->sel_end    = (int32_t)tts->edit_size;
                tts->edit_cur   = tts->edit_size;
                edit_sync_pos(tts);
            } else if (item == 1) { /* Copy */
                edit_copy_to_clip(tts);
                gui_toast("Copied", 0x0080c8a0u);
            } else if (item == 2) { /* Cut */
                edit_push_undo(tts);
                edit_copy_to_clip(tts);
                edit_delete_selection(tts);
                edit_recount(tw);
                edit_scroll_to_cursor(tw);
                gui_toast("Cut", 0x0080c8a0u);
            } else if (item == 3) { /* Paste */
                if (g_clipboard && g_clipboard_len > 0) {
                    edit_push_undo(tts);
                    edit_paste(tw);
                    edit_recount(tw);
                    edit_scroll_to_cursor(tw);
                    gui_toast("Pasted", 0x0080c8a0u);
                } else {
                    gui_toast("Clipboard empty", 0x00708090u);
                }
            } else if (item == 4) { /* Find selection or word */
                tts->undo_in_group = false;
                tts->srch_active  = true;
                tts->srch_is_goto = false;
                tts->srch_is_repl = false;
                tts->repl_focused = false;
                tts->srch_len     = 0;
                tts->srch_buf[0]  = '\0';
                /* If selection active, use it as search query */
                if (tts->sel_anchor >= 0 && tts->sel_anchor != tts->sel_end && tts->edit_buf) {
                    int32_t lo, hi; edit_sel_range(tts, &lo, &hi);
                    int len = (int)(hi - lo);
                    if (len > 63) len = 63;
                    for (int k = 0; k < len; k++) tts->srch_buf[k] = (char)tts->edit_buf[lo + k];
                    tts->srch_buf[len] = '\0';
                    tts->srch_len = len;
                    tts->srch_match_line = -1;
                    if (tts->srch_len > 0) text_search_next(tw, false);
                }
            }
            g_txt_ctx_open = false;
            text_render(tw);
        } else {
            g_txt_ctx_open = false;
            full_redraw();
        }
        int32_t ccx, ccy; mouse_consume_click(&ccx, &ccy);
        return;
    } else if (g_txt_ctx_open && (btn_pressed || rbtn_pressed)) {
        g_txt_ctx_open = false;
        full_redraw();
    }

    /* ── Right-click the Wayland app taskbar button → force-close it ──
     * A reliable close that works regardless of window focus/z-order state
     * (the in-window titlebar close can get flaky with multi-surface apps). */
#ifdef __linux__
    if (rbtn_pressed && in_panel) {
        extern bool wayland_browser_present(void);
        extern bool wayland_close_active(void);
        __attribute__((weak)) int ipc_window_count(void);
        if (wayland_browser_present()) {
            uint64_t tbw2 = taskbtn_w();
            int nipc3 = (ipc_window_count) ? ipc_window_count() : 0;
            int bslot2 = (nipc3 < 8 ? nipc3 : 8);
            uint64_t bx2 = taskbtn_start_x() + (uint64_t)bslot2 * (tbw2 + TASKBTN_GAP);
            if (mx >= (int32_t)bx2 && mx < (int32_t)(bx2 + tbw2)) {
                wayland_close_active();
                gui_toast("Closing app...", 0x00e08060u);
            }
        }
    }
#endif

    /* ── Right-click on Text editor window (edit mode): context menu ── */
    if (rbtn_pressed && !in_panel) {
        for (int zi = MAX_WINS - 1; zi >= 0; zi--) {
            int si = g_z[zi];
            window_t *w = &g_wins[si];
            if (!w->active || w->state == WIN_HIDDEN || w->type != WIN_TEXT) continue;
            if (!w->text.edit_mode) continue;
            if ((uint64_t)mx >= w->x && (uint64_t)mx < w->x + w->w &&
                (uint64_t)my >= w->y + TITLE_H && (uint64_t)my < w->y + w->h) {
                /* Close other menus */
                if (g_ctx_open) { g_ctx_open = false; }
                if (g_fb_ctx_open) { g_fb_ctx_open = false; }
                /* Position context menu clamped to screen */
                int32_t ctx_x = mx, ctx_y = my;
                uint64_t fb_w2 = console_fb_width();
                if ((uint64_t)ctx_x + txt_ctx_w() > fb_w2) ctx_x = (int32_t)(fb_w2 - txt_ctx_w());
                if ((uint64_t)ctx_y + (uint64_t)(TXT_CTX_ITEMS * CTX_ITEM_H) + 2u > ty)
                    ctx_y = (int32_t)(ty - (uint64_t)(TXT_CTX_ITEMS * CTX_ITEM_H + 2u));
                if (ctx_x < 0) ctx_x = 0;
                if (ctx_y < (int32_t)desk_top()) ctx_y = (int32_t)desk_top();
                g_txt_ctx_x   = ctx_x;
                g_txt_ctx_y   = ctx_y;
                g_txt_ctx_win = si;
                g_txt_ctx_open = true;
                g_txt_ctx_hover = -1;
                txt_ctx_draw();
                int32_t ccx, ccy; mouse_consume_click(&ccx, &ccy);
                return;
            }
        }
    }

    /* ── Right-click on Files window: file browser context menu ── */
    if (rbtn_pressed && !in_panel) {
        for (int zi = MAX_WINS - 1; zi >= 0; zi--) {
            int si = g_z[zi];
            window_t *w = &g_wins[si];
            if (!w->active || w->state == WIN_HIDDEN || w->type != WIN_FILES) continue;
            if ((uint64_t)mx >= w->x && (uint64_t)mx < w->x + w->w &&
                (uint64_t)my >= w->y && (uint64_t)my < w->y + w->h) {
                /* Select the row that was right-clicked */
                int ridx = fb_hit_row(w, mx, (int32_t)my);
                if (ridx >= 0) w->fb.sel_row = ridx;
                /* Close desktop ctx menu if open */
                if (g_ctx_open) { g_ctx_open = false; }
                fb_ctx_open_at(si, ridx,
                               ridx >= 0 && w->fb.is_dir[ridx],
                               mx, my);
                int32_t ccx, ccy;
                mouse_consume_click(&ccx, &ccy);
                return;
            }
        }
    }

    /* ── Desktop icon hover ── */
    {
        int new_hov = desk_icon_at(mx, my);
        if (new_hov != g_desk_icon_hover) {
            g_desk_icon_hover = new_hov;
            if (!g_launcher_open && !g_ctx_open) full_redraw();
        }
        /* Left-click on desktop icon */
        if (btn_pressed && new_hov >= 0) {
            g_desk_icon_sel = new_hov;
            const char *ipath = g_desk_icons[new_hov].path;
            const char *_base = strrchr(ipath, '/');
            _base = _base ? _base + 1 : ipath;
            bool _is_exec = (strrchr(_base, '.') == NULL);
            __attribute__((weak)) void gui_spawn_app_with_arg(const char *p, const char *a);
            __attribute__((weak)) void gui_spawn_app(const char *p);
            if (_is_exec) {
                /* Executables (no extension) open on single click */
                if (gui_spawn_app) gui_spawn_app(ipath);
                g_desk_icon_dbl = -1;
            } else {
                /* Documents open on double-click (600ms window) */
                uint64_t now = pit_ticks();
                if (g_desk_icon_dbl == new_hov && now - g_desk_icon_click_t < 60u) {
                    const char *ext = strrchr(ipath, '.');
                    bool is_img = false;
                    if (ext) {
                        static const char *imgs[] = { ".bmp",".ppm",".pgm",".png",".jpg",".jpeg", NULL };
                        for (int _ii = 0; imgs[_ii]; _ii++)
                            if (strcasecmp(ext, imgs[_ii]) == 0) { is_img = true; break; }
                    }
                    if (is_img && gui_spawn_app_with_arg)
                        gui_spawn_app_with_arg("/bin/fifi-imageviewer", ipath);
                    else if (gui_spawn_app_with_arg)
                        gui_spawn_app_with_arg("/bin/fifi-editor", ipath);
                    g_desk_icon_dbl = -1;
                } else {
                    g_desk_icon_dbl     = new_hov;
                    g_desk_icon_click_t = now;
                }
            }
            full_redraw();
            int32_t _cx, _cy; mouse_consume_click(&_cx, &_cy);
        }
        /* Right-click on desktop icon: remove */
        if (rbtn_pressed && new_hov >= 0) {
            g_desk_icons[new_hov].active = false;
            if (g_desk_icon_sel == new_hov) g_desk_icon_sel = -1;
            if (g_desk_icon_dbl == new_hov) g_desk_icon_dbl = -1;
            gui_desktop_save();
            full_redraw();
            int32_t _cx, _cy; mouse_consume_click(&_cx, &_cy);
            return;
        }
    }

    /* ── Right-click a launcher item: open its context menu (pin / desktop) ── */
    if (rbtn_pressed && g_launcher_open) {
        int row = launcher_hit_row(mx, my);
        if (row >= 0) {
            launchctx_open(row, mx, my);
        } else {
            g_launchctx_row  = -1;
            g_launcher_open  = false;
            g_launcher_hover = -1;
        }
        full_redraw();
        int32_t _cx, _cy; mouse_consume_click(&_cx, &_cy);
        return;
    }

    /* ── Right-click a taskbar favorite: unpin it (built-ins can't be removed) ── */
    if (rbtn_pressed && in_panel) {
        int fi = favbar_hit(mx, my);
        if (fi >= FAVBAR_BUILTINS) {
            gui_fav_remove_at(fi - FAVBAR_BUILTINS);
            gui_fav_save();
            gui_toast("Unpinned", 0x0060a0e0u);
            full_redraw();
            int32_t _cx, _cy; mouse_consume_click(&_cx, &_cy);
            return;
        } else if (fi >= 0) {
            gui_toast("Built-in app", 0x00708090u);
            full_redraw();
            int32_t _cx, _cy; mouse_consume_click(&_cx, &_cy);
            return;
        }
    }

    /* ── Right-click: context menu on desktop ── */
    /* When a Wayland app (browser) has focus, right-click belongs to it, not the
     * desktop — otherwise the FiFi desktop menu pops up over the browser. */
    bool wl_focused_rc = false;
#ifdef __linux__
    { extern bool wayland_has_focus(void); wl_focused_rc = wayland_has_focus(); }
#endif
    if (rbtn_pressed && !wl_focused_rc && !in_panel) {
        /* Check if click is on any window */
        bool on_win = false;
        for (int zi = MAX_WINS - 1; zi >= 0; zi--) {
            int si = g_z[zi];
            window_t *w = &g_wins[si];
            if (!w->active || w->state == WIN_HIDDEN) continue;
            if ((uint64_t)mx >= w->x && (uint64_t)mx < w->x + w->w &&
                (uint64_t)my >= w->y && (uint64_t)my < w->y + w->h) {
                on_win = true;
                break;
            }
        }
        if (!on_win) {
            /* Close launcher, vol popup, and fb ctx menu if open */
            if (g_launcher_open) {
                g_launcher_open = false;
                g_launcher_hover = -1;
            }
            if (g_vol_popup_open) {
                g_vol_popup_open = false;
            }
            if (g_cal_popup_open) {
                g_cal_popup_open = false;
            }
            if (g_fb_ctx_open) {
                g_fb_ctx_open = false;
                full_redraw();
            }
            /* Clamp context menu to screen */
            int32_t ctx_x = mx;
            int32_t ctx_y = my;
            if ((uint64_t)ctx_x + ctx_w() > fb_w)
                ctx_x = (int32_t)(fb_w - ctx_w());
            if ((uint64_t)ctx_y + CTX_ITEMS * CTX_ITEM_H + 2u > ty)
                ctx_y = (int32_t)(ty - (uint64_t)(CTX_ITEMS * CTX_ITEM_H + 2u));
            if (ctx_x < 0) ctx_x = 0;
            if (ctx_y < (int32_t)desk_top()) ctx_y = (int32_t)desk_top();
            g_ctx_x = ctx_x;
            g_ctx_y = ctx_y;
            g_ctx_open = true;
            ctx_draw();
            return;
        }
    }

    /* ── Any click dismisses the shortcuts overlay ── */
    if (btn_pressed && g_help_open) {
        int32_t hcx, hcy;
        g_help_open = false;
        full_redraw();
        mouse_consume_click(&hcx, &hcy);
        return;
    }

    /* ── Launcher item context menu clicks (before normal launcher clicks) ── */
    if (btn_pressed && g_launcher_open && g_launchctx_row >= 0) {
        int32_t cx, cy;
        int it = launchctx_hit(mx, my);
        if (it == 0)      launcher_pin_taskbar(g_launchctx_row);
        else if (it == 1) launcher_add_desktop(g_launchctx_row);
        g_launchctx_row = -1; g_launchctx_hover = -1;
        if (it >= 0) { g_launcher_open = false; g_launcher_hover = -1; }
        full_redraw();
        mouse_consume_click(&cx, &cy);
        return;
    }

    /* ── Launcher popup clicks ── */
    if (btn_pressed && g_launcher_open) {
        int32_t cx, cy;
        /* Click inside the search box: keep the menu open (it's always the focus). */
        if (launcher_in_search(mx, my)) {
            mouse_consume_click(&cx, &cy);
            return;
        }
        int row = launcher_hit_row(mx, my);
        if (row >= 0) {
            g_launcher_open = false; g_launcher_hover = -1;
            launcher_do_launch(row);
            mouse_consume_click(&cx, &cy);
            return;
        }
        /* Click anywhere else in the panel (header padding) — keep open; outside — close. */
        uint64_t lx = launcher_lx(), ly = launcher_ly();
        uint64_t w = launcher_panel_w(), ph = launcher_panel_h();
        bool in_panel = ((uint64_t)mx >= lx && (uint64_t)mx < lx + w &&
                         (uint64_t)my >= ly && (uint64_t)my < ly + ph);
        if (in_panel) { mouse_consume_click(&cx, &cy); return; }
        g_launcher_open = false; g_launcher_hover = -1;
        full_redraw();
        /* fall through to window hit test */
    }

    /* ── Volume popup clicks ── */
    if (btn_pressed && g_vol_popup_open) {
        int32_t cx, cy;
        bool inside = ((uint64_t)mx >= g_vol_pop_x &&
                       (uint64_t)mx <  g_vol_pop_x + VOL_POP_W &&
                       (uint64_t)my >= g_vol_pop_y &&
                       (uint64_t)my <  g_vol_pop_y + g_vol_pop_h);
        if (inside) {
            /* [−] button */
            if (g_vol_pop_btn_w > 0u &&
                (uint64_t)mx >= g_vol_pop_minus_x &&
                (uint64_t)mx <  g_vol_pop_minus_x + g_vol_pop_btn_w &&
                (uint64_t)my >= g_vol_pop_btn_y &&
                (uint64_t)my <  g_vol_pop_btn_y + g_vol_pop_btn_h) {
                int v = hda_get_volume() - 5;
                if (v < 0) v = 0;
                hda_set_volume(v);
                vol_popup_draw();
                taskbar_draw_tray();
            /* [+] button */
            } else if (g_vol_pop_btn_w > 0u &&
                (uint64_t)mx >= g_vol_pop_plus_x &&
                (uint64_t)mx <  g_vol_pop_plus_x + g_vol_pop_btn_w &&
                (uint64_t)my >= g_vol_pop_btn_y &&
                (uint64_t)my <  g_vol_pop_btn_y + g_vol_pop_btn_h) {
                int v = hda_get_volume() + 5;
                if (v > 100) v = 100;
                hda_set_volume(v);
                vol_popup_draw();
                taskbar_draw_tray();
            /* Slider track — click sets volume proportionally */
            } else if (g_vol_pop_slid_w > 0u &&
                (uint64_t)mx >= g_vol_pop_slid_x &&
                (uint64_t)mx <  g_vol_pop_slid_x + g_vol_pop_slid_w) {
                uint64_t rel = (uint64_t)mx - g_vol_pop_slid_x;
                int v = (int)(rel * 100u / g_vol_pop_slid_w);
                if (v < 0) v = 0;
                if (v > 100) v = 100;
                hda_set_volume(v);
                vol_popup_draw();
                taskbar_draw_tray();
            }
            mouse_consume_click(&cx, &cy);
            return;
        } else {
            /* Click outside popup — close it */
            g_vol_popup_open = false;
            mouse_consume_click(&cx, &cy);
            full_redraw();
            return;
        }
    }

    /* ── Calendar popup clicks: nav arrows, header picker, month/year cells ── */
    if (btn_pressed && g_cal_popup_open) {
        int32_t cx, cy;
        bool inside = ((uint64_t)mx >= g_cal_pop_x && (uint64_t)mx < g_cal_pop_x + g_cal_pop_w &&
                       (uint64_t)my >= g_cal_pop_y && (uint64_t)my < g_cal_pop_y + g_cal_pop_h);
        if (!inside) {
            g_cal_popup_open = false;
            mouse_consume_click(&cx, &cy);
            full_redraw();
            return;
        }
        bool in_arrow_row = ((uint64_t)my >= g_cal_arrow_by && (uint64_t)my < g_cal_arrow_by + g_cal_arrow_bh);
        /* prev / next month arrows (header row) */
        if (in_arrow_row && (uint64_t)mx >= g_cal_prev_bx && (uint64_t)mx < g_cal_prev_bx + g_cal_arrow_bw) {
            if (--g_cal_view_mon < 1) { g_cal_view_mon = 12; g_cal_view_year--; }
            cal_popup_draw(); mouse_consume_click(&cx, &cy); return;
        }
        if (in_arrow_row && (uint64_t)mx >= g_cal_next_bx && (uint64_t)mx < g_cal_next_bx + g_cal_arrow_bw) {
            if (++g_cal_view_mon > 12) { g_cal_view_mon = 1; g_cal_view_year++; }
            cal_popup_draw(); mouse_consume_click(&cx, &cy); return;
        }
        /* click month/year label → toggle picker */
        if (in_arrow_row && (uint64_t)mx >= g_cal_hdr_bx && (uint64_t)mx < g_cal_hdr_bx + g_cal_hdr_bw) {
            g_cal_pick_open = !g_cal_pick_open;
            full_redraw(); cal_popup_draw(); mouse_consume_click(&cx, &cy); return;
        }
        /* picker: year steppers + month grid */
        if (g_cal_pick_open) {
            bool in_yr_row = ((uint64_t)my >= g_cal_yr_by && (uint64_t)my < g_cal_yr_by + g_cal_yr_bh);
            if (in_yr_row && (uint64_t)mx >= g_cal_yr_prev_bx && (uint64_t)mx < g_cal_yr_prev_bx + g_cal_yr_bw) {
                g_cal_view_year--; cal_popup_draw(); mouse_consume_click(&cx, &cy); return;
            }
            if (in_yr_row && (uint64_t)mx >= g_cal_yr_next_bx && (uint64_t)mx < g_cal_yr_next_bx + g_cal_yr_bw) {
                g_cal_view_year++; cal_popup_draw(); mouse_consume_click(&cx, &cy); return;
            }
            if (g_cal_mcell_w > 0u && g_cal_mcell_h > 0u &&
                (uint64_t)mx >= g_cal_mgrid_x && (uint64_t)mx < g_cal_mgrid_x + 3u * g_cal_mcell_w &&
                (uint64_t)my >= g_cal_mgrid_y && (uint64_t)my < g_cal_mgrid_y + 4u * g_cal_mcell_h) {
                int col = (int)(((uint64_t)mx - g_cal_mgrid_x) / g_cal_mcell_w);
                int row = (int)(((uint64_t)my - g_cal_mgrid_y) / g_cal_mcell_h);
                int m = row * 3 + col + 1;
                if (m >= 1 && m <= 12) {
                    g_cal_view_mon = m;
                    g_cal_pick_open = false;
                    full_redraw(); cal_popup_draw();
                }
                mouse_consume_click(&cx, &cy); return;
            }
        }
        mouse_consume_click(&cx, &cy);
        return;
    }

    /* ── File browser context menu clicks ── */
    if (btn_pressed && g_fb_ctx_open) {
        int32_t cx2, cy2;
        uint64_t fcx = (uint64_t)g_fb_ctx_x;
        uint64_t fcy = (uint64_t)g_fb_ctx_y;
        bool inside = ((uint64_t)mx >= fcx && (uint64_t)mx < fcx + fb_ctx_w() &&
                       (uint64_t)my >= fcy + 1u &&
                       (uint64_t)my < fcy + 1u + (uint64_t)g_fb_ctx_n * CTX_ITEM_H);
        g_fb_ctx_open = false;
        if (inside && g_fb_ctx_win >= 0 && g_fb_ctx_win < MAX_WINS) {
            int item = (int)((uint64_t)my - (fcy + 1u)) / (int)CTX_ITEM_H;
            if (item >= 0 && item < g_fb_ctx_n) {
                fb_ctx_run(item);
            }
        }
        mouse_consume_click(&cx2, &cy2);
        if (inside) return;
        full_redraw();
        /* fall through to window hit test */
    }

    /* ── Context menu clicks ── */
    if (btn_pressed && g_ctx_open) {
        static const char *_ctx_a[CTX_ITEMS] = {
            "Terminal", "Files", "Settings", "Viewer",
            NULL, "File Browser", "Sys Monitor", "Net Monitor", "New Term", "Editor",
            NULL, "Lock Screen", "Show Desktop",
        };
        int32_t cx, cy;
        uint64_t ctx_x = (uint64_t)g_ctx_x;
        uint64_t ctx_y = (uint64_t)g_ctx_y;
        uint64_t total_h = 2u;
        for (int _k = 0; _k < (int)CTX_ITEMS; _k++)
            total_h += _ctx_a[_k] ? CTX_ITEM_H : 8u;
        bool inside = ((uint64_t)mx >= ctx_x && (uint64_t)mx < ctx_x + ctx_w() &&
                       (uint64_t)my >= ctx_y + 1u &&
                       (uint64_t)my < ctx_y + total_h);
        g_ctx_open = false;
        if (inside) {
            uint64_t dy = (uint64_t)my - (ctx_y + 1u);
            int item = -1;
            uint64_t yoff = 0;
            for (int _k = 0; _k < (int)CTX_ITEMS; _k++) {
                uint64_t ih = _ctx_a[_k] ? CTX_ITEM_H : 8u;
                if (dy < yoff + ih) { if (_ctx_a[_k]) item = _k; break; }
                yoff += ih;
            }
            if (item >= 0 && item < 4) {
                window_t *w = &g_wins[item];
                raise_win(item);
                if (w->state == WIN_HIDDEN) win_show(w, item);
                else full_redraw();
            } else if (item >= 5 && item <= 9) {
                static const char *_cp[] = {
                    "/bin/fifi-filebrowser", "/bin/fifi-sysmon",
                    "/bin/fifi-netmon", "/bin/fifi-terminal", "/bin/fifi-editor"
                };
                __attribute__((weak)) void gui_spawn_app(const char *path);
                if (gui_spawn_app) gui_spawn_app(_cp[item - 5]);
                full_redraw();
            } else if (item == 11) {
                __attribute__((weak)) void compositor_lock(void);
                if (compositor_lock) compositor_lock();
                full_redraw();
            } else if (item == 12) {
                gui_show_desktop();
            } else {
                full_redraw();
            }
            mouse_consume_click(&cx, &cy);
            return;
        }
        full_redraw();
        /* fall through to window hit test */
    }

    /* ── Scrollbar drag in progress ── */
    /* ── File browser column separator drag ── */
    {
        for (int _ci = 0; _ci < MAX_WINS; _ci++) {
            window_t *_cw = &g_wins[_ci];
            if (!_cw->active || _cw->state == WIN_HIDDEN || _cw->type != WIN_FILES) continue;
            if (!_cw->fb.col_drag_active) continue;
            if (btn_released) {
                _cw->fb.col_drag_active = false;
                fb_render(_cw);
            } else {
                fb_on_motion(_cw, mx, my);
            }
            int32_t _cx, _cy;
            mouse_consume_click(&_cx, &_cy);
            return;
        }
    }

    /* ── Terminal scrollback scrollbar drag ── */
    if (g_term_sb_drag) {
        if (btn_released) {
            g_term_sb_drag = false;
        } else if (g_term_sb_drag_range > 0) {
            int64_t dy = (int64_t)my - (int64_t)g_term_sb_drag_y0;
            int ns = g_term_sb_drag_s0 - (int)(dy * (int64_t)g_term_sb_drag_max
                                               / (int64_t)g_term_sb_drag_range);
            if (ns < 0) ns = 0;
            if (ns > g_term_sb_drag_max) ns = g_term_sb_drag_max;
            if (ns != g_term_scroll) {
                g_term_scroll = ns;
                console_set_suppress_draw(g_term_scroll > 0);
                full_redraw();
            }
        }
        int32_t cx, cy;
        mouse_consume_click(&cx, &cy);
        return;
    }

    if (g_sb_drag && g_sb_drag_win >= 0) {
        window_t *w = &g_wins[g_sb_drag_win];
        if (btn_released) {
            g_sb_drag = false;
            g_sb_drag_win = -1;
            g_sb_drag_settings = false;
        } else if (g_sb_drag_range > 0) {
            if (g_sb_drag_horiz) {
                int64_t dx = (int64_t)mx - (int64_t)g_sb_drag_x0;
                int ns = g_sb_drag_s0 + (int)(dx * (int64_t)g_sb_drag_max
                                              / (int64_t)g_sb_drag_range);
                if (ns < 0) ns = 0;
                if (ns > g_sb_drag_max) ns = g_sb_drag_max;
                if (ns != w->text.h_scroll) {
                    w->text.h_scroll = ns;
                    text_render(w);
                }
            } else {
                int64_t dy = (int64_t)my - (int64_t)g_sb_drag_y0;
                int ns = g_sb_drag_s0 + (int)(dy * (int64_t)g_sb_drag_max
                                              / (int64_t)g_sb_drag_range);
                if (ns < 0) ns = 0;
                if (ns > g_sb_drag_max) ns = g_sb_drag_max;
                if (g_sb_drag_settings) {
                    if (ns != g_settings_scroll) {
                        g_settings_scroll = ns;
                        win_draw_chrome(w, false);
                        settings_render(w);
                    }
                } else if (g_sb_drag_text) {
                    if (ns != w->text.scroll) {
                        w->text.scroll = ns;
                        text_render(w);
                    }
                } else {
                    if (ns != w->fb.scroll) {
                        w->fb.scroll = ns;
                        fb_render(w);
                    }
                }
            }
        }
        int32_t cx, cy;
        mouse_consume_click(&cx, &cy);
        return;
    }

    /* ── Drag in progress ── */
    if (g_dragging && g_drag_win >= 0) {
        window_t *w = &g_wins[g_drag_win];
        if (btn_released) {
            g_dragging = false;
            g_drag_win = -1;
            if (g_drag_shadow) { kfree(g_drag_shadow); g_drag_shadow = NULL; }
            /* Apply half-snap if preview was active */
            if (g_snap_preview) {
                uint64_t fb_w2 = console_fb_width();
                w->saved_x = w->x; w->saved_y = w->y;
                w->saved_w = w->w; w->saved_h = w->h;
                w->y = desk_top(); w->h = desk_avail();
                if (g_snap_preview == 1) { w->x = 0;           w->w = fb_w2 / 2u; }
                else                     { w->x = fb_w2 / 2u;  w->w = fb_w2 - fb_w2/2u; }
                w->state       = WIN_NORMAL;
                w->half_snapped = true;
                g_snap_preview = 0;
            }
            full_redraw();
        } else {
            int32_t new_x = mx - g_drag_off_x;
            int32_t new_y = my - g_drag_off_y;
            uint64_t fb_w2 = console_fb_width();
            if (new_x < 0) new_x = 0;
            if (new_y < (int32_t)desk_top()) new_y = (int32_t)desk_top();
            if ((uint64_t)new_x + w->w > fb_w2) new_x = (int32_t)(fb_w2 - w->w);
            if ((uint64_t)new_y + w->h > desk_bot()) new_y = (int32_t)(desk_bot() - w->h);
            /* Detect half-snap zone: cursor near left or right screen edge */
            int old_snap = g_snap_preview;
            if (mx <= 2)
                g_snap_preview = 1;
            else if (mx >= (int32_t)fb_w2 - 3)
                g_snap_preview = 2;
            else
                g_snap_preview = 0;
            uint64_t old_wx = w->x, old_wy = w->y;
            w->x = (uint64_t)new_x;
            w->y = (uint64_t)new_y;
            w->btn_cls_x = w->x + w->w - BTN_W;
            w->btn_max_x = w->btn_cls_x - BTN_W;
            w->btn_min_x = w->btn_max_x - BTN_W;
            /* Repaint if position changed or snap state changed */
            if (w->x != old_wx || w->y != old_wy || g_snap_preview != old_snap) {
                if (g_drag_shadow && !g_snap_preview &&
                    g_drag_shad_w == w->w && g_drag_shad_h == w->h) {
                    /* Fast shadow-buffer path: erase old, blit new */
                    draw_desktop_bg();
                    /* Other windows under the dragged one */
                    for (int zi = 0; zi < MAX_WINS - 1; zi++) {
                        int oi = g_z[zi];
                        if (oi == g_drag_win) continue;
                        window_t *ow = &g_wins[oi];
                        if (!ow->active || ow->state == WIN_HIDDEN) continue;
                        win_draw_chrome(ow, true);
                        win_render_content(ow);
                    }
                    /* Blit the captured window pixels at the new position (no drop shadow) */
                    console_paste_rect(g_drag_shadow, w->x, w->y, w->w, w->h);
                    /* Re-draw title bar so active/focus ring is fresh */
                    win_draw_chrome(w, false);
                    draw_status_bar();
                    taskbar_draw();
                } else {
                    full_redraw();
                }
            }
        }
        int32_t cx, cy;
        mouse_consume_click(&cx, &cy);
        return;
    }

    /* ── Resize in progress ── */
    if (g_resizing && g_resize_win >= 0) {
        window_t *w = &g_wins[g_resize_win];
        if (btn_released) {
            g_resizing   = false;
            g_resize_win = -1;
            full_redraw();
        } else {
            win_do_resize(w, mx, my);
        }
        int32_t cx, cy;
        mouse_consume_click(&cx, &cy);
        return;
    }

    /* ── Text drag selection in progress ── */
    if (g_text_drag_sel && g_text_drag_win >= 0) {
        window_t *w = &g_wins[g_text_drag_win];
        if (btn_released || !w->active || w->state == WIN_HIDDEN) {
            g_text_drag_sel = false;
            g_text_drag_win = -1;
        } else if (lbtn) {
            text_state_t *ts = &w->text;
            bool has_sel_data = ts->edit_buf || (ts->data && ts->size > 0);
            if (has_sel_data && ts->sel_anchor >= 0) {
                /* Check if mouse is inside the text area (for scroll throttle) */
                uint64_t _fiy = w->y + TITLE_H;
                uint64_t _fih = w->h - TITLE_H - BORDER;
                uint64_t _fh  = console_font_height();
                uint64_t _tsh = _fh + 4u;
                uint64_t _iht = _fih > _tsh ? _fih - _tsh : _fih;
                bool in_text_area = ((uint64_t)my >= _fiy + PAD &&
                                     (uint64_t)my <  _fiy + _iht);
                bool allow = in_text_area ||
                             (g_gui_tick - g_text_drag_scroll_tick >= 8u);
                if (allow) {
                    uint32_t bi = ts->edit_buf
                                  ? text_xy_to_offset(w, mx, my)
                                  : text_xy_to_offset_ro(w, mx, my);
                    if ((int32_t)bi != ts->sel_end) {
                        int old_scroll = ts->scroll;
                        ts->sel_end = (int32_t)bi;
                        if (ts->edit_buf) {
                            ts->edit_cur = bi;
                            edit_sync_pos(ts);
                            ts->edit_want_col = (uint32_t)ts->edit_cur_col;
                            edit_scroll_to_cursor(w);
                        } else {
                            /* Read mode: scroll to keep sel_end in view */
                            uint64_t _fh2 = console_font_height();
                            if (_fh2 > 0) {
                                int _nl = 0;
                                const uint8_t *_d = (const uint8_t *)ts->data;
                                for (uint32_t _p = 0; _p < bi && _p < (uint32_t)ts->size; _p++)
                                    if (_d[_p] == '\n') _nl++;
                                uint64_t _max_r = _iht > 2u*PAD ? (_iht - 2u*PAD) / _fh2 : 1u;
                                if (_nl < ts->scroll)
                                    ts->scroll = _nl;
                                else if (_nl >= ts->scroll + (int)_max_r)
                                    ts->scroll = _nl - (int)_max_r + 1;
                            }
                        }
                        if (ts->scroll != old_scroll) g_text_drag_scroll_tick = g_gui_tick;
                        text_render(w);
                    }
                }
            }
        }
        int32_t cx, cy;
        mouse_consume_click(&cx, &cy);
        return;
    }

    /* ── Per-window hit tests (z-order top-to-bottom) ── */
    if (btn_pressed) {
        bool hit_any = false;
        extern uint32_t ipc_topmost_z_at(int32_t mx, int32_t my);
        for (int zi = MAX_WINS - 1; zi >= 0; zi--) {
            int si = g_z[zi];
            window_t *w = &g_wins[si];
            if (!w->active || w->state == WIN_HIDDEN) continue;

            int32_t wy  = (int32_t)w->y;
            int32_t wx  = (int32_t)w->x;
            int32_t we  = wx + (int32_t)w->w;
            int32_t wb  = wy + (int32_t)w->h;
            int32_t clx = (int32_t)w->btn_cls_x;
            int32_t mxx = (int32_t)w->btn_max_x;
            int32_t mnx = (int32_t)w->btn_min_x;
            bool in_win = (mx >= wx && mx < we && my >= wy && my < wb);
            bool in_tb  = (my >= wy && my < wy + (int32_t)TITLE_H);

            if (!in_win) {
                resize_dir_t rdir = hit_resize(w, mx, my);
                if (rdir == RES_NONE) continue;
                in_win = true; in_tb = false;
            }

            /* Skip this built-in window if an IPC window is on top at this position */
            if (ipc_topmost_z_at(mx, my) > w->raise_z) {
                hit_any = true;  /* something was here, don't fall through to terminal raise */
                break;           /* IPC window owns this click -- compositor handles it */
            }
            /* Skip if the Wayland browser is on top here — the compositor's mouse
             * routing raises the browser instead. */
#ifdef __linux__
            { extern bool wayland_covers(int32_t, int32_t);
              if (wayland_covers(mx, my) && g_wl_raise_z > w->raise_z) { hit_any = true; break; } }
#endif

            hit_any = true;

            /* Raise window to top on any click */
            bool was_top = (g_z[MAX_WINS - 1] == si);
            raise_win(si);
            if (!was_top) full_redraw();

            if (in_tb && mx >= clx && mx < clx + (int32_t)BTN_W) {
                if (w->type == WIN_TEXT && w->text.edit_mode && w->text.edit_modified)
                    text_save(w);
                win_hide(w, si);
            } else if (in_tb && mx >= mxx && mx < mxx + (int32_t)BTN_W) {
                win_maximize_toggle(w);
            } else if (in_tb && mx >= mnx && mx < mnx + (int32_t)BTN_W) {
                win_hide(w, si);
            } else {
                resize_dir_t rdir = hit_resize(w, mx, my);
                if (rdir != RES_NONE) {
                    w->half_snapped = false;
                    g_resizing   = true;
                    g_resize_win = si;
                    g_resize_dir = rdir;
                    g_resize_ox  = mx;
                    g_resize_oy  = my;
                    g_resize_wx0 = w->x;
                    g_resize_wy0 = w->y;
                    g_resize_ww0 = w->w;
                    g_resize_wh0 = w->h;
                } else if (in_tb && mx >= wx && mx < mnx) {
                    if (w->state == WIN_NORMAL) {
                        /* Double-click detection */
                        bool dbl = (g_last_click_win == si &&
                                    g_gui_tick - g_last_click_tick <= 30u);
                        g_last_click_tick  = g_gui_tick;
                        g_last_click_win   = si;
                        g_last_click_count = dbl ? 2 : 1;
                        if (dbl) {
                            win_maximize_toggle(w);
                        } else if (lbtn) {
                            /* Only start drag when button is physically held — a
                             * synthetic click (tap-to-click, lbtn=false) must not
                             * latch into a permanent drag mode. */
                            g_dragging   = true;
                            g_drag_win   = si;
                            g_drag_off_x = mx - wx;
                            g_drag_off_y = my - wy;
                            /* Unsnap: restore pre-snap size when dragging out of half-snap */
                            if (w->half_snapped && w->saved_w > 0 && w->saved_h > 0) {
                                int32_t rel = g_drag_off_x;
                                uint64_t old_w = w->w;
                                w->w = w->saved_w;
                                w->h = w->saved_h;
                                g_drag_off_x = (int32_t)((int64_t)rel * (int64_t)w->saved_w
                                                          / (int64_t)old_w);
                                if (g_drag_off_x < 0) g_drag_off_x = 0;
                                if ((uint64_t)g_drag_off_x >= w->w - 1u)
                                    g_drag_off_x = (int32_t)(w->w - 1u);
                                w->half_snapped = false;
                            }
                            /* Capture window pixels for smooth shadow-buffer drag */
                            if (g_drag_shadow) { kfree(g_drag_shadow); g_drag_shadow = NULL; }
                            uint64_t shad_pixels = w->w * w->h;
                            g_drag_shadow = (uint32_t *)kmalloc(shad_pixels * 4u);
                            if (g_drag_shadow) {
                                if (!console_capture_rect(g_drag_shadow, w->x, w->y, w->w, w->h)) {
                                    kfree(g_drag_shadow); g_drag_shadow = NULL;
                                } else {
                                    g_drag_shad_w = w->w;
                                    g_drag_shad_h = w->h;
                                }
                            }
                        }
                    } else if (w->state == WIN_MAXIMIZED) {
                        win_maximize_toggle(w);
                    }
                } else if (w->type == WIN_TERM && in_win && !in_tb && g_term_scroll > 0) {
                    /* Terminal scrollback scrollbar click/drag */
                    uint64_t tix = w->x + BORDER;
                    uint64_t tiy = w->y + TITLE_H;
                    uint64_t tiw = w->w - 2u * BORDER;
                    uint64_t tih = w->h - TITLE_H - BORDER;
                    uint64_t tcx = tix + PAD;
                    uint64_t tcy = tiy + PAD;
                    uint64_t tcw = tiw > 2u * PAD ? tiw - 2u * PAD : 1u;
                    uint64_t tch = tih > 2u * PAD ? tih - 2u * PAD : 1u;
                    uint64_t fh_t = console_font_height();
                    int max_rows_t = fh_t > 0 ? (int)(tch / fh_t) : 1;
                    if (max_rows_t < 1) max_rows_t = 1;
                    int total_sb_t = console_tsb_count_lines();
                    if (total_sb_t > max_rows_t) {
                        uint64_t sb_x_t = tcx + tcw - 6u;
                        if ((uint64_t)mx >= sb_x_t && (uint64_t)mx < sb_x_t + 4u &&
                            (uint64_t)my >= tcy && (uint64_t)my < tcy + tch) {
                            uint64_t sb_h_t   = tch;
                            uint64_t thumb_h_t = (uint64_t)max_rows_t * sb_h_t / (uint64_t)total_sb_t;
                            if (thumb_h_t < 6u) thumb_h_t = 6u;
                            if (thumb_h_t > sb_h_t) thumb_h_t = sb_h_t;
                            int max_sc_t = total_sb_t - max_rows_t;
                            if (max_sc_t < 1) max_sc_t = 1;
                            uint64_t thumb_y_t = tcy + (uint64_t)(max_sc_t - g_term_scroll) *
                                                 (sb_h_t - thumb_h_t) / (uint64_t)max_sc_t;
                            if (thumb_y_t + thumb_h_t > tcy + sb_h_t)
                                thumb_y_t = tcy + sb_h_t - thumb_h_t;
                            if ((uint64_t)my >= thumb_y_t && (uint64_t)my < thumb_y_t + thumb_h_t) {
                                /* Thumb drag */
                                g_term_sb_drag        = true;
                                g_term_sb_drag_y0     = my;
                                g_term_sb_drag_s0     = g_term_scroll;
                                g_term_sb_drag_range  = sb_h_t > thumb_h_t ? sb_h_t - thumb_h_t : 1u;
                                g_term_sb_drag_max    = max_sc_t;
                            } else {
                                /* Track click: jump */
                                int ns = max_sc_t - (int)(((uint64_t)my - tcy) * (uint64_t)max_sc_t / sb_h_t);
                                if (ns < 0) ns = 0;
                                if (ns < 0) ns = 0;
                                if (ns > max_sc_t) ns = max_sc_t;
                                g_term_scroll = ns;
                                console_set_suppress_draw(g_term_scroll > 0);
                                full_redraw();
                            }
                        }
                    }
                } else if (w->type == WIN_FILES && in_win && !in_tb) {
                    fb_on_click(w, mx, my);
                } else if (w->type == WIN_TEXT && in_win && !in_tb) {
                    /* Welcome screen: click on a recent file row */
                    if (!w->text.edit_mode && !w->text.data && w->text.size == 0
                        && !w->text.path[0] && g_recent_count > 0) {
                        uint64_t fh2 = console_font_height();
                        uint64_t iy2 = w->y + TITLE_H;
                        uint64_t ih2 = w->h - TITLE_H - BORDER;
                        int nrec2 = g_recent_count;
                        uint64_t block_h2 = (uint64_t)(8 + nrec2 + 2) * fh2 + 4u;
                        uint64_t top_y2 = iy2 + (ih2 > block_h2 + 8u ? (ih2 - block_h2) / 2u : 4u);
                        uint64_t rec_y2 = top_y2 + (uint64_t)(8 + 2) * fh2;
                        if ((uint64_t)my >= rec_y2 && (uint64_t)my < rec_y2 + (uint64_t)nrec2 * fh2) {
                            int ridx2 = (int)((uint64_t)my - rec_y2) / (int)fh2;
                            if (ridx2 >= 0 && ridx2 < nrec2) {
                                text_open(w, g_recent[ridx2]);
                                full_redraw();
                            }
                        }
                    }
                    /* Dismiss search bar on click in text area */
                    if (w->text.srch_active) {
                        uint64_t fiy2 = w->y + TITLE_H;
                        uint64_t fih2 = w->h - TITLE_H - BORDER;
                        uint64_t fh2  = console_font_height();
                        uint64_t tv_sh2 = fh2 + 4u; /* status bar height */
                        uint64_t bar_y = fiy2 + fih2 - tv_sh2 - (fh2 + 8u);
                        if ((uint64_t)my < bar_y) {
                            w->text.srch_active = false;
                            text_render(w);
                        }
                    }
                    /* Click on text viewer scrollbar / minimap */
                    {
                    uint64_t fix = w->x + BORDER;
                    uint64_t fiy = w->y + TITLE_H;
                    uint64_t fiw = w->w - 2u * BORDER;
                    uint64_t fih = w->h - TITLE_H - BORDER;
                    uint64_t fh2 = console_font_height();
                    uint64_t tv_sh2 = fh2 + 4u;
                    uint64_t srch_bar_h = w->text.srch_active
                                         ? (w->text.srch_is_repl ? 2u*(fh2+8u) : fh2+8u) : 0u;
                    uint64_t save_as_bh = (w->text.save_as_active || w->text.open_bar_active) ? (fh2+8u) : 0u;
                    uint64_t ih_text = fih > tv_sh2 + srch_bar_h + save_as_bh ? fih - tv_sh2 - srch_bar_h - save_as_bh : fih;
                    /* Minimap click */
                    bool _show_mm2 = (w->text.total_lines > 0 &&
                                      w->text.total_lines > (int)((ih_text > 2u*PAD ? ih_text - 2u*PAD : 1u) / fh2));
                    uint64_t mm_x2 = fix + fiw - TV_MINIMAP_W;
                    if (_show_mm2 && w->text.total_lines > 0 &&
                        (uint64_t)mx >= mm_x2 && (uint64_t)mx < mm_x2 + TV_MINIMAP_W &&
                        (uint64_t)my >= fiy && (uint64_t)my < fiy + ih_text) {
                        uint64_t max_r2 = ih_text > 2u * PAD ? (ih_text - 2u * PAD) / fh2 : 1u;
                        int max_sc2 = w->text.total_lines - (int)max_r2;
                        if (max_sc2 < 0) max_sc2 = 0;
                        /* Start drag: range = ih_text, so dragging full height = full scroll range */
                        w->text.scroll_vel = 0; w->text.scroll_acc = 0;
                        g_sb_drag       = true;
                        g_sb_drag_win   = si;
                        g_sb_drag_y0    = my;
                        /* Initial scroll = click position, then drag adjusts from there */
                        int _ns = (int)(((uint64_t)my - fiy) * (uint64_t)w->text.total_lines / ih_text);
                        if (_ns < 0) _ns = 0;
                        if (_ns > max_sc2) _ns = max_sc2;
                        w->text.scroll  = _ns;
                        g_sb_drag_s0    = _ns;
                        g_sb_drag_range = ih_text > 1u ? ih_text : 1u;
                        g_sb_drag_max   = max_sc2;
                        g_sb_drag_text  = true;
                        g_sb_drag_horiz = false;
                        text_render(w);
                    }
                    uint64_t sbx = fix + fiw - 8u;
                    if (!_show_mm2 && w->text.total_lines > 0 &&
                        (uint64_t)mx >= sbx && (uint64_t)mx < sbx + 8u &&
                        (uint64_t)my >= fiy && (uint64_t)my < fiy + ih_text) {
                        uint64_t max_r = ih_text > 2u * PAD ? (ih_text - 2u * PAD) / fh2 : 1u;
                        int max_sc = w->text.total_lines - (int)max_r;
                        if (max_sc < 0) max_sc = 0;
                        uint64_t th = (max_r * ih_text) / (uint64_t)w->text.total_lines;
                        if (th < 8u) th = 8u;
                        uint64_t ty = fiy + ((uint64_t)w->text.scroll * (ih_text - th))
                                          / (uint64_t)(max_sc > 0 ? max_sc : 1);
                        if ((uint64_t)my >= ty && (uint64_t)my < ty + th) {
                            /* Thumb drag — cancel inertia */
                            w->text.scroll_vel = 0; w->text.scroll_acc = 0;
                            g_sb_drag       = true;
                            g_sb_drag_win   = si;
                            g_sb_drag_y0    = my;
                            g_sb_drag_s0    = w->text.scroll;
                            g_sb_drag_range = ih_text > th ? ih_text - th : 1u;
                            g_sb_drag_max   = max_sc;
                            g_sb_drag_text  = true;
                            g_sb_drag_horiz = false;
                        } else {
                            /* Track click: jump */
                            int ns = (int)(((uint64_t)my - fiy) *
                                           (uint64_t)w->text.total_lines / ih_text);
                            if (ns < 0) ns = 0;
                            if (ns > max_sc) ns = max_sc;
                            w->text.scroll = ns;
                            text_render(w);
                        }
                    }

                    /* Horizontal scrollbar click */
                    if (!w->text.word_wrap && w->text.max_line_len > 0 && ih_text > 12u) {
                        uint64_t hb_y   = fiy + ih_text - 8u;
                        uint64_t gw2    = fh2 * 2u + 2u;
                        uint64_t hb_x   = fix + gw2;
                        uint64_t hb_w   = fiw > gw2 + 8u ? fiw - gw2 - 8u : 1u;
                        if ((uint64_t)my >= hb_y && (uint64_t)my < hb_y + 8u &&
                            (uint64_t)mx >= hb_x && (uint64_t)mx < hb_x + hb_w) {
                            uint64_t max_r = ih_text > 2u * PAD ? (ih_text - 2u * PAD) / fh2 : 1u;
                            int max_hs = w->text.max_line_len - (int)max_r;
                            if (max_hs < 0) max_hs = 0;
                            uint64_t thumb_w = (max_r * hb_w) / (uint64_t)w->text.max_line_len;
                            if (thumb_w < 8) thumb_w = 8;
                            if (thumb_w > hb_w) thumb_w = hb_w;
                            uint64_t thumb_x = hb_x + ((uint64_t)w->text.h_scroll * (hb_w - thumb_w))
                                               / (uint64_t)(max_hs > 0 ? max_hs : 1);
                            if ((uint64_t)mx >= thumb_x && (uint64_t)mx < thumb_x + thumb_w) {
                                g_sb_drag        = true;
                                g_sb_drag_win    = si;
                                g_sb_drag_x0     = mx;
                                g_sb_drag_s0     = w->text.h_scroll;
                                g_sb_drag_range  = hb_w > thumb_w ? hb_w - thumb_w : 1u;
                                g_sb_drag_max    = max_hs;
                                g_sb_drag_text   = true;
                                g_sb_drag_horiz  = true;
                            } else {
                                /* Track click: jump proportionally */
                                int ns = (int)(((uint64_t)mx - hb_x) * (uint64_t)w->text.max_line_len / hb_w);
                                if (ns < 0) ns = 0;
                                if (ns > max_hs) ns = max_hs;
                                w->text.h_scroll = ns;
                                text_render(w);
                            }
                        }
                    }
                    } /* end scrollbar block */
                    /* Edit mode: click in text area → move cursor */
                    if (w->text.edit_mode) {
                        uint64_t fix3 = w->x + BORDER;
                        uint64_t fiy3 = w->y + TITLE_H;
                        uint64_t fiw3 = w->w - 2u * BORDER;
                        uint64_t fih3 = w->h - TITLE_H - BORDER;
                        uint64_t fh3  = console_font_height();
                        uint64_t fw3  = console_font_width();
                        uint64_t tv_sh3 = fh3 + 4u;
                        uint64_t srch_h3 = w->text.srch_active
                                           ? (w->text.srch_is_repl ? 2u*(fh3+8u) : fh3+8u) : 0u;
                        uint64_t ih_txt3 = fih3 > tv_sh3 + srch_h3 ? fih3 - tv_sh3 - srch_h3 : fih3;
                        uint64_t gtot3 = w->text.total_lines > 0 ? (uint64_t)w->text.total_lines : 1u;
                        uint64_t gw3 = 1;
                        { uint64_t t3=gtot3; while(t3>=10){t3/=10;gw3++;} gw3=(gw3+2u)*fw3; }
                        uint64_t tx3 = fix3 + gw3 + 1u;
                        (void)fiw3;
                        /* Click in line-number gutter: select entire line */
                        text_state_t *ts_g = &w->text;
                        if ((uint64_t)mx >= fix3 && (uint64_t)mx < tx3 &&
                            (uint64_t)my >= fiy3 + PAD && (uint64_t)my < fiy3 + ih_txt3 &&
                            ts_g->edit_buf) {
                            int gclick_row = (int)((uint64_t)my - (fiy3 + PAD)) / (int)fh3;
                            int target_gl = ts_g->scroll + gclick_row;
                            if (target_gl < 0) target_gl = 0;
                            if (target_gl < ts_g->total_lines) {
                                /* Find start of target line */
                                uint32_t gls = 0; int gln = 0;
                                while (gls < ts_g->edit_size && gln < target_gl) {
                                    if (ts_g->edit_buf[gls] == '\n') gln++;
                                    gls++;
                                }
                                /* Find end of target line (exclusive, past '\n') */
                                uint32_t gle = gls;
                                while (gle < ts_g->edit_size && ts_g->edit_buf[gle] != '\n') gle++;
                                if (gle < ts_g->edit_size) gle++; /* include '\n' */
                                ts_g->sel_anchor = (int32_t)gls;
                                ts_g->sel_end    = (int32_t)gle;
                                ts_g->edit_cur   = gle > 0 ? gle - 1u : 0u;
                                edit_sync_pos(ts_g);
                                ts_g->edit_want_col = 0;
                                ts_g->undo_in_group = false;
                                text_render(w);
                            }
                        }
                        /* Check click is in text area (not gutter, scrollbar, or minimap) */
                        bool show_mm3 = (w->text.total_lines > 0 &&
                                         w->text.total_lines > (int)((ih_txt3 > 2u*PAD ? ih_txt3 - 2u*PAD : 1u) / fh3));
                        uint64_t text_right3 = fix3 + fiw3 - 8u - (show_mm3 ? TV_MINIMAP_W + 1u : 0u);
                        if ((uint64_t)mx >= tx3 &&
                            (uint64_t)mx <  text_right3 &&
                            (uint64_t)my >= fiy3 + PAD &&
                            (uint64_t)my <  fiy3 + ih_txt3) {
                            int click_row = (int)((uint64_t)my - (fiy3 + PAD)) / (int)fh3;
                            int click_col = (int)((uint64_t)mx - (tx3 + PAD)) / (int)fw3 + w->text.h_scroll;
                            int target_line = w->text.scroll + click_row;
                            if (target_line < 0) target_line = 0;
                            if (click_col < 0) click_col = 0;
                            /* Scan edit_buf to find byte offset of (target_line, click_col) */
                            text_state_t *ts2 = &w->text;
                            if (ts2->edit_buf && target_line < ts2->total_lines) {
                                int ln = 0, cl = 0;
                                uint32_t bi = 0;
                                /* Advance to target line */
                                while (bi < ts2->edit_size && ln < target_line) {
                                    if (ts2->edit_buf[bi] == '\n') ln++;
                                    bi++;
                                }
                                /* Advance to target column or end of line */
                                while (bi < ts2->edit_size && ts2->edit_buf[bi] != '\n' && cl < click_col) {
                                    bi++; cl++;
                                }
                                /* Multi-click detection */
                                bool _rapid = (g_last_click_win == si &&
                                               g_gui_tick - g_last_click_tick <= 30u);
                                if (_rapid) g_last_click_count++;
                                else        g_last_click_count = 1;
                                bool dbl_txt    = !kbd_shift_down() && g_last_click_count == 2;
                                bool triple_txt = !kbd_shift_down() && g_last_click_count >= 3;

                                if (triple_txt && ts2->edit_buf) {
                                    /* Triple-click: select entire line */
                                    uint32_t ls = bi;
                                    while (ls > 0 && ts2->edit_buf[ls - 1] != '\n') ls--;
                                    uint32_t le = ls;
                                    while (le < ts2->edit_size && ts2->edit_buf[le] != '\n') le++;
                                    if (le < ts2->edit_size) le++; /* include the '\n' */
                                    ts2->sel_anchor    = (int32_t)ls;
                                    ts2->sel_end       = (int32_t)le;
                                    ts2->edit_cur      = le;
                                    edit_sync_pos(ts2);
                                    ts2->edit_want_col = (uint32_t)ts2->edit_cur_col;
                                } else if (dbl_txt && ts2->edit_buf) {
                                    /* Word char: alphanumeric + underscore */
                                    uint8_t _cc = (bi < ts2->edit_size) ? ts2->edit_buf[bi] : 0;
                                    bool _wc = (_cc>='a'&&_cc<='z')||(_cc>='A'&&_cc<='Z')||
                                               (_cc>='0'&&_cc<='9')||_cc=='_';
                                    /* Find word start */
                                    uint32_t ws = bi;
                                    while (ws > 0) {
                                        uint8_t pc = ts2->edit_buf[ws-1];
                                        bool _pw = (pc>='a'&&pc<='z')||(pc>='A'&&pc<='Z')||
                                                   (pc>='0'&&pc<='9')||pc=='_';
                                        if (_wc ? !_pw : _pw || pc==' '||pc=='\t'||pc=='\n') break;
                                        ws--;
                                    }
                                    /* Find word end */
                                    uint32_t we = bi;
                                    while (we < ts2->edit_size) {
                                        uint8_t pc = ts2->edit_buf[we];
                                        bool _pw = (pc>='a'&&pc<='z')||(pc>='A'&&pc<='Z')||
                                                   (pc>='0'&&pc<='9')||pc=='_';
                                        if (_wc ? !_pw : _pw || pc==' '||pc=='\t'||pc=='\n') break;
                                        we++;
                                    }
                                    ts2->sel_anchor    = (int32_t)ws;
                                    ts2->sel_end       = (int32_t)we;
                                    ts2->edit_cur      = we;
                                    edit_sync_pos(ts2);
                                    ts2->edit_want_col = (uint32_t)ts2->edit_cur_col;
                                } else if (kbd_shift_down()) {
                                    /* Shift+click: extend selection from anchor */
                                    if (ts2->sel_anchor < 0)
                                        ts2->sel_anchor = (int32_t)ts2->edit_cur;
                                    ts2->edit_cur      = bi;
                                    ts2->edit_cur_line = ln;
                                    ts2->edit_cur_col  = cl;
                                    ts2->edit_want_col = (uint32_t)cl;
                                    ts2->sel_end       = (int32_t)bi;
                                } else {
                                    edit_sel_clear(ts2);
                                    ts2->edit_cur      = bi;
                                    ts2->edit_cur_line = ln;
                                    ts2->edit_cur_col  = cl;
                                    ts2->edit_want_col = (uint32_t)cl;
                                    /* Start drag-to-select */
                                    g_text_drag_sel = true;
                                    g_text_drag_win = si;
                                    ts2->sel_anchor = (int32_t)bi;
                                    ts2->sel_end    = (int32_t)bi;
                                }
                                ts2->undo_in_group = false;
                                g_last_click_tick = g_gui_tick;
                                g_last_click_win  = si;
                                text_render(w);
                            }
                        }
                    }
                    /* Read-mode: click in text area → start drag selection */
                    if (!w->text.edit_mode && w->text.data && w->text.size > 0 &&
                        w->text.total_lines > 0) {
                        uint64_t fix_r = w->x + BORDER;
                        uint64_t fiy_r = w->y + TITLE_H;
                        uint64_t fiw_r = w->w - 2u * BORDER;
                        uint64_t fih_r = w->h - TITLE_H - BORDER;
                        uint64_t fh_r  = console_font_height();
                        uint64_t fw_r  = console_font_width();
                        uint64_t tv_sh_r = fh_r + 4u;
                        uint64_t srch_h_r = w->text.srch_active
                                            ? (w->text.srch_is_repl ? 2u*(fh_r+8u) : fh_r+8u) : 0u;
                        uint64_t ih_r = fih_r > tv_sh_r + srch_h_r ? fih_r - tv_sh_r - srch_h_r : fih_r;
                        uint64_t gtot_r = (uint64_t)w->text.total_lines;
                        uint64_t gw_r = 1;
                        { uint64_t t_r=gtot_r; while(t_r>=10){t_r/=10;gw_r++;} gw_r=(gw_r+2u)*fw_r; }
                        uint64_t tx_r = fix_r + gw_r + 1u;
                        bool show_mm_r = w->text.total_lines >
                                         (int)((ih_r > 2u*PAD ? ih_r - 2u*PAD : 1u) / fh_r);
                        uint64_t text_right_r = fix_r + fiw_r - 8u - (show_mm_r ? TV_MINIMAP_W + 1u : 0u);
                        if ((uint64_t)mx >= tx_r && (uint64_t)mx < text_right_r &&
                            (uint64_t)my >= fiy_r + PAD && (uint64_t)my < fiy_r + ih_r) {
                            uint32_t bi_r = text_xy_to_offset_ro(w, mx, my);
                            if (kbd_shift_down() && w->text.sel_anchor >= 0) {
                                w->text.sel_end = (int32_t)bi_r;
                            } else {
                                w->text.sel_anchor = (int32_t)bi_r;
                                w->text.sel_end    = (int32_t)bi_r;
                                g_text_drag_sel = true;
                                g_text_drag_win = si;
                            }
                            text_render(w);
                        }
                    }
                } else if (w->type == WIN_SETTINGS && in_win && !in_tb) {
                    /* Scrollbar click/drag */
                    {
                        uint64_t _ix = w->x + BORDER, _iy = w->y + TITLE_H;
                        uint64_t _iw = w->w - 2u * BORDER, _ih = w->h - TITLE_H - BORDER;
                        uint64_t _sb_x = _ix + _iw - 6u;
                        int _tot = g_settings_total_h;
                        if (_tot > (int)_ih && (uint64_t)mx >= _sb_x && (uint64_t)mx < _sb_x + 6u &&
                            (uint64_t)my >= _iy && (uint64_t)my < _iy + _ih) {
                            int _max_sc = _tot - (int)_ih;
                            uint64_t _th = _ih * _ih / (uint64_t)_tot;
                            if (_th < 8u) _th = 8u;
                            uint64_t _ty = _iy + (uint64_t)((int64_t)_ih * (int64_t)g_settings_scroll / (int64_t)_tot);
                            if ((uint64_t)my >= _ty && (uint64_t)my < _ty + _th) {
                                g_sb_drag = true; g_sb_drag_win = si;
                                g_sb_drag_y0 = my; g_sb_drag_s0 = g_settings_scroll;
                                g_sb_drag_range = _ih > _th ? _ih - _th : 1u;
                                g_sb_drag_max = _max_sc;
                                g_sb_drag_text = false; g_sb_drag_horiz = false;
                                g_sb_drag_settings = true;
                            } else {
                                int _ns = (int)((uint64_t)(_tot) * ((uint64_t)my - _iy) / _ih) - (int)(_ih / 2);
                                if (_ns < 0) _ns = 0;
                                if (_ns > _max_sc) _ns = _max_sc;
                                g_settings_scroll = _ns;
                                win_draw_chrome(w, false); settings_render(w);
                            }
                            mouse_consume_click(&(int32_t){0}, &(int32_t){0});
                            goto settings_click_done;
                        }
                    }
                    /* Font selector prev/next buttons */
                    if (g_font_btn_bh > 0 &&
                        (uint64_t)my >= g_font_btn_by &&
                        (uint64_t)my <  g_font_btn_by + g_font_btn_bh) {
                        int nf = 0; while (g_font_paths[nf]) nf++;
                        bool changed_font = false;
                        if ((uint64_t)mx >= g_font_prev_bx &&
                            (uint64_t)mx < g_font_prev_bx + g_font_btn_bw) {
                            g_font_idx = (g_font_idx > 0) ? g_font_idx - 1 : nf - 1;
                            changed_font = true;
                        } else if ((uint64_t)mx >= g_font_next_bx &&
                                   (uint64_t)mx < g_font_next_bx + g_font_btn_bw) {
                            g_font_idx = (g_font_idx < nf - 1) ? g_font_idx + 1 : 0;
                            changed_font = true;
                        }
                        if (changed_font) {
                            console_load_psf(g_font_paths[g_font_idx]);
                            /* Reset settings window size so win_show recomputes it at the new font metrics */
                            g_wins[2].w = 0; g_wins[2].h = 0;
                            win_show(&g_wins[2], 2);
                        }
                    }
                    /* Accent colour swatches (16 presets across 2 rows) */
                    if (g_theme_swatch_sz > 0) {
                        for (int ai = 0; ai < ACCENT_PRESET_COUNT; ai++) {
                            uint64_t swy = (ai < 8) ? g_theme_accent_by : g_theme_accent_by2;
                            if ((uint64_t)my >= swy && (uint64_t)my < swy + g_theme_swatch_sz &&
                                (uint64_t)mx >= g_theme_accent_bx[ai] &&
                                (uint64_t)mx <  g_theme_accent_bx[ai] + g_theme_swatch_sz) {
                                g_theme.accent = g_accent_presets[ai];
                                gui_settings_save();
                                full_redraw();
                                break;
                            }
                        }
                    }
                    /* Wallpaper buttons — per-button y for multi-row support */
                    if (g_theme_wall_bh > 0) {
                        for (int wi = 0; wi < WALLPAPER_COUNT; wi++) {
                            if (g_theme_wall_by_arr[wi] > 0u &&
                                (uint64_t)my >= g_theme_wall_by_arr[wi] &&
                                (uint64_t)my <  g_theme_wall_by_arr[wi] + g_theme_wall_bh &&
                                (uint64_t)mx >= g_theme_wall_bx[wi] &&
                                (uint64_t)mx <  g_theme_wall_bx[wi] + g_theme_wall_bw) {
                                g_theme.wallpaper = wi;
                                gui_settings_save();
                                full_redraw();
                                break;
                            }
                        }
                    }
                    /* Panel position tiles: Bottom / Top / Left / Right */
                    if (g_theme_panel_bh > 0) {
                        for (int pi = 0; pi < PANEL_POS_COUNT; pi++) {
                            if ((uint64_t)my >= g_theme_panel_by[pi] &&
                                (uint64_t)my <  g_theme_panel_by[pi] + g_theme_panel_bh &&
                                (uint64_t)mx >= g_theme_panel_bx[pi] &&
                                (uint64_t)mx <  g_theme_panel_bx[pi] + g_theme_panel_bw) {
                                g_theme.panel_edge = (uint8_t)pi;
                                gui_settings_save();
                                full_redraw();
                                break;
                            }
                        }
                    }
                    /* Panel alignment tiles: Left / Center / Right */
                    if (g_theme_align_bh > 0) {
                        for (int ai = 0; ai < PANEL_ALIGN_COUNT; ai++) {
                            if ((uint64_t)my >= g_theme_align_by[ai] &&
                                (uint64_t)my <  g_theme_align_by[ai] + g_theme_align_bh &&
                                (uint64_t)mx >= g_theme_align_bx[ai] &&
                                (uint64_t)mx <  g_theme_align_bx[ai] + g_theme_align_bw) {
                                g_theme.panel_align = (uint8_t)ai;
                                gui_settings_save();
                                full_redraw();
                                break;
                            }
                        }
                    }
                    /* Toggle buttons: 12h, Animations, Status Bar, Desk Info, Glass, Shadows, Auto-hide */
                    if (g_theme_toggle_h > 0) {
                        for (int ti = 0; ti < THEME_TOGGLE_COUNT; ti++) {
                            if ((uint64_t)my >= g_theme_toggle_y[ti] &&
                                (uint64_t)my <  g_theme_toggle_y[ti] + g_theme_toggle_h &&
                                (uint64_t)mx >= g_theme_toggle_x[ti] &&
                                (uint64_t)mx <  g_theme_toggle_x[ti] + g_theme_toggle_w) {
                                if (ti == 0) g_theme.clock_12h     = !g_theme.clock_12h;
                                if (ti == 1) g_theme.animations    = !g_theme.animations;
                                if (ti == 2) g_theme.statusbar     = !g_theme.statusbar;
                                if (ti == 3) g_theme.desktop_info  = !g_theme.desktop_info;
                                if (ti == 4) g_theme.fx_glass      = !g_theme.fx_glass;
                                if (ti == 5) g_theme.fx_shadows    = !g_theme.fx_shadows;
                                if (ti == 6) g_theme.panel_autohide = !g_theme.panel_autohide;
                                gui_settings_save();
                                full_redraw();
                                break;
                            }
                        }
                    }
                    /* UTC offset buttons: [−] and [+] */
                    if (g_utc_btn_bh > 0 &&
                        (uint64_t)my >= g_utc_btn_by &&
                        (uint64_t)my <  g_utc_btn_by + g_utc_btn_bh) {
                        if ((uint64_t)mx >= g_utc_minus_bx &&
                            (uint64_t)mx <  g_utc_minus_bx + g_font_btn_bw) {
                            if (g_theme.utc_offset > -12) g_theme.utc_offset--;
                            gui_settings_save();
                            full_redraw();
                        } else if ((uint64_t)mx >= g_utc_plus_bx &&
                                   (uint64_t)mx <  g_utc_plus_bx + g_font_btn_bw) {
                            if (g_theme.utc_offset < 14) g_theme.utc_offset++;
                            gui_settings_save();
                            full_redraw();
                        }
                    }
                    /* Volume [−] / [+] buttons */
                    if (g_vol_btn_bh > 0 &&
                        (uint64_t)my >= g_vol_btn_by &&
                        (uint64_t)my <  g_vol_btn_by + g_vol_btn_bh) {
                        if ((uint64_t)mx >= g_vol_minus_bx &&
                            (uint64_t)mx <  g_vol_minus_bx + g_font_btn_bw) {
                            int nv = hda_get_volume() - 5;
                            if (nv < 0) nv = 0;
                            hda_set_volume(nv);
                            full_redraw();
                        } else if ((uint64_t)mx >= g_vol_plus_bx &&
                                   (uint64_t)mx <  g_vol_plus_bx + g_font_btn_bw) {
                            int nv = hda_get_volume() + 5;
                            if (nv > 100) nv = 100;
                            hda_set_volume(nv);
                            full_redraw();
                        }
                    }
                    /* Volume chime test button */
                    if (g_vol_chime_bh > 0 &&
                        (uint64_t)my >= g_vol_chime_by &&
                        (uint64_t)my <  g_vol_chime_by + g_vol_chime_bh &&
                        (uint64_t)mx >= g_vol_chime_bx &&
                        (uint64_t)mx <  g_vol_chime_bx + g_vol_chime_bw) {
                        hda_play_tone(750, 400);
                    }
                    /* Launch Gamepad Visualizer button */
                    if (g_gaming_btn_bh > 0 &&
                        (uint64_t)my >= g_gaming_btn_by &&
                        (uint64_t)my <  g_gaming_btn_by + g_gaming_btn_bh &&
                        (uint64_t)mx >= g_gaming_btn_bx &&
                        (uint64_t)mx <  g_gaming_btn_bx + g_gaming_btn_bw) {
                        __attribute__((weak)) void gui_spawn_app(const char *path);
                        if (gui_spawn_app) gui_spawn_app("/bin/fifi-gamepad");
                    }
                    /* Gaming Mode toggle button */
                    if (g_gaming_mode_bh > 0 &&
                        (uint64_t)my >= g_gaming_mode_by &&
                        (uint64_t)my <  g_gaming_mode_by + g_gaming_mode_bh &&
                        (uint64_t)mx >= g_gaming_mode_bx &&
                        (uint64_t)mx <  g_gaming_mode_bx + g_gaming_mode_bw) {
                        extern bool gaming_mode_active(void);
                        extern void gaming_mode_set(bool on);
                        gaming_mode_set(!gaming_mode_active());
                        settings_render(w);
                    }
                    /* Firewall toggle button */
                    if (g_fw_btn_bh > 0 &&
                        (uint64_t)my >= g_fw_btn_by &&
                        (uint64_t)my <  g_fw_btn_by + g_fw_btn_bh &&
                        (uint64_t)mx >= g_fw_btn_bx &&
                        (uint64_t)mx <  g_fw_btn_bx + g_fw_btn_bw) {
                        __attribute__((weak)) void gui_exec_silent(const char *p, const char *a1, const char *a2);
                        bool now_on = (g_fw_state != 1);
                        if (gui_exec_silent) {
                            if (now_on)
                                gui_exec_silent("/usr/sbin/nft", "-f", "/etc/nftables.conf");
                            else
                                gui_exec_silent("/usr/sbin/nft", "flush", "ruleset");
                        }
                        g_fw_state = now_on ? 1 : 0;
                        settings_render(w);
                    }
                    /* DNS server cycle button */
                    if (g_dns_btn_bh > 0 &&
                        (uint64_t)my >= g_dns_btn_by &&
                        (uint64_t)my <  g_dns_btn_by + g_dns_btn_bh &&
                        (uint64_t)mx >= g_dns_btn_bx &&
                        (uint64_t)mx <  g_dns_btn_bx + g_dns_btn_bw) {
                        __attribute__((weak)) void gui_set_dns(int mode);
                        g_dns_mode = (g_dns_mode + 1) % 3;
                        if (gui_set_dns) gui_set_dns(g_dns_mode);
                        settings_render(w);
                    }
                    /* VPN connect / disconnect button */
                    if (g_vpn_btn_bh > 0 &&
                        (uint64_t)my >= g_vpn_btn_by &&
                        (uint64_t)my <  g_vpn_btn_by + g_vpn_btn_bh &&
                        (uint64_t)mx >= g_vpn_btn_bx &&
                        (uint64_t)mx <  g_vpn_btn_bx + g_vpn_btn_bw) {
                        __attribute__((weak)) bool gui_vpn_connected(void);
                        __attribute__((weak)) void gui_vpn_connect(void);
                        __attribute__((weak)) void gui_vpn_disconnect(void);
                        if (gui_vpn_connected && gui_vpn_connected()) {
                            if (gui_vpn_disconnect) gui_vpn_disconnect();
                        } else {
                            if (gui_vpn_connect) gui_vpn_connect();
                        }
                        settings_render(w);
                    }
                    /* VPN auto-connect toggle */
                    if (g_vpn_auto_bh > 0 &&
                        (uint64_t)my >= g_vpn_auto_by &&
                        (uint64_t)my <  g_vpn_auto_by + g_vpn_auto_bh &&
                        (uint64_t)mx >= g_vpn_auto_bx &&
                        (uint64_t)mx <  g_vpn_auto_bx + g_vpn_auto_bw) {
                        __attribute__((weak)) bool gui_vpn_autoconnect_enabled(void);
                        __attribute__((weak)) void gui_vpn_set_autoconnect(bool on);
                        bool cur = gui_vpn_autoconnect_enabled && gui_vpn_autoconnect_enabled();
                        if (gui_vpn_set_autoconnect) gui_vpn_set_autoconnect(!cur);
                        settings_render(w);
                    }
                    /* Lock timeout cycle button */
                    if (g_lto_btn_bh > 0 &&
                        (uint64_t)my >= g_lto_btn_by &&
                        (uint64_t)my <  g_lto_btn_by + g_lto_btn_bh &&
                        (uint64_t)mx >= g_lto_btn_bx &&
                        (uint64_t)mx <  g_lto_btn_bx + g_lto_btn_bw) {
                        static const int lto_secs[] = { 0, 60, 300, 600, 1800 };
                        g_lto_idx = (g_lto_idx + 1) % 5;
                        __attribute__((weak)) void compositor_set_lock_timeout(int s);
                        if (compositor_set_lock_timeout)
                            compositor_set_lock_timeout(lto_secs[g_lto_idx]);
                        settings_render(w);
                    }
                    settings_click_done:;
                }
            }
            break;
        }
        (void)hit_any;
        int32_t cx, cy;
        mouse_consume_click(&cx, &cy);
    }
#ifdef __linux__
    clock_gettime(CLOCK_MONOTONIC, &_stC);
#endif

    /* ── Inertial scroll tick ── */
    bool inertial_dirty = false;
    for (int zi = 0; zi < MAX_WINS; zi++) {
        window_t *w = &g_wins[zi];
        if (!w->active || w->state == WIN_HIDDEN) continue;
        if (w->type == WIN_FILES && w->fb.scroll_vel) {
            w->fb.scroll_acc += w->fb.scroll_vel;
            int lines = w->fb.scroll_acc / 16;
            w->fb.scroll_acc -= lines * 16;
            if (lines) {
                w->fb.scroll += lines;
                if (w->fb.scroll < 0) w->fb.scroll = 0;
                if (w->fb.scroll >= w->fb.entry_count)
                    w->fb.scroll = w->fb.entry_count > 0 ? w->fb.entry_count - 1 : 0;
                inertial_dirty = true;
            }
            w->fb.scroll_vel = w->fb.scroll_vel * 7 / 8;
            if (w->fb.scroll_vel > -16 && w->fb.scroll_vel < 16) {
                w->fb.scroll_vel = 0;
                w->fb.scroll_acc = 0;
            }
        } else if (w->type == WIN_TEXT && w->text.scroll_vel) {
            w->text.scroll_acc += w->text.scroll_vel;
            int lines = w->text.scroll_acc / 16;
            w->text.scroll_acc -= lines * 16;
            if (lines) {
                w->text.scroll += lines;
                if (w->text.scroll < 0) w->text.scroll = 0;
                inertial_dirty = true;
            }
            w->text.scroll_vel = w->text.scroll_vel * 7 / 8;
            if (w->text.scroll_vel > -16 && w->text.scroll_vel < 16) {
                w->text.scroll_vel = 0;
                w->text.scroll_acc = 0;
            }
        }
    }
    if (inertial_dirty) { g_redraw_src = 98; full_redraw(); }

    /* ── Lock screen overlay ────────────────────────────────────────────── */
    {
        __attribute__((weak)) bool compositor_locked(void);
        bool _locked = compositor_locked && compositor_locked();
        static bool _was_locked = false;
        if (!_locked && _was_locked) {
            /* Just unlocked — force a full desktop repaint to clear the overlay */
            _was_locked = false;
            full_redraw();
        }
        _was_locked = _locked;
        if (_locked) {
            static uint64_t s_lock_sec = (uint64_t)-1;
            uint64_t hz = pit_get_hz(); if (!hz) hz = 100;
            uint64_t now_sec = pit_ticks() / hz;
            __attribute__((weak)) bool compositor_lock_pin_dirty(void);
            bool _pin_dirty = compositor_lock_pin_dirty ? compositor_lock_pin_dirty() : false;
            if (now_sec != s_lock_sec || _pin_dirty) {
                s_lock_sec = now_sec;
                uint64_t scw = console_fb_width();
                uint64_t sch = console_fb_height();
                uint64_t cfw = console_font_width();
                uint64_t cfh = console_font_height();

                /* Scale factor for the big clock: 4x at 1080p+, 3x at 720p, 2x below */
                uint64_t clk_scale = (scw >= 1920u) ? 4u : (scw >= 1280u) ? 3u : 2u;

                /* Full-screen dark overlay with subtle gradient bands */
                console_fill_rect(0, 0, scw, sch, 0x00060810u);
                /* Slightly lighter horizontal band behind the clock area */
                uint64_t band_h = cfh * clk_scale + cfh * 6u;
                uint64_t band_y = sch / 2u - band_h / 2u - cfh * 2u;
                console_fill_rect(0, band_y, scw, band_h, 0x000c1422u);

                /* "FiFi OS" branding at 2x scale, centered at ~15% from top */
                const char *hdr = "FiFi OS";
                uint64_t hlen = gui_strlen(hdr);
                uint64_t hcw = cfw * 2u;
                uint64_t hx = (scw > hlen * hcw) ? (scw - hlen * hcw) / 2u : 0u;
                uint64_t hy = sch / 7u;
                gui_draw_str_scaled(hx, hy, hdr, 2u, 0x00203a5cu, 0x00060810u);

#ifdef __linux__
                char timebuf[8] = "--:--";
                char datebuf[24] = "";
                time_t now = time(NULL);
                struct tm *lt = localtime(&now);
                if (lt) {
                    snprintf(timebuf, sizeof(timebuf), "%02d:%02d", lt->tm_hour, lt->tm_min);
                    snprintf(datebuf, sizeof(datebuf), "%04d-%02d-%02d",
                             lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);
                }
#else
                char timebuf[8] = "--:--";
                char datebuf[24] = "";
                { uint8_t hr, mn, sc; rtc_read(&hr, &mn, &sc);
                  snprintf(timebuf, sizeof(timebuf), "%02d:%02d", hr, mn); }
#endif
                /* Large clock: centered vertically at 45% of screen */
                uint64_t tlen    = gui_strlen(timebuf);
                uint64_t clk_w   = tlen * cfw * clk_scale;
                uint64_t clk_h   = cfh * clk_scale;
                uint64_t tx      = (scw > clk_w) ? (scw - clk_w) / 2u : 0u;
                uint64_t ty      = sch * 9u / 20u - clk_h / 2u;
                /* Background panel for clock */
                uint64_t pad_x = cfw * clk_scale / 2u;
                uint64_t pad_y = cfh * clk_scale / 4u;
                console_fill_rect(tx - pad_x, ty - pad_y,
                                  clk_w + pad_x * 2u, clk_h + pad_y * 2u, 0x0009111cu);
                console_fill_rect(tx - pad_x, ty - pad_y,
                                  clk_w + pad_x * 2u, 1u, 0x001e2c40u);
                console_fill_rect(tx - pad_x, ty - pad_y + clk_h + pad_y * 2u - 1u,
                                  clk_w + pad_x * 2u, 1u, 0x001e2c40u);
                gui_draw_str_scaled(tx, ty, timebuf, clk_scale, 0x0090c4e8u, 0x0009111cu);

                /* Date below clock */
                if (datebuf[0]) {
                    uint64_t dlen = gui_strlen(datebuf);
                    uint64_t dx = (scw > dlen * cfw) ? (scw - dlen * cfw) / 2u : 0u;
                    uint64_t dy = ty + clk_h + pad_y + cfh / 2u + 6u;
                    gui_draw_str(dx, dy, datebuf, 0x00486080u, 0x00060810u);
                }

                uint64_t hinty = sch * 3u / 4u;

#ifdef __linux__
                /* PIN entry: show dots + feedback when a PIN file is configured */
                __attribute__((weak)) int  compositor_lock_pin_len(void);
                __attribute__((weak)) bool compositor_lock_bad_pin(void);
                int _plen = compositor_lock_pin_len ? compositor_lock_pin_len() : 0;
                bool _bad = compositor_lock_bad_pin ? compositor_lock_bad_pin() : false;
                bool _has_pin = (vfs_filesize("lock-pin") >= 0);

                if (_has_pin) {
                    uint64_t pin_y = hinty - cfh * 3u;
                    /* Row of dots for typed chars */
                    const char *plbl = "PIN: ";
                    uint64_t plbl_w = gui_strlen(plbl) * cfw;
                    uint64_t dots_total = 16u * cfw;
                    uint64_t pin_x = (scw > plbl_w + dots_total) ? (scw - plbl_w - dots_total) / 2u : 0u;
                    gui_draw_str(pin_x, pin_y, plbl, 0x00607080u, 0x00060810u);
                    for (int _pi = 0; _pi < 16; _pi++) {
                        const char *dot = _pi < _plen ? "*" : "_";
                        uint32_t col = _pi < _plen ? 0x0090c4e8u : 0x00203040u;
                        gui_draw_str(pin_x + plbl_w + (uint64_t)_pi * cfw, pin_y, dot, col, 0x00060810u);
                    }
                    if (_bad) {
                        const char *bad_msg = "Incorrect PIN";
                        uint64_t blen = gui_strlen(bad_msg);
                        uint64_t bx = (scw > blen * cfw) ? (scw - blen * cfw) / 2u : 0u;
                        gui_draw_str(bx, pin_y + cfh + 4u, bad_msg, 0x00e84040u, 0x00060810u);
                    }
                    const char *hint = "Type PIN and press Enter";
                    uint64_t hintlen = gui_strlen(hint);
                    uint64_t hintx = (scw > hintlen * cfw) ? (scw - hintlen * cfw) / 2u : 0u;
                    gui_draw_str(hintx, hinty, hint, 0x00304050u, 0x00060810u);
                } else {
                    const char *hint = "Press any key to unlock";
                    uint64_t hintlen = gui_strlen(hint);
                    uint64_t hintx = (scw > hintlen * cfw) ? (scw - hintlen * cfw) / 2u : 0u;
                    gui_draw_str(hintx, hinty, hint, 0x00304050u, 0x00060810u);
                }
#else
                const char *hint = "Press any key to unlock";
                uint64_t hintlen = gui_strlen(hint);
                uint64_t hintx = (scw > hintlen * cfw) ? (scw - hintlen * cfw) / 2u : 0u;
                gui_draw_str(hintx, hinty, hint, 0x00304050u, 0x00060810u);
#endif
                console_mark_dirty_rows(0, (uint32_t)sch);
            }
        }
    }

#ifdef __linux__
    {
        long _ms_hover = _SUB_MS(_st0, _stA);
        long _ms_kbd   = _SUB_MS(_stA, _stB);
        long _ms_click = _SUB_MS(_stB, _stC);
        long _ms_max   = _ms_hover > _ms_kbd ? _ms_hover : _ms_kbd;
        if (_ms_click > _ms_max) _ms_max = _ms_click;
        (void)_ms_max;
    }
#undef _SUB_MS
#endif

}

/* ── Public helpers callable from platform code ─────────────────────────── */

void gui_open_in_viewer(const char *path) {
    text_open(&g_wins[3], path);
    win_show(&g_wins[3], 3);
    raise_win(3);
}

/* Load an image file and set it as the desktop wallpaper. */
void gui_set_wallpaper_image(const char *path) {
    if (!path) return;
    uint32_t *new_px = NULL;
    uint32_t nw = 0, nh = 0;
    if (!platform_load_image(path, &new_px, &nw, &nh)) {
        gui_toast("Wallpaper: unsupported format or load failed", 0x00c04030u);
        return;
    }
    free(g_wall_img);
    g_wall_img   = new_px;
    g_wall_img_w = nw;
    g_wall_img_h = nh;
    g_theme.wallpaper = WALLPAPER_IMAGE;
    full_redraw();
}

/* Add a desktop icon shortcut. label may be NULL (defaults to basename of path). */
void gui_add_desktop_icon(const char *path, const char *label) {
    if (!path) return;
    /* Find an empty slot */
    for (int i = 0; i < DESK_ICON_MAX; i++) {
        if (!g_desk_icons[i].active) {
            strncpy(g_desk_icons[i].path, path, sizeof(g_desk_icons[i].path) - 1);
            if (label && label[0])
                strncpy(g_desk_icons[i].label, label, sizeof(g_desk_icons[i].label) - 1);
            else
                g_desk_icons[i].label[0] = '\0';
            g_desk_icons[i].active = true;
            full_redraw();
            return;
        }
    }
    gui_toast("Desktop full (max 12 icons)", 0x00c04030u);
}

void gui_toast_extern(const char *msg, uint32_t color) {
    gui_toast(msg, color);
}

void gui_show_desktop(void) {
    static bool s_hidden = false;
    static win_state_t s_prev_state[MAX_WINS];
    if (!s_hidden) {
        for (int i = 0; i < MAX_WINS; i++) {
            s_prev_state[i] = g_wins[i].state;
            if (g_wins[i].active && g_wins[i].state != WIN_HIDDEN)
                g_wins[i].state = WIN_HIDDEN;
        }
        __attribute__((weak)) void ipc_hide_all(void);
        if (ipc_hide_all) ipc_hide_all();
        __attribute__((weak)) void wayland_set_all_minimized(bool m);
        if (wayland_set_all_minimized) wayland_set_all_minimized(true);
        s_hidden = true;
        gui_toast("Desktop", 0x0060a0e0u);
    } else {
        for (int i = 0; i < MAX_WINS; i++) {
            if (g_wins[i].active && s_prev_state[i] != WIN_HIDDEN)
                g_wins[i].state = s_prev_state[i];
        }
        __attribute__((weak)) void ipc_show_all(void);
        if (ipc_show_all) ipc_show_all();
        __attribute__((weak)) void wayland_set_all_minimized(bool m);
        if (wayland_set_all_minimized) wayland_set_all_minimized(false);
        s_hidden = false;
        gui_toast("Restore", 0x0060a0e0u);
    }
    /* No full_redraw() here -- called from event thread, would race with
     * drm_flush() running outside g_mx in the render thread.
     * ipc_hide_all/ipc_show_all set g_ipc_needs_redraw so render picks it up. */
}

/* Alt+F4 fallback: hide the topmost visible built-in window (same effect as
 * its close button — built-in windows are persistent, close = hide). */
void gui_close_focused_win(void) {
    for (int i = MAX_WINS - 1; i >= 0; i--) {
        int wi = g_z[i];
        if (g_wins[wi].active && g_wins[wi].state != WIN_HIDDEN) {
            g_wins[wi].state = WIN_HIDDEN;
            full_redraw();
            return;
        }
    }
}

void gui_snap_focused(int zone) {
    int top = -1;
    for (int i = MAX_WINS - 1; i >= 0; i--) {
        int wi = g_z[i];
        if (g_wins[wi].active && g_wins[wi].state != WIN_HIDDEN) { top = wi; break; }
    }
    if (top < 0) return;
    window_t *w = &g_wins[top];
    uint64_t fb_w = console_fb_width();
    uint64_t fb_h = console_fb_height();
    uint64_t uh   = fb_h > TASKBAR_H ? fb_h - TASKBAR_H : fb_h;
    static const char *labels[] = {"Snap restore","Snap left","Snap right","Snap max"};
    if (zone >= 0 && zone <= 3) gui_toast(labels[zone], 0x0060a0e0u);
    if (zone == 0) {
        if (w->half_snapped) {
            w->x = w->saved_x; w->y = w->saved_y;
            w->w = w->saved_w; w->h = w->saved_h;
            w->half_snapped = false;
        }
    } else {
        if (!w->half_snapped) {
            w->saved_x = w->x; w->saved_y = w->y;
            w->saved_w = w->w; w->saved_h = w->h;
        }
        switch (zone) {
        case 1: w->x = 0;        w->y = 0; w->w = fb_w / 2;        w->h = uh; break;
        case 2: w->x = fb_w / 2; w->y = 0; w->w = fb_w - fb_w / 2; w->h = uh; break;
        case 3: w->x = 0;        w->y = 0; w->w = fb_w;             w->h = uh; break;
        }
        w->half_snapped = true;
    }
    full_redraw();
}

/* Scroll the built-in terminal by one page. dir > 0 = scroll back, dir < 0 = forward. */
void gui_term_scroll_page(int dir) {
    if (!g_wins[0].active || g_wins[0].state == WIN_HIDDEN) return;
    uint64_t fh = g_wins[0].h > TITLE_H + BORDER + 2u * PAD
                  ? g_wins[0].h - TITLE_H - BORDER - 2u * PAD : 1u;
    int page = (int)(fh / console_font_height());
    if (page < 1) page = 1;
    int tot = console_tsb_count_lines();
    if (dir > 0) {
        g_term_scroll += page;
        if (g_term_scroll > tot) g_term_scroll = tot;
    } else {
        g_term_scroll -= page;
        if (g_term_scroll < 0) g_term_scroll = 0;
    }
    console_set_suppress_draw(g_term_scroll > 0);
    full_redraw();
}

/* Re-composite built-in windows that sit ABOVE the IPC windows they overlap.
 *
 * IPC windows are blitted from their own framebuffers every frame (ipc_blit_all), so any
 * built-in window that should appear on top of an IPC window must be re-drawn AFTER that
 * blit. This runs last in the compositor frame, after all IPC drawing.
 *
 * EVERY built-in window is treated identically, including the terminal (slot 0): each is
 * re-rendered iff its raise_z is greater than the highest IPC z over its own rectangle.
 * Windows are drawn in ascending raise_z order so the topmost one lands last. */
void gui_overdraw_top(void) {
    extern uint32_t ipc_topmost_z_in_rect(uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh);
    extern int ipc_window_count(void);
    bool wl_mapped = false;
#ifdef __linux__
    { extern bool wayland_any_mapped(void); wl_mapped = wayland_any_mapped(); }
#endif

    /* Nothing composited above the built-ins (no IPC windows, no Wayland browser) →
     * full_redraw already painted them correctly. */
    if (ipc_window_count() == 0 && !wl_mapped) return;

    /* Visible windows sorted ascending by raise_z (lowest first, topmost last). */
    int order[MAX_WINS], n = 0;
    for (int i = 0; i < MAX_WINS; i++) {
        window_t *w = &g_wins[i];
        if (!w->active || w->state == WIN_HIDDEN || w->anim_phase == ANIM_CLOSE) continue;
        order[n++] = i;
    }
    for (int a = 1; a < n; a++) {
        int key = order[a], b = a - 1;
        while (b >= 0 && g_wins[order[b]].raise_z > g_wins[key].raise_z) {
            order[b + 1] = order[b]; b--;
        }
        order[b + 1] = key;
    }

    for (int j = 0; j < n; j++) {
        window_t *w = &g_wins[order[j]];
        /* Re-draw a built-in window over the IPC layer ONLY where it is genuinely on top,
         * i.e. its raise_z beats every IPC window covering its rectangle. The terminal
         * (slot 0) is a normal participant here: when the user raises it (clicks it or
         * picks it from the taskbar) its raise_z goes above the IPC apps and it covers
         * them; when an IPC app is on top, the terminal's raise_z is lower and it is left
         * behind. Click and scroll routing use the terminal-inclusive topmost so input
         * matches what is visually on top. */
        uint32_t ipc_z = ipc_topmost_z_in_rect((uint32_t)w->x, (uint32_t)w->y,
                                               (uint32_t)w->w, (uint32_t)w->h);
        uint32_t below_z = ipc_z;
        if (wl_mapped && g_wl_raise_z > below_z) below_z = g_wl_raise_z;
        /* Only redraw over the layers below if this window is genuinely on top. */
        if (w->raise_z <= below_z) continue;
        /* fill_content=true draws the FULL chrome (title bar + border + focus ring). */
        switch (w->type) {
        case WIN_TERM:
            win_draw_chrome(w, true);
            win_render_content(w);   /* term_set_viewport → console re-renders text over IPC */
            break;
        case WIN_FILES:
            fb_render(w);
            break;
        case WIN_SETTINGS:
            win_draw_chrome(w, true);
            settings_render(w);
            break;
        case WIN_TEXT:
            win_draw_chrome(w, true);
            text_render(w);
            break;
        default: break;
        }
    }
}

/* Returns true if any visible built-in window (INCLUDING the terminal) with raise_z > ipc_z
 * overlaps the rect (rx,ry,rw,rh). Used by ipc_draw_overlays to skip drawing IPC chrome in
 * areas where a higher-z built-in window is on top. */
bool gui_builtin_covers(int32_t rx, int32_t ry, uint32_t rw, uint32_t rh, uint32_t ipc_z) {
    for (int i = 0; i < MAX_WINS; i++) {
        window_t *w = &g_wins[i];
        if (!w->active || w->state == WIN_HIDDEN || w->anim_phase == ANIM_CLOSE) continue;
        if (w->raise_z <= ipc_z) continue;  /* this built-in is behind the IPC window */
        /* AABB overlap test */
        if ((int32_t)w->x >= rx + (int32_t)rw) continue;
        if (rx >= (int32_t)(w->x + w->w))       continue;
        if ((int32_t)w->y >= ry + (int32_t)rh)  continue;
        if (ry >= (int32_t)(w->y + w->h))        continue;
        return true;
    }
    return false;
}
