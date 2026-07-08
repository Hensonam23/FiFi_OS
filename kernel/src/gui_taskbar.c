#include "gui_internal.h"
#ifdef __linux__
#include <string.h>
#include <stdlib.h>
#endif

/* ── Taskbar ─────────────────────────────────────────────────────────── */

uint64_t logo_eff_w(void) {
    uint64_t fw = console_font_width();
    uint64_t w  = (uint64_t)gui_strlen("FiFi OS") * fw + 16u;
    return (w > LOGO_W) ? w : LOGO_W;
}

/* ── Taskbar favorites: built-in app launchers + user-pinned apps ─────────
 * The first FAVBAR_BUILTINS entries are the always-present built-in windows
 * (Terminal/Files/Settings/Viewer); the rest are user-pinned favorites. Every
 * entry is a square icon button that shows a running indicator when its
 * window/app is open. This replaces the old wide text window-buttons. */
static const struct { const char *label; const char *icon; int slot; } g_fav_builtin[FAVBAR_BUILTINS] = {
    { "Terminal",     "/bin/fifi-terminal",    0 },
    { "Files",        "/bin/fifi-filebrowser", 1 },
    { "Settings",     "/bin/fifi-settings",    2 },
    { "Image Viewer", "/bin/fifi-imageviewer", 3 },
};

int favbar_count(void) { return FAVBAR_BUILTINS + g_fav_count; }
int favbar_builtin_slot(int idx) {
    return (idx >= 0 && idx < FAVBAR_BUILTINS) ? g_fav_builtin[idx].slot : -1;
}

uint64_t fav_btn_w(void)     { return TASKBAR_H; }                 /* square icon button */
uint64_t favbar_start_x(void){ return LOGO_X + logo_eff_w() + 8u; }
uint64_t favbar_w(void) {
    return (uint64_t)favbar_count() * (fav_btn_w() + TASKBTN_GAP) + 8u;
}

uint64_t taskbtn_start_x(void) {
    return favbar_start_x() + favbar_w();
}

uint64_t taskbtn_w(void) {
    uint64_t fw = console_font_width();
    uint64_t w  = 16u * fw;   /* fits "Security Center" (15 chars) + margin */
    return w > (uint64_t)TASKBTN_W ? w : (uint64_t)TASKBTN_W;
}

/* Window titles (e.g. "Google — LibreWolf") contain UTF-8 punctuation the PSF
 * console font can't render byte-by-byte, producing mojibake ("Google ė¢Â Li").
 * Decode to a display-safe ASCII copy: common punctuation → sane equivalents,
 * anything else non-ASCII → '?'. */
static void taskbar_ascii_label(const char *in, char *out, size_t n) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o + 1 < n) {
        unsigned char c = *p;
        if (c < 0x80u) { out[o++] = (char)c; p++; continue; }
        uint32_t cp = 0; int cont = 0;
        if      ((c & 0xE0u) == 0xC0u) { cp = c & 0x1Fu; cont = 1; }
        else if ((c & 0xF0u) == 0xE0u) { cp = c & 0x0Fu; cont = 2; }
        else if ((c & 0xF8u) == 0xF0u) { cp = c & 0x07u; cont = 3; }
        else { p++; continue; }               /* stray continuation byte */
        p++;
        for (int i = 0; i < cont && (*p & 0xC0u) == 0x80u; i++) { cp = (cp << 6) | (*p & 0x3Fu); p++; }
        char r;
        switch (cp) {
            case 0x2012: case 0x2013: case 0x2014: case 0x2015: r = '-'; break;  /* dashes */
            case 0x2018: case 0x2019: case 0x201A: case 0x2032: r = '\''; break; /* quotes */
            case 0x201C: case 0x201D: case 0x201E: case 0x2033: r = '"'; break;
            case 0x2026: r = '.'; break;                                          /* ellipsis */
            case 0x00B7: case 0x2022: case 0x2027: r = '-'; break;                /* bullets */
            case 0x00A0: case 0x2009: case 0x202F: r = ' '; break;                /* spaces */
            default: r = (cp < 0x80u) ? (char)cp : '?'; break;
        }
        out[o++] = r;
    }
    out[o] = '\0';
}

/* Rounded-pill task button: gradient fill + 2px corner softening + underline
 * running/focused indicator (KDE Plasma style). */
void taskbar_pill(uint64_t bx, uint64_t ty, uint64_t tbw, const char *label_raw,
                  bool vis, bool focused, bool hov) {
    char label[64];
    taskbar_ascii_label(label_raw ? label_raw : "", label, sizeof label);
    /* Apps title as "<page/doc> - <App>"; show just the app name (matches the
     * SSD titlebar) — keep only the segment after the last " - " separator. */
    const char *lbl = label;
    for (size_t j = 1; label[j] && label[j + 1]; j++)
        if (label[j] == '-' && label[j - 1] == ' ' && label[j + 1] == ' ')
            lbl = &label[j + 2];
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();
    uint64_t ph = TASKBAR_H - 6u;
    uint32_t top = focused ? 0x003f74c8u : hov ? 0x002c3a52u : vis ? 0x00242f44u : 0x001a2232u;
    uint32_t bot = focused ? 0x002a55a0u : hov ? 0x00202c40u : vis ? 0x001a2434u : 0x00141a28u;
    console_fill_vgrad(bx, ty + 3u, tbw, ph, top, bot);
    /* corner softening: notch the 4 pill corners back to the bar base */
    uint32_t base = 0x000b0f1au;
    console_fill_rect(bx,            ty + 3u,          2u, 1u, base);
    console_fill_rect(bx,            ty + 4u,          1u, 1u, base);
    console_fill_rect(bx + tbw - 2u, ty + 3u,          2u, 1u, base);
    console_fill_rect(bx + tbw - 1u, ty + 4u,          1u, 1u, base);
    console_fill_rect(bx,            ty + 2u + ph,     2u, 1u, base);
    console_fill_rect(bx,            ty + 1u + ph,     1u, 1u, base);
    console_fill_rect(bx + tbw - 2u, ty + 2u + ph,     2u, 1u, base);
    console_fill_rect(bx + tbw - 1u, ty + 1u + ph,     1u, 1u, base);
    /* running/focused underline */
    if (vis) {
        uint64_t uw = focused ? tbw - 8u : tbw / 3u;
        uint64_t ux = bx + (tbw - uw) / 2u;
        console_fill_rect(ux, ty + TASKBAR_H - 3u, uw, 2u,
                          focused ? 0x0078b4ffu : 0x00446a9cu);
    }
    uint64_t llen     = (uint64_t)gui_strlen(lbl);
    uint64_t max_ch   = tbw / fw;
    uint64_t disp_len = llen < max_ch ? llen : max_ch;
    uint64_t lpx      = bx + (tbw > disp_len * fw ? (tbw - disp_len * fw) / 2u : 0u);
    uint64_t lpy      = ty + (TASKBAR_H - fh) / 2u;
    gui_draw_str_clip_fg(lpx, lpy, lbl,
                         focused ? 0x00f0f6ffu : COL_TASKBTN_FG, max_ch);
}

void taskbar_draw_btn(int slot, const char *label) {
    uint64_t fb_h = console_fb_height();
    uint64_t ty   = fb_h - TASKBAR_H;
    uint64_t tbw  = taskbtn_w();
    uint64_t bx   = taskbtn_start_x() + (uint64_t)slot * (tbw + TASKBTN_GAP);
    bool     vis  = (slot < MAX_WINS && g_wins[slot].active &&
                     (g_wins[slot].state != WIN_HIDDEN ||
                      g_wins[slot].anim_phase == ANIM_CLOSE));
    bool     hov  = (g_taskbtn_hover == slot);
    /* Focused = topmost visible window */
    bool focused = (vis && g_z[MAX_WINS - 1] == slot);
    taskbar_pill(bx, ty, tbw, label, vis, focused, hov);
}

/* Draw a small system-tray area on the right side of the taskbar:
 *   [HH:MM] [mem bar] */
void taskbar_draw_tray(void) {
    uint64_t fb_w = console_fb_width();
    uint64_t fb_h = console_fb_height();
    uint64_t fw   = console_font_width();
    uint64_t fh   = console_font_height();
    uint64_t ty   = fb_h - TASKBAR_H;
    uint32_t bg   = COL_TASKBAR;

    /* ── Clock ── */
    uint8_t rh = 0, rm = 0, rs = 0;
    rtc_get_time(&rh, &rm, &rs);
    {
        int32_t adj = (int32_t)rh + (int32_t)g_theme.utc_offset;
        adj = ((adj % 24) + 24) % 24;
        rh = (uint8_t)adj;
    }

    /* Build clock string: HH:MM:SS (24h) or H:MM:SS AM/PM (12h) */
    char clk[14]; /* max "12:59:59 PM\0" = 12 chars */
    int clk_chars;
    if (g_theme.clock_12h) {
        const char *ampm = (rh < 12u) ? "AM" : "PM";
        uint8_t h12 = rh % 12u; if (h12 == 0u) h12 = 12u;
        clk[0] = (char)('0' + h12 / 10u); clk[1] = (char)('0' + h12 % 10u); clk[2] = ':';
        gui_itoa_pad2(rm, clk + 3); clk[5] = ':';
        gui_itoa_pad2(rs, clk + 6); clk[8] = ' ';
        clk[9] = ampm[0]; clk[10] = ampm[1]; clk[11] = '\0';
        clk_chars = 11;
    } else {
        gui_itoa_pad2(rh, clk + 0); clk[2] = ':';
        gui_itoa_pad2(rm, clk + 3); clk[5] = ':';
        gui_itoa_pad2(rs, clk + 6); clk[8] = '\0';
        clk_chars = 8;
    }

    uint64_t clk_w  = (uint64_t)clk_chars * fw;
    uint64_t clk_x  = fb_w > clk_w + 8u ? fb_w - clk_w - 8u : 0u;
    uint64_t clk_y  = ty + (TASKBAR_H - fh) / 2u;
    /* repaint the clock cell with the panel gradient (keeps digits crisp) */
    uint32_t clk_top = g_cal_popup_open ? 0x001b2740u : 0x00101624u;
    uint32_t clk_bot = g_cal_popup_open ? 0x00121a2eu : 0x00080b14u;
    console_fill_vgrad(clk_x > 4u ? clk_x - 4u : 0u, ty + 1u, clk_w + 8u, TASKBAR_H - 1u,
                       clk_top, clk_bot);
    gui_draw_str_fg(clk_x, clk_y, clk, 0x00b8d8f4u);
    /* expose the clock hit region so a click opens the calendar */
    g_clk_x = clk_x > 4u ? clk_x - 4u : 0u;
    g_clk_w = clk_w + 8u;
    (void)bg;

    /* (Memory usage bar removed per user request — it read as a separator bar.) */

    /* ── Gamepad indicator (shown when gamepad connected) ── */
    uint64_t tray_right = clk_x > 8u ? clk_x - 8u : 0u;
    {
        extern bool input_gamepad_connected(void);
        if (input_gamepad_connected()) {
            static const char *gp_lbl = "GP";
            uint64_t gpl = 2u;
            uint64_t gpw = gpl * fw + 8u;
            uint64_t gpx = tray_right > gpw ? tray_right - gpw : 0u;
            console_fill_rect(gpx, ty + 3u, gpw, TASKBAR_H - 6u, 0x00102820u);
            gui_draw_str(gpx + 4u, ty + (TASKBAR_H - fh) / 2u, gp_lbl, 0x0050e880u, 0x00102820u);
            tray_right = gpx > 4u ? gpx - 4u : 0u;
        }
    }

    /* ── Network indicator ── */
    {
        bool has_nic = net_nic_present();
        bool has_ip  = (net_ip != 0);
        const char *net_lbl = has_ip ? "LAN" : (has_nic ? "NIC" : "---");
        uint32_t net_bg  = has_ip  ? 0x00102820u : (has_nic ? 0x00201010u : 0x00141414u);
        uint32_t net_fg  = has_ip  ? 0x0050e880u : (has_nic ? 0x00e07050u : COL_TASKBAR);
        uint64_t nw      = 3u * fw + 8u;
        uint64_t nx      = tray_right > nw ? tray_right - nw : 0u;
        console_fill_rect(nx, ty + 3u, nw, TASKBAR_H - 6u, net_bg);
        if (has_nic || has_ip)
            gui_draw_str(nx + 4u, ty + (TASKBAR_H - fh) / 2u, net_lbl, net_fg, net_bg);
        g_net_tray_x = nx; g_net_tray_w = nw;
        tray_right = nx > 4u ? nx - 4u : 0u;
    }

    /* ── FPS counter (shown in gaming mode, left of network indicator) ── */
    uint64_t fps_right_edge = tray_right;
    {
        extern uint32_t compositor_fps(void);
        extern bool gaming_mode_active(void);
        if (gaming_mode_active()) {
            uint32_t fps = compositor_fps();
            uint32_t fps_for_color = fps;
            char ftxt[8]; int fi = 0;
            if (fps >= 1000) { ftxt[fi++]='9'; ftxt[fi++]='9'; ftxt[fi++]='9'; }
            else if (fps >= 100) { ftxt[fi++]=(char)('0'+fps/100); fps%=100; ftxt[fi++]=(char)('0'+fps/10); ftxt[fi++]=(char)('0'+fps%10); }
            else if (fps >= 10)  { ftxt[fi++]=(char)('0'+fps/10);  ftxt[fi++]=(char)('0'+fps%10); }
            else                 { ftxt[fi++]=(char)('0'+fps); }
            ftxt[fi++]='f'; ftxt[fi++]='p'; ftxt[fi++]='s'; ftxt[fi]='\0';
            uint64_t fw2 = (uint64_t)fi * fw + 8u;
            uint64_t fx  = fps_right_edge > fw2 ? fps_right_edge - fw2 : 0u;
            uint32_t fbg = 0x00101828u;
            uint32_t ffg = fps_for_color >= 60u ? 0x0050e880u : fps_for_color >= 30u ? 0x00e8c040u : 0x00e86040u;
            console_fill_rect(fx, ty + 3u, fw2, TASKBAR_H - 6u, fbg);
            gui_draw_str(fx + 4u, ty + (TASKBAR_H - fh) / 2u, ftxt, ffg, fbg);
            fps_right_edge = fx > 4u ? fx - 4u : 0u;
        }
    }

    /* ── Volume tray icon (left of memory bar / FPS counter) ── */
    {
        int vol = hda_is_ready() ? hda_get_volume() : -1;
        char vtxt[6]; int vi = 0;
        if (vol < 0) {
            vtxt[vi++] = '-'; vtxt[vi++] = '-'; vtxt[vi++] = '%';
        } else if (vol >= 100) {
            vtxt[vi++] = '1'; vtxt[vi++] = '0'; vtxt[vi++] = '0'; vtxt[vi++] = '%';
        } else {
            if (vol >= 10) vtxt[vi++] = (char)('0' + vol / 10);
            vtxt[vi++] = (char)('0' + vol % 10);
            vtxt[vi++] = '%';
        }
        vtxt[vi] = '\0';
        uint64_t vw  = (uint64_t)vi * fw + 8u;
        uint64_t vx  = fps_right_edge > vw + 4u ? fps_right_edge - vw - 4u : 0u;
        uint64_t vy  = ty + (TASKBAR_H - fh) / 2u;
        uint32_t vbg = g_vol_popup_open ? g_theme.accent : 0x00141e2au;
        console_fill_rect(vx, ty + 3u, vw, TASKBAR_H - 6u, vbg);
        uint32_t vfg = g_vol_popup_open ? 0x00ffffffu : 0x0090b8d8u;
        gui_draw_str(vx + 4u, vy, vtxt, vfg, vbg);
        g_vol_tray_x = vx;
        g_vol_tray_w = vw;
    }

    /* (CPU usage bar removed per user request — it read as a separator bar.) */
    uint64_t left_edge = g_vol_tray_x > 6u ? g_vol_tray_x - 6u : 0u;
    g_cpu_tray_x = 0; g_cpu_tray_w = 0;

    /* ── Battery (laptops only) — glyph fill + charging bolt ── */
    g_batt_present = false; g_batt_x = 0; g_batt_w = 0;
    if (battery_present && battery_present()) {
        g_batt_present = true;
        int pct = battery_percent ? battery_percent() : -1;
        bool chg = battery_charging && battery_charging();
        if (pct < 0) pct = 0; if (pct > 100) pct = 100;
        uint64_t bodyw = 24u, bodyh = 12u, nub = 2u;
        uint64_t total = bodyw + nub;
        uint64_t bxx = left_edge > total ? left_edge - total : 0u;
        uint64_t byy = ty + (TASKBAR_H - bodyh) / 2u;
        uint32_t oc = 0x00b8c8dcu;
        console_fill_rect(bxx, byy, bodyw, 1u, oc);
        console_fill_rect(bxx, byy + bodyh - 1u, bodyw, 1u, oc);
        console_fill_rect(bxx, byy, 1u, bodyh, oc);
        console_fill_rect(bxx + bodyw - 1u, byy, 1u, bodyh, oc);
        console_fill_rect(bxx + bodyw, byy + (bodyh - 6u) / 2u, nub, 6u, oc);
        uint32_t fc = chg ? 0x0050d090u : (pct <= 15 ? 0x00e05050u : pct <= 35 ? 0x00e8c040u : 0x0060c860u);
        uint64_t innerw = bodyw - 4u;
        uint64_t fillw = (uint64_t)pct * innerw / 100u;
        if (fillw > 0u) console_fill_rect(bxx + 2u, byy + 2u, fillw, bodyh - 4u, fc);
        if (chg) {   /* white lightning bolt centered on the battery */
            uint64_t cxb = bxx + bodyw / 2u, cyb = byy + 2u;
            uint32_t blt = 0x00ffffffu;
            console_fill_rect(cxb + 1u, cyb,      2u, 3u, blt);
            console_fill_rect(cxb - 2u, cyb + 3u, 5u, 1u, blt);
            console_fill_rect(cxb - 2u, cyb + 4u, 2u, 3u, blt);
        }
        g_batt_x = bxx; g_batt_w = bodyw + nub;
        left_edge = bxx > 8u ? bxx - 8u : 0u;
    }
}

/* ── System tray indicators state + hover tooltips ───────────────────── */
static int cal_dow(int y, int m, int d);   /* defined in the calendar section below */
bool     g_batt_present = false;
uint64_t g_batt_x = 0, g_batt_w = 0;
uint64_t g_cpu_tray_x = 0, g_cpu_tray_w = 0;
uint64_t g_net_tray_x = 0, g_net_tray_w = 0;
uint64_t g_mem_tray_x = 0, g_mem_tray_w = 0;
int      g_tray_hover = TRAY_NONE;

static void tray_item_region(int id, uint64_t *x, uint64_t *w) {
    switch (id) {
        case TRAY_BATT: *x = g_batt_x;     *w = g_batt_w;     break;
        case TRAY_CPU:  *x = g_cpu_tray_x; *w = g_cpu_tray_w; break;
        case TRAY_NET:  *x = g_net_tray_x; *w = g_net_tray_w; break;
        case TRAY_MEM:  *x = g_mem_tray_x; *w = g_mem_tray_w; break;
        case TRAY_VOL:  *x = g_vol_tray_x; *w = g_vol_tray_w; break;
        case TRAY_CLK:  *x = g_clk_x;      *w = g_clk_w;      break;
        default:        *x = 0;            *w = 0;            break;
    }
}

int tray_item_at(int32_t mx, int32_t my) {
    uint64_t ty = console_fb_height() - TASKBAR_H;
    if ((uint64_t)my < ty) return TRAY_NONE;
    static const int ids[6] = { TRAY_BATT, TRAY_CPU, TRAY_NET, TRAY_MEM, TRAY_VOL, TRAY_CLK };
    for (int k = 0; k < 6; k++) {
        if (ids[k] == TRAY_BATT && !g_batt_present) continue;
        uint64_t x, w; tray_item_region(ids[k], &x, &w);
        if (w > 0u && (uint64_t)mx >= x && (uint64_t)mx < x + w) return ids[k];
    }
    return TRAY_NONE;
}

static void tray_tip_text(int id, char *buf, int n) {
    buf[0] = '\0';
    switch (id) {
    case TRAY_BATT: {
        int pct = battery_percent ? battery_percent() : -1;
        bool chg = battery_charging && battery_charging();
        int m = battery_minutes ? battery_minutes() : -1;
        if (chg) {
            if (m > 0) snprintf(buf, n, "Charging - %d%% (%dh %02dm to full)", pct, m / 60, m % 60);
            else       snprintf(buf, n, "Charging - %d%%", pct);
        } else {
            if (m > 0) snprintf(buf, n, "%d%% - %dh %02dm remaining", pct, m / 60, m % 60);
            else       snprintf(buf, n, "%d%% remaining", pct);
        }
        break; }
    case TRAY_CPU: { int c = cpu_usage_percent ? cpu_usage_percent() : -1; snprintf(buf, n, "CPU: %d%%", c < 0 ? 0 : c); break; }
    case TRAY_NET:
        if (net_ip) { char ip[20]; gui_ip4_str(net_ip, ip, (int)sizeof ip); snprintf(buf, n, "Network: %s", ip); }
        else snprintf(buf, n, "Network: disconnected");
        break;
    case TRAY_MEM: {
        uint64_t tp = pmm_get_total_pages(), fp = pmm_get_free_pages();
        unsigned long long usedMB = (unsigned long long)(tp - fp) * 4096ull / 1048576ull;
        unsigned long long totMB  = (unsigned long long)tp * 4096ull / 1048576ull;
        snprintf(buf, n, "Memory: %llu / %llu MB", usedMB, totMB);
        break; }
    case TRAY_VOL: { int v = hda_is_ready() ? hda_get_volume() : -1; if (v < 0) snprintf(buf, n, "Volume: n/a"); else snprintf(buf, n, "Volume: %d%%", v); break; }
    case TRAY_CLK: {
        uint8_t d = 1, mo = 1; uint16_t y = 2026; rtc_get_date(&d, &mo, &y);
        static const char *wd[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
        static const char *mnames[] = { "January", "February", "March", "April", "May", "June",
            "July", "August", "September", "October", "November", "December" };
        if (mo < 1 || mo > 12) mo = 1;
        int dow = cal_dow((int)y, (int)mo, (int)d);
        if (dow < 0 || dow > 6) dow = 0;
        snprintf(buf, n, "%s, %s %d, %d", wd[dow], mnames[mo - 1], d, y);
        break; }
    default: break;
    }
}

void tray_tip_draw(void) {
    if (g_tray_hover < 0) return;
    char buf[96]; tray_tip_text(g_tray_hover, buf, (int)sizeof buf);
    if (!buf[0]) return;
    uint64_t fw = console_font_width(), fh = console_font_height();
    uint64_t ty = console_fb_height() - TASKBAR_H;
    uint64_t tlen = (uint64_t)gui_strlen(buf);
    uint64_t tw = tlen * fw + 16u, th = fh + 10u;
    uint64_t ix, iw; tray_item_region(g_tray_hover, &ix, &iw);
    uint64_t center = ix + iw / 2u;
    uint64_t px = center > tw / 2u ? center - tw / 2u : 0u;
    uint64_t fbw = console_fb_width();
    if (px + tw > fbw) px = fbw > tw ? fbw - tw : 0u;
    uint64_t py = ty > th + 6u ? ty - th - 6u : 0u;
    console_fill_rect(px, py, tw, th, 0x000e1622u);
    console_fill_rect(px, py, tw, 1u, 0x00304a70u);
    console_fill_rect(px, py + th - 1u, tw, 1u, 0x00223048u);
    console_fill_rect(px, py, 1u, th, 0x00223048u);
    console_fill_rect(px + tw - 1u, py, 1u, th, 0x00223048u);
    gui_draw_str_fg(px + 8u, py + (th - fh) / 2u, buf, 0x00d6e6f7u);
}

/* Icon path + label for a unified favbar index (built-ins first, then user favs). */
static const char *favbar_icon_path(int i) {
    if (i < 0) return NULL;
    if (i < FAVBAR_BUILTINS) return g_fav_builtin[i].icon;
    int j = i - FAVBAR_BUILTINS;
    return (j >= 0 && j < g_fav_count) ? g_favs[j].path : NULL;
}
static const char *favbar_label(int i) {
    if (i < 0) return "";
    if (i < FAVBAR_BUILTINS) return g_fav_builtin[i].label;
    int j = i - FAVBAR_BUILTINS;
    return (j >= 0 && j < g_fav_count) ? g_favs[j].label : "";
}

/* Case-insensitive "does one string contain the other" (min 3 chars). */
static bool ci_related(const char *a, const char *b) {
    if (!a || !b || !a[0] || !b[0]) return false;
    char la[24], lb[24];
    int i = 0; for (; a[i] && i < 23; i++) la[i] = (a[i] >= 'A' && a[i] <= 'Z') ? (char)(a[i] + 32) : a[i]; la[i] = 0;
    int k = 0; for (; b[k] && k < 23; k++) lb[k] = (b[k] >= 'A' && b[k] <= 'Z') ? (char)(b[k] + 32) : b[k]; lb[k] = 0;
    if (i < 3 || k < 3) return false;
    const char *hay = (i >= k) ? la : lb, *ndl = (i >= k) ? lb : la;
    for (const char *h = hay; *h; h++) {
        const char *x = h, *y = ndl;
        while (*x && *y && *x == *y) { x++; y++; }
        if (!*y) return true;
    }
    return false;
}

/* Is the app behind favbar entry `i` currently open? Built-ins map to a window
 * slot (exact); user favorites match by title against open IPC/browser windows. */
static bool favbar_running(int i) {
    if (i < FAVBAR_BUILTINS) {
        int s = g_fav_builtin[i].slot;
        return g_wins[s].active && g_wins[s].state != WIN_HIDDEN;
    }
    const char *lbl = favbar_label(i);
    __attribute__((weak)) int  ipc_window_count(void);
    __attribute__((weak)) bool ipc_window_info(int slot, char *title, int max, bool *focused);
    if (ipc_window_count && ipc_window_info) {
        int n = ipc_window_count();
        for (int wi = 0; wi < n && wi < 8; wi++) {
            char t[24] = ""; bool f = false;
            ipc_window_info(wi, t, (int)sizeof(t), &f);
            if (ci_related(t, lbl)) return true;
        }
    }
#ifdef __linux__
    {
        __attribute__((weak)) bool wayland_browser_present(void);
        __attribute__((weak)) const char *wayland_browser_title(void);
        if (wayland_browser_present && wayland_browser_present()) {
            const char *bt = (wayland_browser_title) ? wayland_browser_title() : "Browser";
            if (ci_related(bt, lbl) || ci_related("browser", lbl)) return true;
        }
    }
#endif
    return false;
}
/* Built-in window that is the topmost (focused) window. */
static bool favbar_focused(int i) {
    if (i >= FAVBAR_BUILTINS) return false;
    int s = g_fav_builtin[i].slot;
    return g_wins[s].active && g_wins[s].state != WIN_HIDDEN && g_z[MAX_WINS - 1] == s;
}

/* Favorite app icons, decoded once and cached by resolved path. Falls back to a
 * lettered tile. Linux-only (weak PNG loader). */
#ifdef __linux__
__attribute__((weak)) uint32_t *fifi_load_png(const char *path, uint32_t *w, uint32_t *h);
typedef struct { char path[192]; uint32_t *img; uint32_t w, h; bool tried; } favicon_t;
static favicon_t g_favicon[FAVBAR_BUILTINS + FAV_MAX];
static uint32_t *fav_icon(int i, uint32_t *ow, uint32_t *oh) {
    if (!fifi_load_png || i < 0 || i >= FAVBAR_BUILTINS + FAV_MAX) return NULL;
    const char *p = favbar_icon_path(i);
    if (!p || !p[0]) return NULL;
    favicon_t *c = &g_favicon[i];
    if (strncmp(c->path, p, sizeof(c->path)) != 0) {
        if (c->img) { free(c->img); c->img = NULL; }
        c->tried = false;
        strncpy(c->path, p, sizeof(c->path) - 1);
        c->path[sizeof(c->path) - 1] = '\0';
    }
    if (!c->tried) {
        c->tried = true;
        c->img = app_load_icon_png(p, &c->w, &c->h);
    }
    if (c->img) { *ow = c->w; *oh = c->h; return c->img; }
    return NULL;
}
#endif

void favbar_draw(void) {
    uint64_t fw  = console_font_width();
    uint64_t fh  = console_font_height();
    uint64_t ty  = console_fb_height() - TASKBAR_H;
    uint64_t fbw = fav_btn_w();
    uint64_t sx  = favbar_start_x();
    int n = favbar_count();
    for (int i = 0; i < n; i++) {
        uint64_t bx  = sx + (uint64_t)i * (fbw + TASKBTN_GAP);
        bool     hov = (g_fav_hover == i);
        bool     run = favbar_running(i);
        bool     foc = favbar_focused(i);
        /* Background: brighter when focused, then hovered, then merely running. */
        uint32_t top = foc ? 0x00335a94u : hov ? 0x002c3a52u : run ? 0x00243a52u : 0x00202a3cu;
        uint32_t bot = foc ? 0x00244572u : hov ? 0x00202c40u : run ? 0x0019283au : 0x00161d2cu;
        console_fill_vgrad(bx, ty + 3u, fbw, TASKBAR_H - 6u, top, bot);
        /* icon or lettered fallback */
        uint64_t isz = (TASKBAR_H > 14u) ? TASKBAR_H - 12u : 8u;
        uint64_t ix  = bx + (fbw > isz ? (fbw - isz) / 2u : 0u);
        uint64_t iy  = ty + (TASKBAR_H > isz ? (TASKBAR_H - isz) / 2u : 0u);
        bool drew = false;
#ifdef __linux__
        {
            uint32_t iw = 0, ih = 0;
            uint32_t *img = fav_icon(i, &iw, &ih);
            if (img) { console_blit_scaled_alpha(img, iw, ih, ix, iy, isz, isz); drew = true; }
        }
#endif
        if (!drew) {
            const char *lbl = favbar_label(i);
            char L[2] = { lbl[0] ? lbl[0] : '?', '\0' };
            if (L[0] >= 'a' && L[0] <= 'z') L[0] = (char)(L[0] - 32);
            uint64_t lx = bx + (fbw > fw ? (fbw - fw) / 2u : 0u);
            uint64_t ly = ty + (TASKBAR_H > fh ? (TASKBAR_H - fh) / 2u : 0u);
            gui_draw_str_fg(lx, ly, L, hov ? 0x00f0f6ffu : 0x0090b8e0u);
        }
        /* Running/open indicator: an underline bar (wider + brighter when focused). */
        if (run) {
            uint64_t uw = foc ? fbw - 10u : fbw / 2u;
            uint64_t ux = bx + (fbw > uw ? (fbw - uw) / 2u : 0u);
            console_fill_rect(ux, ty + TASKBAR_H - 3u, uw, 2u,
                              foc ? 0x0078b4ffu : 0x00507fb0u);
        }
    }
}

int favbar_hit(int32_t mx, int32_t my) {
    uint64_t ty = console_fb_height() - TASKBAR_H;
    if ((uint64_t)my < ty) return -1;
    uint64_t fbw = fav_btn_w();
    uint64_t sx  = favbar_start_x();
    int n = favbar_count();
    for (int i = 0; i < n; i++) {
        uint64_t bx = sx + (uint64_t)i * (fbw + TASKBTN_GAP);
        if ((uint64_t)mx >= bx && (uint64_t)mx < bx + fbw) return i;
    }
    return -1;
}

void taskbar_draw(void) {
    uint64_t fb_w = console_fb_width();
    uint64_t fb_h = console_fb_height();
    uint64_t fw   = console_font_width();
    uint64_t fh   = console_font_height();
    uint64_t ty   = fb_h - TASKBAR_H;

    /* Panel: subtle vertical gradient with a hairline accent on top */
    console_fill_vgrad(0, ty, fb_w, TASKBAR_H, 0x00101624u, 0x00080b14u);
    console_fill_rect(0, ty, fb_w, 1u, 0x00223350u);

    /* Launcher button: accent gradient pill */
    uint64_t lw = logo_eff_w();
    uint32_t lg_top = g_launcher_open ? 0x00548ae0u : 0x002d54a8u;
    uint32_t lg_bot = g_launcher_open ? 0x003a6cc4u : 0x001f3c80u;
    console_fill_vgrad(LOGO_X, ty + 3u, lw, TASKBAR_H - 6u, lg_top, lg_bot);
    {   /* pill corner softening */
        uint32_t base = 0x000b0f1au;
        uint64_t ph = TASKBAR_H - 6u;
        console_fill_rect(LOGO_X,           ty + 3u,      2u, 1u, base);
        console_fill_rect(LOGO_X,           ty + 4u,      1u, 1u, base);
        console_fill_rect(LOGO_X + lw - 2u, ty + 3u,      2u, 1u, base);
        console_fill_rect(LOGO_X + lw - 1u, ty + 4u,      1u, 1u, base);
        console_fill_rect(LOGO_X,           ty + 2u + ph, 2u, 1u, base);
        console_fill_rect(LOGO_X,           ty + 1u + ph, 1u, 1u, base);
        console_fill_rect(LOGO_X + lw - 2u, ty + 2u + ph, 2u, 1u, base);
        console_fill_rect(LOGO_X + lw - 1u, ty + 1u + ph, 1u, 1u, base);
    }
    const char *logo = "FiFi OS";
    uint64_t llen = (uint64_t)gui_strlen(logo);
    uint64_t lpx  = LOGO_X + (lw - llen * fw) / 2u;
    uint64_t lpy  = ty + (TASKBAR_H > fh ? (TASKBAR_H - fh) / 2u : 0u);
    gui_draw_str_fg(lpx, lpy, logo, 0x00f2f7ffu);

    /* The favorites strip now holds the built-in launchers (Terminal/Files/
     * Settings/Viewer) plus user-pinned apps, each with a running indicator. */
    favbar_draw();

    /* ── Running IPC app windows (labeled pills, after the favorites strip) ── */
    __attribute__((weak)) int  ipc_window_count(void);
    __attribute__((weak)) bool ipc_window_info(int slot, char *title, int max, bool *focused);
    if (ipc_window_count && ipc_window_info) {
        int nipc = ipc_window_count();
        for (int wi = 0; wi < nipc && wi < 8; wi++) {
            char ipc_title[20] = "App";
            bool ipc_focused = false;
            ipc_window_info(wi, ipc_title, (int)sizeof(ipc_title), &ipc_focused);
            uint64_t ibtw = taskbtn_w();
            uint64_t bx = taskbtn_start_x() + (uint64_t)wi * (ibtw + TASKBTN_GAP);
            taskbar_pill(bx, ty, ibtw, ipc_title, true, ipc_focused, false);
        }
    }

    /* ── Wayland windows: one task button per toplevel (after IPC buttons) ── */
#ifdef __linux__
    {
        __attribute__((weak)) int  wayland_toplevel_count(void);
        __attribute__((weak)) bool wayland_toplevel_info(int, char *, int, bool *);
        if (wayland_toplevel_count && wayland_toplevel_info) {
            int nipc = (ipc_window_count) ? ipc_window_count() : 0;
            int base = (nipc < 8 ? nipc : 8);
            uint64_t ibtw = taskbtn_w();
            int nwl = wayland_toplevel_count();
            for (int wi = 0; wi < nwl && base + wi < 12; wi++) {
                char t[24] = "App"; bool f = false;
                wayland_toplevel_info(wi, t, (int)sizeof(t), &f);
                uint64_t bx = taskbtn_start_x() + (uint64_t)(base + wi) * (ibtw + TASKBTN_GAP);
                taskbar_pill(bx, ty, ibtw, t, true, f, false);
            }
        }
    }
#endif

    taskbar_draw_tray();
}

/* ── Volume tray popup ───────────────────────────────────────────────── */
#define VOL_POP_W 170u

void vol_popup_draw(void) {
    uint64_t fb_w = console_fb_width();
    uint64_t fb_h = console_fb_height();
    uint64_t fw   = console_font_width();
    uint64_t fh   = console_font_height();
    uint64_t ty   = fb_h - TASKBAR_H;

    /* Height: top-pad + title + gap + btn-row + bot-pad */
    uint64_t btn_h  = fh + 4u;
    uint64_t pop_h  = 6u + fh + 6u + btn_h + 8u;
    g_vol_pop_h = pop_h;

    /* Right-align popup to tray icon right edge */
    uint64_t pop_right = (g_vol_tray_w > 0u) ? (g_vol_tray_x + g_vol_tray_w) : (fb_w - 4u);
    uint64_t px = (pop_right > VOL_POP_W) ? (pop_right - VOL_POP_W) : 0u;
    if (px + VOL_POP_W > fb_w) px = fb_w - VOL_POP_W;
    uint64_t py = (ty > pop_h + 4u) ? (ty - pop_h - 4u) : 0u;
    g_vol_pop_x = px;
    g_vol_pop_y = py;

    /* Background + accent border */
    uint32_t pbg = 0x00101828u;
    uint32_t pbo = g_theme.accent;
    console_fill_rect(px, py, VOL_POP_W, pop_h, pbg);
    console_fill_rect(px, py, VOL_POP_W, 1u, pbo);
    console_fill_rect(px, py + pop_h - 1u, VOL_POP_W, 1u, pbo);
    console_fill_rect(px, py, 1u, pop_h, pbo);
    console_fill_rect(px + VOL_POP_W - 1u, py, 1u, pop_h, pbo);

    /* Title: "Volume: XX%" */
    int vol = hda_get_volume();
    char title[16]; int ti = 0;
    const char *tlbl = "Volume: ";
    for (int k = 0; tlbl[k]; k++) title[ti++] = tlbl[k];
    if (vol >= 100) { title[ti++] = '1'; title[ti++] = '0'; title[ti++] = '0'; }
    else            { if (vol >= 10) title[ti++] = (char)('0' + vol / 10); title[ti++] = (char)('0' + vol % 10); }
    title[ti++] = '%'; title[ti] = '\0';
    uint64_t tlen = (uint64_t)ti;
    uint64_t ttx  = px + (VOL_POP_W > tlen * fw ? (VOL_POP_W - tlen * fw) / 2u : 2u);
    uint64_t tty  = py + 6u;
    gui_draw_str(ttx, tty, title, 0x00c0d8f0u, pbg);

    /* Control row: [−] slider [+] */
    uint64_t row_y  = tty + fh + 6u;
    uint64_t pad    = 6u;
    uint64_t btn_w  = 20u;
    uint64_t slid_x = px + pad + btn_w + 4u;
    uint64_t slid_w = VOL_POP_W > 2u * pad + 2u * btn_w + 8u
                    ? VOL_POP_W - 2u * pad - 2u * btn_w - 8u : 4u;
    uint64_t slid_h = 8u;
    uint64_t slid_y = row_y + (btn_h - slid_h) / 2u;
    uint64_t mb_x   = px + pad;
    uint64_t pb_x   = px + VOL_POP_W - pad - btn_w;

    g_vol_pop_minus_x = mb_x;
    g_vol_pop_plus_x  = pb_x;
    g_vol_pop_btn_y   = row_y;
    g_vol_pop_btn_w   = btn_w;
    g_vol_pop_btn_h   = btn_h;
    g_vol_pop_slid_x  = slid_x;
    g_vol_pop_slid_w  = slid_w;

    /* [−] button */
    uint32_t bbg = 0x00182030u;
    console_fill_rect(mb_x, row_y, btn_w, btn_h, bbg);
    console_fill_rect(mb_x, row_y, btn_w, 1u, pbo);
    console_fill_rect(mb_x, row_y + btn_h - 1u, btn_w, 1u, pbo);
    console_fill_rect(mb_x, row_y, 1u, btn_h, pbo);
    console_fill_rect(mb_x + btn_w - 1u, row_y, 1u, btn_h, pbo);
    gui_draw_str(mb_x + (btn_w - fw) / 2u, row_y + (btn_h - fh) / 2u, "-", 0x00e0f0ffu, bbg);

    /* [+] button */
    console_fill_rect(pb_x, row_y, btn_w, btn_h, bbg);
    console_fill_rect(pb_x, row_y, btn_w, 1u, pbo);
    console_fill_rect(pb_x, row_y + btn_h - 1u, btn_w, 1u, pbo);
    console_fill_rect(pb_x, row_y, 1u, btn_h, pbo);
    console_fill_rect(pb_x + btn_w - 1u, row_y, 1u, btn_h, pbo);
    gui_draw_str(pb_x + (btn_w - fw) / 2u, row_y + (btn_h - fh) / 2u, "+", 0x00e0f0ffu, bbg);

    /* Slider track */
    console_fill_rect(slid_x, slid_y, slid_w, slid_h, 0x00080c14u);
    console_fill_rect(slid_x, slid_y, slid_w, 1u, 0x00202838u);
    console_fill_rect(slid_x, slid_y + slid_h - 1u, slid_w, 1u, 0x00202838u);
    console_fill_rect(slid_x, slid_y, 1u, slid_h, 0x00202838u);
    console_fill_rect(slid_x + slid_w - 1u, slid_y, 1u, slid_h, 0x00202838u);

    /* Slider fill */
    if (vol > 0 && slid_w > 2u) {
        uint64_t fill = (uint64_t)(vol) * (slid_w - 2u) / 100u;
        if (fill > slid_w - 2u) fill = slid_w - 2u;
        if (fill > 0u)
            console_fill_rect(slid_x + 1u, slid_y + 1u, fill, slid_h - 2u, g_theme.accent);
    }
}

/* ── Clock / calendar popup ──────────────────────────────────────────── */
bool     g_cal_popup_open = false;
uint64_t g_clk_x = 0, g_clk_w = 0;
uint64_t g_cal_pop_x = 0, g_cal_pop_y = 0, g_cal_pop_w = 0, g_cal_pop_h = 0;
int      g_cal_view_mon = 0, g_cal_view_year = 0;   /* 0 = uninitialized */
bool     g_cal_pick_open = false;
uint64_t g_cal_prev_bx = 0, g_cal_next_bx = 0, g_cal_arrow_by = 0, g_cal_arrow_bw = 0, g_cal_arrow_bh = 0;
uint64_t g_cal_hdr_bx = 0, g_cal_hdr_bw = 0, g_cal_hdr_by = 0, g_cal_hdr_bh = 0;
uint64_t g_cal_yr_prev_bx = 0, g_cal_yr_next_bx = 0, g_cal_yr_by = 0, g_cal_yr_bw = 0, g_cal_yr_bh = 0;
uint64_t g_cal_mgrid_x = 0, g_cal_mgrid_y = 0, g_cal_mcell_w = 0, g_cal_mcell_h = 0;

static const char *cal_month_names[] = { "January", "February", "March", "April",
    "May", "June", "July", "August", "September", "October", "November", "December" };
static const char *cal_month_abbr[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* Sakamoto's day-of-week: 0=Sunday .. 6=Saturday */
static int cal_dow(int y, int m, int d) {
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    int r = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    return (r < 0) ? r + 7 : r;
}
static int cal_days_in_month(int m, int y) {
    static const int dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    if (m < 1 || m > 12) return 30;
    return dm[m - 1];
}

void cal_popup_draw(void) {
    uint64_t fb_w = console_fb_width();
    uint64_t fb_h = console_fb_height();
    uint64_t fw   = console_font_width();
    uint64_t fh   = console_font_height();
    uint64_t ty   = fb_h - TASKBAR_H;

    uint8_t td = 1, tmon = 1; uint16_t tyr = 2026;
    rtc_get_date(&td, &tmon, &tyr);
    if (tmon < 1) tmon = 1; if (tmon > 12) tmon = 12;
    /* g_cal_view_* are seeded by the clock-click handler; guard anyway */
    if (g_cal_view_mon < 1 || g_cal_view_mon > 12) g_cal_view_mon = tmon;
    if (g_cal_view_year <= 0) g_cal_view_year = tyr;
    int vmon = g_cal_view_mon, vyr = g_cal_view_year;

    uint64_t cell_w = fw * 3u + 2u;
    uint64_t cell_h = fh + 4u;
    uint64_t pad    = 8u;
    uint64_t grid_w = 7u * cell_w;
    uint64_t pop_w  = grid_w + 2u * pad;
    uint64_t header_h = fh + 8u, wk_h = fh + 2u;
    uint64_t pop_h  = pad + header_h + 4u + wk_h + 6u * cell_h + pad;

    uint64_t pop_right = (g_clk_w > 0u) ? (g_clk_x + g_clk_w) : (fb_w - 4u);
    uint64_t px = (pop_right > pop_w) ? (pop_right - pop_w) : 0u;
    if (px + pop_w > fb_w) px = fb_w - pop_w;
    uint64_t py = (ty > pop_h + 4u) ? (ty - pop_h - 4u) : 0u;
    g_cal_pop_x = px; g_cal_pop_y = py; g_cal_pop_w = pop_w; g_cal_pop_h = pop_h;

    /* Background + accent border */
    uint32_t pbo = g_theme.accent;
    console_fill_vgrad(px, py, pop_w, pop_h, 0x00141d30u, 0x000c1220u);
    console_fill_rect(px, py, pop_w, 1u, pbo);
    console_fill_rect(px, py + pop_h - 1u, pop_w, 1u, pbo);
    console_fill_rect(px, py, 1u, pop_h, pbo);
    console_fill_rect(px + pop_w - 1u, py, 1u, pop_h, pbo);

    /* ── Header row: [<]  Month YYYY  [>]  (month name is clickable) ── */
    uint64_t hrow_y = py + pad;
    uint64_t aw = fw + 10u, ah = header_h;
    g_cal_prev_bx = px + pad;            g_cal_next_bx = px + pop_w - pad - aw;
    g_cal_arrow_by = hrow_y;             g_cal_arrow_bw = aw; g_cal_arrow_bh = ah;
    /* arrow buttons */
    for (int a = 0; a < 2; a++) {
        uint64_t abx = a ? g_cal_next_bx : g_cal_prev_bx;
        console_fill_vgrad(abx, hrow_y, aw, ah, 0x0024344cu, 0x001a2638u);
        const char *ac = a ? ">" : "<";
        gui_draw_str_fg(abx + (aw > fw ? (aw - fw) / 2u : 0u),
                        hrow_y + (ah > fh ? (ah - fh) / 2u : 0u), ac, 0x00b8d8f4u);
    }
    /* month + year label, centered and clickable */
    char hdr[24]; int hi = 0;
    const char *ms = cal_month_names[vmon - 1];
    for (int k = 0; ms[k] && hi < 15; k++) hdr[hi++] = ms[k];
    hdr[hi++] = ' ';
    hdr[hi++] = (char)('0' + (vyr / 1000) % 10);
    hdr[hi++] = (char)('0' + (vyr / 100) % 10);
    hdr[hi++] = (char)('0' + (vyr / 10) % 10);
    hdr[hi++] = (char)('0' + vyr % 10);
    hdr[hi] = '\0';
    uint64_t hlen = (uint64_t)hi * fw;
    uint64_t hx = px + (pop_w > hlen ? (pop_w - hlen) / 2u : 0u);
    g_cal_hdr_bx = g_cal_prev_bx + aw; g_cal_hdr_bw = g_cal_next_bx - (g_cal_prev_bx + aw);
    g_cal_hdr_by = hrow_y;             g_cal_hdr_bh = ah;
    gui_draw_str_fg(hx, hrow_y + (ah > fh ? (ah - fh) / 2u : 0u), hdr,
                    g_cal_pick_open ? 0x0080b4ffu : 0x00e6ecf7u);

    /* ── Month/year picker overlay ── */
    if (g_cal_pick_open) {
        uint64_t body_y = hrow_y + header_h + 4u;
        /* Year stepper: [<] YYYY [>] */
        uint64_t yb_w = fw + 10u, yb_h = fh + 6u;
        uint64_t yrow_y = body_y;
        char ys[5]; ys[0]=(char)('0'+(vyr/1000)%10); ys[1]=(char)('0'+(vyr/100)%10);
        ys[2]=(char)('0'+(vyr/10)%10); ys[3]=(char)('0'+vyr%10); ys[4]='\0';
        uint64_t ys_w = 4u * fw;
        uint64_t grp_w = yb_w + 12u + ys_w + 12u + yb_w;
        uint64_t grp_x = px + (pop_w > grp_w ? (pop_w - grp_w) / 2u : 0u);
        g_cal_yr_prev_bx = grp_x; g_cal_yr_next_bx = grp_x + grp_w - yb_w;
        g_cal_yr_by = yrow_y; g_cal_yr_bw = yb_w; g_cal_yr_bh = yb_h;
        console_fill_vgrad(g_cal_yr_prev_bx, yrow_y, yb_w, yb_h, 0x0024344cu, 0x001a2638u);
        gui_draw_str_fg(g_cal_yr_prev_bx + (yb_w-fw)/2u, yrow_y + (yb_h-fh)/2u, "<", 0x00b8d8f4u);
        console_fill_vgrad(g_cal_yr_next_bx, yrow_y, yb_w, yb_h, 0x0024344cu, 0x001a2638u);
        gui_draw_str_fg(g_cal_yr_next_bx + (yb_w-fw)/2u, yrow_y + (yb_h-fh)/2u, ">", 0x00b8d8f4u);
        gui_draw_str_fg(grp_x + yb_w + 12u, yrow_y + (yb_h-fh)/2u, ys, 0x00e6ecf7u);

        /* 3x4 month grid */
        uint64_t mg_y = yrow_y + yb_h + 8u;
        uint64_t mcell_w = grid_w / 3u, mcell_h = fh + 10u;
        g_cal_mgrid_x = px + pad; g_cal_mgrid_y = mg_y; g_cal_mcell_w = mcell_w; g_cal_mcell_h = mcell_h;
        for (int m = 0; m < 12; m++) {
            int col = m % 3, row = m / 3;
            uint64_t cx = g_cal_mgrid_x + (uint64_t)col * mcell_w;
            uint64_t cy = mg_y + (uint64_t)row * mcell_h;
            bool cur = ((m + 1) == vmon);
            if (cur) console_fill_vgrad(cx + 2u, cy + 1u, mcell_w - 4u, mcell_h - 2u, 0x003a6cc8u, 0x002a4f9cu);
            const char *ab = cal_month_abbr[m];
            uint64_t al = (uint64_t)gui_strlen(ab) * fw;
            gui_draw_str_fg(cx + (mcell_w > al ? (mcell_w - al) / 2u : 0u),
                            cy + (mcell_h > fh ? (mcell_h - fh) / 2u : 0u), ab,
                            cur ? 0x00f2f7ffu : 0x00c0d0e8u);
        }
        return;   /* picker replaces the day grid */
    }

    /* ── Weekday header row ── */
    static const char *wd[] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    uint64_t gx = px + pad;
    uint64_t wy = py + pad + header_h + 4u;
    for (int c = 0; c < 7; c++) {
        uint64_t cx = gx + (uint64_t)c * cell_w;
        uint64_t twx = cx + (cell_w > 2u * fw ? (cell_w - 2u * fw) / 2u : 0u);
        uint32_t wfg = (c == 0 || c == 6) ? 0x006888a8u : 0x0090a8c8u;
        gui_draw_str_fg(twx, wy, wd[c], wfg);
    }

    /* ── Day grid (highlight today only when viewing the current month) ── */
    bool view_is_now = (vmon == (int)tmon && vyr == (int)tyr);
    int fdow = cal_dow(vyr, vmon, 1);
    int dim  = cal_days_in_month(vmon, vyr);
    uint64_t g0y = wy + wk_h;
    int day = 1;
    for (int row = 0; row < 6; row++) {
        for (int col = 0; col < 7; col++) {
            int cellidx = row * 7 + col;
            if (cellidx < fdow || day > dim) continue;
            uint64_t cx = gx + (uint64_t)col * cell_w;
            uint64_t cy = g0y + (uint64_t)row * cell_h;
            bool today = (view_is_now && day == (int)td);
            if (today)
                console_fill_vgrad(cx + 1u, cy + 1u, cell_w - 2u, cell_h - 2u,
                                   0x003a6cc8u, 0x002a4f9cu);
            char ds[3];
            if (day >= 10) { ds[0] = (char)('0' + day / 10); ds[1] = (char)('0' + day % 10); ds[2] = '\0'; }
            else           { ds[0] = (char)('0' + day); ds[1] = '\0'; }
            uint64_t dl = (uint64_t)gui_strlen(ds) * fw;
            uint64_t dx = cx + (cell_w > dl ? (cell_w - dl) / 2u : 0u);
            uint64_t dy = cy + (cell_h > fh ? (cell_h - fh) / 2u : 0u);
            uint32_t dfg = today ? 0x00f2f7ffu :
                           (col == 0 || col == 6) ? 0x0098b0d0u : 0x00c8d4f0u;
            gui_draw_str_fg(dx, dy, ds, dfg);
            day++;
        }
    }
}
