#include "gui_internal.h"

/* ── Status bar (top strip, 0..STATUS_H-1) ───────────────────────────── */

/* Slim 4-point FiFi star (logo mark) centered at (cx,cy), arm length r. */
static void draw_fifi_star(int64_t cx, int64_t cy, int64_t r, uint32_t col) {
    for (int64_t dy = -r; dy <= r; dy++) {          /* vertical diamond */
        int64_t w = r - (dy < 0 ? -dy : dy);
        if (w > 0) console_fill_rect((uint64_t)(cx - w), (uint64_t)(cy + dy), (uint64_t)(2*w + 1), 1u, col);
    }
    for (int64_t dx = -r; dx <= r; dx++) {          /* horizontal diamond */
        int64_t h = r - (dx < 0 ? -dx : dx);
        if (h > 0) console_fill_rect((uint64_t)(cx + dx), (uint64_t)(cy - h), 1u, (uint64_t)(2*h + 1), col);
    }
}

/* Horizon bar: a floating glass card at the top holding the FiFi mark, intent
 * workspace pills (Build/Research/Play), and a right status cluster
 * (wifi · battery · clock). Matches the FiFi design language. */
void draw_status_bar(void) {
    if (!g_theme.statusbar) return;
    /* Auto-hide when a window is maximized: in the normal (pre-window) pass, skip
     * drawing so the maximized window fills all the way to the top edge. The bar is
     * redrawn as an overlay (g_statusbar_overlay) only while the cursor rests at the
     * very top edge. Zero the hit rects so a click on the covered area does nothing. */
    if (!statusbar_bottom() && any_window_maximized() && !g_statusbar_overlay) {
        g_bar_search_x = 0; g_bar_search_w = 0;
        g_intent_w[0] = g_intent_w[1] = g_intent_w[2] = 0;
        return;
    }
    uint64_t fb_w = console_fb_width();
    uint64_t fw   = console_font_width();
    uint64_t fh   = console_font_height();
    uint64_t sby  = statusbar_y();

    /* Repaint the wallpaper across the reserved strip so glass never accumulates.
     * In overlay mode the bar floats over the maximized window, so blend directly
     * over whatever is beneath (no wallpaper repaint) — full_redraw repaints the
     * window under it each frame, so alpha never accumulates. */
    if (!g_statusbar_overlay)
        for (uint64_t r = 0; r < STATUS_H; r++)
            console_fill_rect(0, sby + r, fb_w, 1u, desktop_bg_at(sby + r));

    /* Floating glass card. */
    uint64_t M = 16u, H = 44u;
    uint64_t bx = M, by = sby + M, bw = (fb_w > 2u*M) ? fb_w - 2u*M : fb_w, bh = H;
    console_blend_rect(bx, by, bw, bh, 0x000c1017u, 150u);
    console_blend_rect(bx, by, bw, 1u, 0x00ffffffu, 22u);
    int R = 12;
    for (int r = 0; r < R; r++) {
        int dyk = R - r, rr = R*R - dyk*dyk; if (rr < 0) rr = 0;
        int s = 0; while ((s+1)*(s+1) <= rr) s++; int n = R - s; if (n <= 0) continue;
        uint32_t ct = desktop_bg_at(by + (uint64_t)r), cb = desktop_bg_at(by + bh - 1u - (uint64_t)r);
        console_fill_rect(bx, by + (uint64_t)r, (uint64_t)n, 1u, ct);
        console_fill_rect(bx + bw - (uint64_t)n, by + (uint64_t)r, (uint64_t)n, 1u, ct);
        console_fill_rect(bx, by + bh - 1u - (uint64_t)r, (uint64_t)n, 1u, cb);
        console_fill_rect(bx + bw - (uint64_t)n, by + bh - 1u - (uint64_t)r, (uint64_t)n, 1u, cb);
    }
    console_blend_rect(bx + (uint64_t)R, by, bw - 2u*(uint64_t)R, 1u, 0x00ffffffu, 22u);
    console_blend_rect(bx + (uint64_t)R, by + bh - 1u, bw - 2u*(uint64_t)R, 1u, 0x00ffffffu, 16u);

    uint64_t midy = by + bh/2u;
    uint64_t cy   = by + (bh > fh ? (bh - fh)/2u : 0u);
    uint64_t x    = bx + 16u;

    /* Brand: coral star + "FiFi". */
    draw_fifi_star((int64_t)(x + 7u), (int64_t)midy, 7, g_theme.accent);
    x += 20u;
    gui_draw_str_fg(x, cy, "FiFi", 0x00e9edf3u); x += 4u*fw + 12u;
    console_blend_rect(x, by + (bh - 18u)/2u, 1u, 18u, 0x00ffffffu, 26u); x += 12u;

    /* Intent workspace pills (clickable — switch the active intent). */
    static const char *pills[3] = { "Build", "Research", "Play" };
    uint64_t ph = 24u, py = by + (bh - ph)/2u;
    g_intent_y = py; g_intent_h = ph;
    for (int i = 0; i < 3; i++) {
        uint64_t plen = (uint64_t)gui_strlen(pills[i]);
        uint64_t pw = plen*fw + 26u;
        g_intent_x[i] = x; g_intent_w[i] = pw;
        if (i == g_active_intent) {
            console_fill_rect(x, py, pw, ph, g_theme.accent);
            console_fill_rect(x + 9u, midy - 2u, 5u, 5u, 0x000a0d12u);
            gui_draw_str_fg(x + 18u, cy, pills[i], 0x000a0d12u);
        } else {
            console_blend_rect(x, py, pw, ph, 0x00ffffffu, 13u);
            console_fill_rect(x + 9u, midy - 2u, 5u, 5u, 0x005a626cu);
            gui_draw_str_fg(x + 18u, cy, pills[i], 0x009da6b0u);
        }
        x += pw + 6u;
    }
    uint64_t search_start = x + 6u;   /* empty middle begins after the pills */

    /* Right cluster, laid out from the right edge inward: clock · battery · wifi. */
    uint64_t rx = bx + bw - 16u;
    uint8_t rh = 0, rm = 0, rsx = 0; rtc_get_time(&rh, &rm, &rsx);
    { int32_t adj = (int32_t)rh + (int32_t)g_theme.utc_offset; adj = ((adj%24)+24)%24; rh = (uint8_t)adj; }
    char clk[10]; int clen;
    if (g_theme.clock_12h) {
        const char *ap = (rh < 12u) ? "AM" : "PM"; uint8_t h = rh % 12u; if (!h) h = 12u;
        clk[0]='0'+h/10u; clk[1]='0'+h%10u; clk[2]=':'; gui_itoa_pad2(rm, clk+3); clk[5]=' '; clk[6]=ap[0]; clk[7]=ap[1]; clk[8]='\0'; clen=8;
    } else { gui_itoa_pad2(rh, clk); clk[2]=':'; gui_itoa_pad2(rm, clk+3); clk[5]='\0'; clen=5; }
    rx -= (uint64_t)clen*fw; gui_draw_str_fg(rx, cy, clk, 0x00e9edf3u);
    rx -= 14u;
    if (battery_present && battery_present()) {
        int pct = battery_percent ? battery_percent() : -1; if (pct < 0) pct = 0; if (pct > 100) pct = 100;
        char pb[6]; int k = 0;
        if (pct >= 100) { pb[k++]='1'; pb[k++]='0'; pb[k++]='0'; }
        else { if (pct >= 10) pb[k++]=(char)('0'+pct/10); pb[k++]=(char)('0'+pct%10); }
        pb[k++]='%'; pb[k]='\0';
        rx -= (uint64_t)k*fw; gui_draw_str_fg(rx, cy, pb, 0x00b6bcc6u); rx -= 7u;
        uint64_t gw = 22u, gh = 11u, gyy = midy - gh/2u; rx -= (gw + 3u);
        uint32_t oc = 0x0099a2acu;
        console_fill_rect(rx, gyy, gw, 1u, oc); console_fill_rect(rx, gyy+gh-1u, gw, 1u, oc);
        console_fill_rect(rx, gyy, 1u, gh, oc);  console_fill_rect(rx+gw-1u, gyy, 1u, gh, oc);
        console_fill_rect(rx+gw, gyy+(gh-5u)/2u, 2u, 5u, oc);
        uint64_t fwid = (uint64_t)pct*(gw-4u)/100u;
        if (fwid) console_fill_rect(rx+2u, gyy+2u, fwid, gh-4u, pct <= 15 ? 0x00e05050u : 0x00b6bcc6u);
        rx -= 12u;
    }
    { uint32_t wc = (net_ip != 0) ? 0x00e9edf3u : 0x005a626cu; rx -= 13u;
      console_fill_rect(rx,     midy+2u, 3u, 5u,  wc);
      console_fill_rect(rx+5u,  midy-1u, 3u, 8u,  wc);
      console_fill_rect(rx+10u, midy-5u, 3u, 12u, wc); }

    /* Search field in the empty middle — click opens the app launcher/search. */
    uint64_t search_end = rx > 14u ? rx - 14u : search_start;
    if (search_end > search_start + 10u*fw) {
        g_bar_search_x = search_start; g_bar_search_w = search_end - search_start;
        g_bar_search_y = by; g_bar_search_h = bh;
        uint64_t shh = 22u, shy = by + (bh - shh)/2u;
        extern bool g_launcher_top; extern char g_launch_q[40]; extern int g_launch_qlen;
        bool focused = g_launcher_open && g_launcher_top;
        console_blend_rect(search_start, shy, g_bar_search_w, shh, 0x00ffffffu, focused ? 14u : 8u);
        if (focused && g_launch_qlen > 0) {
            gui_draw_str_fg(search_start + 12u, cy, g_launch_q, 0x00e9edf3u);
            console_fill_rect(search_start + 12u + (uint64_t)g_launch_qlen*fw + 1u, cy, 2u, fh, g_theme.accent);
        } else {
            gui_draw_str_fg(search_start + 12u, cy, "Search", focused ? 0x009da6b0u : 0x006a707au);
            if (focused) console_fill_rect(search_start + 12u, cy, 2u, fh, g_theme.accent);
        }
    } else { g_bar_search_x = 0; g_bar_search_w = 0; }
}

/* ── Resize edge highlight ───────────────────────────────────────────── */

void draw_resize_hint(int slot, resize_dir_t dir) {
    if (slot < 0 || slot >= MAX_WINS) return;
    window_t *w = &g_wins[slot];
    if (!w->active || w->state != WIN_NORMAL) return;

    uint32_t col  = 0x0058a0e0u;
    uint64_t wx   = w->x, wy   = w->y;
    uint64_t we   = wx + w->w, wb = wy + w->h;
    uint64_t hs   = 14u;   /* handle size */
    uint64_t ht   = 2u;    /* handle thickness */

    switch (dir) {
        case RES_NW:
            console_fill_rect(wx,      wy,      hs, ht, col);
            console_fill_rect(wx,      wy,      ht, hs, col);
            break;
        case RES_NE:
            console_fill_rect(we - hs, wy,      hs, ht, col);
            console_fill_rect(we - ht, wy,      ht, hs, col);
            break;
        case RES_SW:
            console_fill_rect(wx,      wb - ht, hs, ht, col);
            console_fill_rect(wx,      wb - hs, ht, hs, col);
            break;
        case RES_SE:
            console_fill_rect(we - hs, wb - ht, hs, ht, col);
            console_fill_rect(we - ht, wb - hs, ht, hs, col);
            break;
        case RES_N:
            console_fill_rect(wx, wy,      w->w, ht, col);
            break;
        case RES_S:
            console_fill_rect(wx, wb - ht, w->w, ht, col);
            break;
        case RES_W:
            console_fill_rect(wx,      wy + TITLE_H, ht, w->h - TITLE_H, col);
            break;
        case RES_E:
            console_fill_rect(we - ht, wy + TITLE_H, ht, w->h - TITLE_H, col);
            break;
        default: break;
    }
}

/* ── Desktop icons ───────────────────────────────────────────────────── */

const char *desk_icon_basename(const char *path) {
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

/* Per-icon PNG logo cache. A logo lives next to the icon target with the same
 * basename: /path/App.sh → /path/App.png (written by appstore-install.sh).
 * Loaded once via the Linux platform PNG loader; weak so bare-metal skips it. */
__attribute__((weak)) uint32_t *fifi_load_png(const char *path, uint32_t *w, uint32_t *h);

typedef struct {
    char      path[192];
    uint32_t *img;
    uint32_t  w, h;
    bool      tried;
} icon_img_t;
static icon_img_t g_icon_imgs[DESK_ICON_MAX];

static icon_img_t *desk_icon_logo(int i) {
    if (!fifi_load_png) return NULL;
    const char *p = g_desk_icons[i].path;
    icon_img_t *c = &g_icon_imgs[i];
    if (strncmp(c->path, p, sizeof(c->path)) != 0) {
        /* path changed — reset cache slot */
        if (c->img) { free(c->img); c->img = NULL; }
        c->tried = false;
        strncpy(c->path, p, sizeof(c->path) - 1);
        c->path[sizeof(c->path) - 1] = '\0';
    }
    if (!c->tried) {
        c->tried = true;
        /* shared resolver: sibling <stem>.png, then /fifi-data/icons/<base>.png
         * — same lookup the launcher and taskbar favorites use */
        c->img = app_load_icon_png(p, &c->w, &c->h);
    }
    return c->img ? c : NULL;
}

/* X of the right-edge icon column. Normally flush-right (DESK_ICON_PAD margin),
 * but nudged left just enough that the widest label can sit CENTERED under its
 * icon without running off the screen edge (so labels never right-align). */
uint64_t desk_icon_col_x(void) {
    uint64_t fw  = console_font_width();
    uint64_t fbw = desk_right();   /* usable right edge — clears a right-edge dock */
    uint64_t flush = (fbw > DESK_ICON_W + DESK_ICON_PAD) ? fbw - DESK_ICON_W - DESK_ICON_PAD : 0u;
    uint64_t maxlw = 0;
    for (int i = 0; i < DESK_ICON_MAX; i++) {
        if (!g_desk_icons[i].active) continue;
        const char *nm = g_desk_icons[i].label[0]
                         ? g_desk_icons[i].label
                         : desk_icon_basename(g_desk_icons[i].path);
        char lb[24]; strncpy(lb, nm, sizeof(lb) - 1); lb[sizeof(lb) - 1] = '\0';
        uint64_t w = (uint64_t)gui_strlen(lb) * fw;
        if (w > maxlw) maxlw = w;
    }
    uint64_t half = maxlw / 2u, edge = 6u;
    uint64_t max_center = (fbw > half + edge) ? fbw - half - edge : 0u;  /* rightmost icon center that fits the label */
    uint64_t col_for_label = (max_center > DESK_ICON_W / 2u) ? max_center - DESK_ICON_W / 2u : 0u;
    return (flush < col_for_label) ? flush : col_for_label;  /* the more-left of the two */
}

/* Resolve icon i's top-left. Placed icons use their stored (x,y); the rest
 * auto-stack in the right-edge column (only active+unplaced icons count, so a
 * mix of dragged and default icons both lay out sensibly). */
void desk_icon_pos(int i, uint64_t *ox, uint64_t *oy) {
    if (i < 0 || i >= DESK_ICON_MAX) { *ox = 0; *oy = 0; return; }
    if (g_desk_icons[i].placed) {
        *ox = (uint64_t)(g_desk_icons[i].x < 0 ? 0 : g_desk_icons[i].x);
        *oy = (uint64_t)(g_desk_icons[i].y < 0 ? 0 : g_desk_icons[i].y);
        return;
    }
    uint64_t y = desk_top() + DESK_ICON_PAD;
    for (int j = 0; j < i; j++) {
        if (!g_desk_icons[j].active || g_desk_icons[j].placed) continue;
        y += DESK_ICON_H + DESK_ICON_PAD;
    }
    *ox = desk_icon_col_x();
    *oy = y;
}

void draw_desktop_icons(void) {
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();
    uint64_t db = desk_bot();

    for (int i = 0; i < DESK_ICON_MAX; i++) {
        if (!g_desk_icons[i].active) continue;
        uint64_t icon_x, icon_y;
        desk_icon_pos(i, &icon_x, &icon_y);
        if (icon_y + DESK_ICON_H > db) continue;   /* off the bottom of the work area */

        bool hov = (g_desk_icon_hover == i);
        bool sel = (g_desk_icon_sel   == i);

        /* Icon background */
        uint32_t bg = sel ? (g_theme.accent & 0x00ffffffu) | 0x00182030u
                     : hov ? 0x001a2840u : 0x00000000u;
        if (bg) console_fill_rect(icon_x - 2u, icon_y - 2u,
                                  DESK_ICON_W + 4u, DESK_ICON_H + 4u, bg);

        /* Icon face: real PNG logo when available, colored tile otherwise */
        const char *name = g_desk_icons[i].label[0]
                           ? g_desk_icons[i].label
                           : desk_icon_basename(g_desk_icons[i].path);
        uint64_t isz  = 40u;
        uint64_t iix  = icon_x + (DESK_ICON_W - isz) / 2u;
        uint64_t iiy  = icon_y + 4u;
        icon_img_t *logo = desk_icon_logo(i);
        if (logo) {
            console_blit_scaled_alpha(logo->img, logo->w, logo->h, iix, iiy, isz, isz);
        } else {
            /* Pick color by extension */
            const char *ext = strrchr(g_desk_icons[i].path, '.');
            uint32_t ic = 0x00304878u; /* default blue */
            const char *ic_txt = "FILE";
            if (ext) {
                if (strcasecmp(ext, ".bmp") == 0 || strcasecmp(ext, ".ppm") == 0 ||
                    strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".jpg") == 0) {
                    ic = 0x00305848u; ic_txt = "IMG";
                } else if (strcasecmp(ext, ".txt") == 0 || strcasecmp(ext, ".md") == 0 ||
                           strcasecmp(ext, ".log") == 0) {
                    ic = 0x00384860u; ic_txt = "TXT";
                } else if (strcasecmp(ext, ".sh") == 0) {
                    ic = 0x0026543cu; ic_txt = "APP";
                } else if (strcasecmp(ext, ".AppImage") == 0) {
                    ic = 0x0026543cu; ic_txt = "APP";
                }
            }
            console_fill_vgrad(iix, iiy, isz, isz, ic | 0x00101418u, ic);
            /* border */
            uint32_t ibc = (ic >> 1) | 0x00404040u;
            console_fill_rect(iix,        iiy,        isz, 1u, ibc);
            console_fill_rect(iix,        iiy+isz-1u, isz, 1u, ibc);
            console_fill_rect(iix,        iiy,        1u, isz, ibc);
            console_fill_rect(iix+isz-1u, iiy,        1u, isz, ibc);
            /* type label in center of icon */
            uint64_t tl  = (uint64_t)gui_strlen(ic_txt);
            uint64_t ttx = iix + (isz > tl * fw ? (isz - tl * fw) / 2u : 0u);
            uint64_t tty = iiy + (isz - fh) / 2u;
            gui_draw_str_fg(ttx, tty, ic_txt, 0x00d0e4f4u);
        }

        /* Label below icon. Icons are a single right-edge column, so a long name
         * may extend left of the icon cell without colliding — center it on the
         * icon and clamp to the screen so full names ("LibreOffice") stay readable. */
        char lbuf[24];
        strncpy(lbuf, name, sizeof(lbuf) - 1);
        lbuf[sizeof(lbuf) - 1] = '\0';
        uint64_t llen  = (uint64_t)gui_strlen(lbuf);
        uint64_t lwpx  = llen * fw;
        uint64_t ictr  = icon_x + DESK_ICON_W / 2u;
        uint64_t llx   = (ictr > lwpx / 2u) ? ictr - lwpx / 2u : 0u;
        if (llx + lwpx > console_fb_width())            /* clamp to right edge */
            llx = console_fb_width() > lwpx ? console_fb_width() - lwpx : 0u;
        uint64_t lly  = icon_y + isz + 8u;
        /* Plain label text — no background box or shadow behind it. */
        gui_draw_str_fg(llx, lly, lbuf, hov ? 0x00e0efffU : 0x00c0d8f0u);

    }
}

/* ── Desktop icon position helper ────────────────────────────────────── */
/* Returns icon index at (mx,my), -1 if none */
int desk_icon_at(int mx, int my) {
    uint64_t db = desk_bot();
    /* Iterate topmost-last so a dragged (placed) icon overlapping another wins
     * the hit-test; here order is fine since placed icons rarely overlap. */
    for (int i = 0; i < DESK_ICON_MAX; i++) {
        if (!g_desk_icons[i].active) continue;
        uint64_t icon_x, icon_y;
        desk_icon_pos(i, &icon_x, &icon_y);
        if (icon_y + DESK_ICON_H > db) continue;
        if ((uint64_t)mx >= icon_x - 2u && (uint64_t)mx < icon_x + DESK_ICON_W + 2u &&
            (uint64_t)my >= icon_y - 2u && (uint64_t)my < icon_y + DESK_ICON_H + 2u)
            return i;
    }
    return -1;
}

/* ── Desktop info overlay (neofetch-style) ───────────────────────────── */

void draw_desktop_info(void) {
    if (!g_theme.desktop_info) return;
    uint64_t dbot = desk_bot();
    uint64_t dt   = desk_top();
    uint64_t fw   = console_font_width();
    uint64_t fh   = console_font_height();

    /* Build info strings */
    char mem_str[32];
    {
        uint64_t total_p = pmm_get_total_pages();
        uint64_t free_p  = pmm_get_free_pages();
        uint64_t used_mb = ((total_p - free_p) * 4096u) >> 20u;
        uint64_t tot_mb  = (total_p * 4096u) >> 20u;
        char ub[8], tb[8];
        gui_itoa((int)used_mb, ub, 8); gui_itoa((int)tot_mb, tb, 8);
        int ri = 0; const char *p;
        for (p=ub; *p && ri<28; ) mem_str[ri++]=*p++;
        for (p=" / "; *p && ri<28; ) mem_str[ri++]=*p++;
        for (p=tb; *p && ri<28; ) mem_str[ri++]=*p++;
        for (p=" MB"; *p && ri<28; ) mem_str[ri++]=*p++;
        mem_str[ri] = '\0';
    }
    char ip_str[20];
    gui_ip4_str(net_ip, ip_str, 20);

    char up_str[16];
    {
        uint64_t hz2 = pit_get_hz(); if (!hz2) hz2 = 100;
        uint64_t sc  = pit_ticks() / hz2;
        uint64_t mn  = sc / 60u; sc %= 60u;
        uint64_t hr  = mn / 60u; mn %= 60u;
        char hb[4], mb2[4], sb2[4];
        gui_itoa_pad2((int)hr,  hb); gui_itoa_pad2((int)mn, mb2); gui_itoa_pad2((int)sc, sb2);
        int ri = 0;
        for (const char *p=hb; *p; ) up_str[ri++]=*p++;
        up_str[ri++]=':';
        for (const char *p=mb2; *p; ) up_str[ri++]=*p++;
        up_str[ri++]=':';
        for (const char *p=sb2; *p; ) up_str[ri++]=*p++;
        up_str[ri]='\0';
    }

    __attribute__((weak)) const char *platform_kernel_str(void);
    const char *kern_str = (platform_kernel_str && platform_kernel_str())
                         ? platform_kernel_str() : "freestanding";
    struct { const char *key; const char *val; } rows[] = {
        { "OS",       "FiFi OS Beta 1.0"   },
        { "Arch",     "x86_64"             },
        { "Kernel",   kern_str             },
        { "Memory",   mem_str              },
        { "Network",  ip_str               },
        { "Uptime",   up_str               },
        { NULL, NULL }
    };

    /* Measure widths — key col fixed, val col from actual content */
    int nrows = 0; while (rows[nrows].key) nrows++;
    uint64_t key_w = 9u * fw;   /* longest key "Network " = 8 + colon */
    uint64_t max_val_chars = 20u;
    for (int _ri = 0; _ri < nrows; _ri++) {
        uint64_t vl = (uint64_t)gui_strlen(rows[_ri].val);
        if (vl > max_val_chars) max_val_chars = vl;
    }
    uint64_t val_w = max_val_chars * fw;
    uint64_t panel_w = key_w + val_w + 20u;
    uint64_t row_h2  = fh + 4u;
    uint64_t panel_h = (uint64_t)nrows * row_h2 + fh + 16u; /* title + rows + padding */

    /* Place panel: bottom-left, 24px from edges — clear of a left-edge dock. */
    uint64_t px = desk_left() + 24u;
    uint64_t py = dbot > panel_h + 24u ? dbot - panel_h - 24u : dt + 4u;
    if (py + panel_h > dbot) return;

    /* Panel background */
    uint32_t panel_bg = 0x00060c18u;
    console_fill_rect(px, py, panel_w, panel_h, panel_bg);
    console_fill_rect(px, py, panel_w, 1u, g_theme.accent);
    console_fill_rect(px, py, 2u, panel_h, g_theme.accent);
    console_fill_rect(px, py + panel_h - 1u, panel_w, 1u, 0x00202838u);
    console_fill_rect(px + panel_w - 1u, py, 1u, panel_h, 0x00202838u);

    /* "FiFi OS" title line */
    uint64_t tx = px + 8u;
    uint64_t ty2 = py + 6u;
    gui_draw_str(tx, ty2, "FiFi OS", g_theme.accent, panel_bg);
    /* version dim text */
    uint64_t ver_x = tx + 8u * fw;
    gui_draw_str(ver_x, ty2, "Beta 1.0", 0x00384858u, panel_bg);

    /* Separator */
    console_fill_rect(px + 4u, ty2 + fh + 2u, panel_w - 8u, 1u, 0x00182838u);

    /* Info rows */
    uint64_t ry2 = ty2 + fh + 6u;
    for (int i = 0; rows[i].key; i++) {
        gui_draw_str(tx, ry2, rows[i].key, 0x00506878u, panel_bg);
        uint64_t vx = tx + key_w;
        gui_draw_str(vx, ry2, rows[i].val, 0x0090a8bcu, panel_bg);
        ry2 += row_h2;
    }
}

/* ── Desktop background — wallpaper presets ──────────────────────────── */

/* Gradient wallpaper endpoints (top → bottom). A richer, deeper field than the
 * old flat blue: a touch brighter at the top and noticeably deeper at the
 * bottom gives the desktop real depth. Shared so corner-rounding can sample the
 * exact backdrop colour instead of guessing. */
#define WALL_GRAD_TOP  0x00294a8fu
#define WALL_GRAD_BOT  0x00070a14u

/* Cache for smooth "field" wallpapers (Aurora + variants): rendered once and
 * blitted thereafter (see draw_desktop_bg). Declared here so desktop_bg_at can
 * sample it. Keyed on resolution + accent + which field. */
static uint32_t *g_aurora_cache = 0;
static uint64_t  g_aurora_cw = 0, g_aurora_ch = 0;
static uint32_t  g_aurora_accent = 0;
static int       g_aurora_wall = -1;

/* Wallpaper colour at absolute row y — used by window corner rounding so the
 * punched-out corners reveal the true backdrop for any wallpaper/palette. For
 * non-gradient wallpapers, return a representative deep tone. */
uint32_t desktop_bg_at(uint64_t y) {
    uint64_t dt = desk_top(), dav = desk_avail();
    if (g_theme.wallpaper == WALLPAPER_GRADIENT) {
        if (dav == 0) return WALL_GRAD_BOT;
        int64_t f = (int64_t)(y >= dt ? y - dt : 0);
        if (f > (int64_t)dav) f = (int64_t)dav;
        uint32_t r0=(WALL_GRAD_TOP>>16)&0xff, g0=(WALL_GRAD_TOP>>8)&0xff, b0=WALL_GRAD_TOP&0xff;
        uint32_t r1=(WALL_GRAD_BOT>>16)&0xff, g1=(WALL_GRAD_BOT>>8)&0xff, b1=WALL_GRAD_BOT&0xff;
        uint32_t r=(uint32_t)(r0 + (int64_t)(r1-(int64_t)r0)*f/(int64_t)dav);
        uint32_t g=(uint32_t)(g0 + (int64_t)(g1-(int64_t)g0)*f/(int64_t)dav);
        uint32_t b=(uint32_t)(b0 + (int64_t)(b1-(int64_t)b0)*f/(int64_t)dav);
        return (r<<16)|(g<<8)|b;
    }
    if (g_theme.wallpaper == WALLPAPER_SOLID) return 0x00111118u;
    /* Field wallpapers (Aurora + variants, ids >= WALLPAPER_AURORA): sample the
     * rendered field cache at its horizontal centre for this row so corner-rounding
     * + the status-bar strip repaint match the real pixels. Deep tone until built. */
    if (g_theme.wallpaper >= WALLPAPER_AURORA) {
        if (g_aurora_cache && g_aurora_cw > 0 && y < g_aurora_ch)
            return g_aurora_cache[y * g_aurora_cw + g_aurora_cw / 2u];
        return 0x00090e1au;
    }
    return 0x000a0c14u;
}

/* Cheap parabolic sine approximation (no libm): input radians, output ~[-1,1].
 * Uses only add/sub/mul/div, so it needs no math library (works in static build). */
static float aurora_fsin(float x) {
    const float PI = 3.14159265f, TAU = 6.28318531f;
    x -= TAU * (float)((int)(x / TAU + (x < 0.0f ? -0.5f : 0.5f)));  /* wrap ~[-PI,PI] */
    if (x < -PI) x += TAU; else if (x > PI) x -= TAU;
    float ax = x < 0.0f ? -x : x;
    float y = 1.27323954f * x - 0.405284735f * x * ax;              /* 4/PI, 4/PI^2 */
    float ay = y < 0.0f ? -y : y;
    return 0.225f * (y * ay - y) + y;                               /* precision refine */
}

/* ── Smooth per-pixel "field" wallpapers ───────────────────────────────────
 * A dark vertical base + a few soft, gently FLOWING colour ribbons lit
 * additively, with 4x4 Bayer dither to defeat 8-bit banding. Every field
 * wallpaper (Aurora, Northern Lights, Nebula, Dusk, ...) is just a different
 * field_cfg_t through this SAME renderer, so they are ALL seamless (no banded
 * rects, no per-segment seams). Rendered once and cached (see draw_desktop_bg). */
#define FIELD_MAX_RIB 6
typedef struct {
    uint32_t base_top, base_bot;   /* dark vertical base gradient */
    int      nrib;                 /* number of ribbons (<= FIELD_MAX_RIB) */
    int      accent_rib;           /* ribbon index using g_theme.accent, or -1 */
    struct {
        float    cy, hh, peak;     /* centre frac, half-height frac, additive peak */
        uint32_t col;              /* ribbon colour (ignored if this is accent_rib) */
        float    amp, fq, ph;      /* vertical undulation: amplitude frac, freq, phase */
        float    im, ifq, iph;     /* horizontal intensity: depth, freq (hi=rays), phase */
    } rib[FIELD_MAX_RIB];
} field_cfg_t;

static void smoothfield_render(uint32_t *buf, uint64_t fb_w, uint64_t fb_h,
                               const field_cfg_t *cfg, uint32_t accent) {
    float W = (float)fb_w, Hf = (float)fb_h;
    int nr = cfg->nrib; if (nr > FIELD_MAX_RIB) nr = FIELD_MAX_RIB; if (nr < 1) nr = 1;
    /* Resolve ribbon colours (accent substitution) to float r/g/b. */
    float cr[FIELD_MAX_RIB], cg[FIELD_MAX_RIB], cb[FIELD_MAX_RIB];
    for (int i = 0; i < nr; i++) {
        uint32_t c = (i == cfg->accent_rib) ? accent : cfg->rib[i].col;
        cr[i] = (float)((c >> 16) & 0xffu);
        cg[i] = (float)((c >> 8)  & 0xffu);
        cb[i] = (float)( c        & 0xffu);
    }
    float btR=(float)((cfg->base_top>>16)&0xffu), btG=(float)((cfg->base_top>>8)&0xffu), btB=(float)(cfg->base_top&0xffu);
    float bbR=(float)((cfg->base_bot>>16)&0xffu), bbG=(float)((cfg->base_bot>>8)&0xffu), bbB=(float)(cfg->base_bot&0xffu);
    const float TAU = 6.28318531f;
    /* Hoist per-column sine work out of the pixel loop. */
    float *cy = (float *)kmalloc(fb_w * (uint64_t)nr * sizeof(float));
    float *im = (float *)kmalloc(fb_w * (uint64_t)nr * sizeof(float));
    if (!cy || !im) { if (cy) kfree(cy); if (im) kfree(im); return; }
    for (uint64_t x = 0; x < fb_w; x++) {
        float u = (float)x / W;
        for (int i = 0; i < nr; i++) {
            cy[x*nr+i] = (cfg->rib[i].cy + cfg->rib[i].amp * aurora_fsin(u*TAU*cfg->rib[i].fq + cfg->rib[i].ph)) * Hf;
            float m = 0.5f * (1.0f + aurora_fsin(u*TAU*cfg->rib[i].ifq + cfg->rib[i].iph));
            im[x*nr+i] = (1.0f - cfg->rib[i].im) + cfg->rib[i].im * m;
        }
    }
    static const int bay[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    for (uint64_t y = 0; y < fb_h; y++) {
        float fy = (float)y, v = fy / Hf;
        float baseR = btR + (bbR - btR) * v;
        float baseG = btG + (bbG - btG) * v;
        float baseB = btB + (bbB - btB) * v;
        uint32_t *rowp = buf + y * fb_w;
        const int *brow = bay[y & 3];
        for (uint64_t x = 0; x < fb_w; x++) {
            float R = baseR, G = baseG, B = baseB;
            for (int i = 0; i < nr; i++) {
                float d = (fy - cy[x*nr+i]) / (cfg->rib[i].hh * Hf);
                float ad = d < 0.0f ? -d : d;
                if (ad >= 1.0f) continue;
                float t = 1.0f - ad*ad; t *= t;               /* smooth quartic bump */
                float inten = cfg->rib[i].peak * t * im[x*nr+i];
                R += cr[i] * inten; G += cg[i] * inten; B += cb[i] * inten;
            }
            int dth = (brow[x & 3] - 7) >> 1;                 /* ~-4..+4 ordered dither */
            int Ri = (int)(R + 0.5f) + dth;
            int Gi = (int)(G + 0.5f) + dth;
            int Bi = (int)(B + 0.5f) + dth;
            if (Ri < 0) Ri = 0; else if (Ri > 255) Ri = 255;
            if (Gi < 0) Gi = 0; else if (Gi > 255) Gi = 255;
            if (Bi < 0) Bi = 0; else if (Bi > 255) Bi = 255;
            rowp[x] = ((uint32_t)Ri << 16) | ((uint32_t)Gi << 8) | (uint32_t)Bi;
        }
    }
    kfree(cy); kfree(im);
}

/* ── Field wallpaper presets ───────────────────────────────────────────────*/
/* "Aurora" — the accent-adaptive flagship: teal-green top, warm accent hero
 * curtain, violet floor. (accent_rib=1 → the hero uses g_theme.accent.) */
static const field_cfg_t FIELD_AURORA = {
    0x00080c17u, 0x0003050au, 3, 1,
    {{ 0.24f,0.28f,0.58f, 0x002cd6bau, 0.055f,1.4f,0.0f, 0.42f,2.3f,0.7f },
     { 0.46f,0.26f,0.80f, 0x00000000u, 0.060f,1.1f,2.1f, 0.34f,1.7f,2.0f },
     { 0.74f,0.32f,0.60f, 0x007c5af0u, 0.050f,1.3f,4.2f, 0.42f,2.0f,3.4f }},
};

/* "Northern Lights" — true-to-life aurora: emerald hero curtain with vertical
 * ray striations (high ifq), a magenta upper fringe, a cyan lower glow, over a
 * deep night-blue base. Curated palette (not accent-tinted). */
static const field_cfg_t FIELD_NORTHERN = {
    0x00060c1cu, 0x00020408u, 3, -1,
    {{ 0.24f,0.20f,0.46f, 0x00c24a96u, 0.05f,1.5f,0.5f, 0.44f,6.5f,0.9f },
     { 0.46f,0.25f,0.83f, 0x0033f593u, 0.065f,1.2f,2.2f, 0.48f,6.5f,0.7f },
     { 0.74f,0.31f,0.50f, 0x0010b0e0u, 0.045f,0.95f,4.5f, 0.36f,1.8f,3.2f }},
};

/* "Nebula" — deep-space blooms: indigo body, violet, magenta hero, cyan wisp,
 * deep-blue floor. Diffuse (low ifq), Hubble-field depth. */
static const field_cfg_t FIELD_NEBULA = {
    0x00080614u, 0x00020108u, 5, -1,
    {{ 0.52f,0.34f,0.47f, 0x004a3ad0u, 0.05f,1.0f,0.0f, 0.35f,1.4f,0.5f },
     { 0.30f,0.26f,0.60f, 0x008a46f2u, 0.06f,1.3f,1.6f, 0.40f,1.8f,2.1f },
     { 0.68f,0.24f,0.72f, 0x00e0409au, 0.07f,1.15f,3.0f, 0.42f,1.6f,4.0f },
     { 0.15f,0.19f,0.50f, 0x0033d6e0u, 0.08f,1.6f,4.7f, 0.45f,2.2f,0.9f },
     { 0.86f,0.30f,0.42f, 0x005a30c8u, 0.04f,0.9f,5.5f, 0.38f,1.2f,3.3f }},
};

/* "Coral Dusk" — twilight: cool indigo night up top warming to a rose mid-band,
 * coral afterglow, and an amber sun-line at the horizon. */
static const field_cfg_t FIELD_DUSK = {
    0x00080616u, 0x00170608u, 4, -1,
    {{ 0.20f,0.30f,0.50f, 0x00705cf0u, 0.04f,1.1f,0.4f, 0.35f,1.6f,0.5f },
     { 0.46f,0.26f,0.56f, 0x00e84f9cu, 0.05f,1.4f,2.1f, 0.40f,2.2f,2.0f },
     { 0.66f,0.24f,0.62f, 0x00f06a55u, 0.06f,0.9f,3.6f, 0.42f,1.3f,3.5f },
     { 0.86f,0.32f,0.83f, 0x00ff9a3au, 0.045f,1.0f,5.0f, 0.38f,1.8f,4.8f }},
};

/* "Abyssal Ocean" — cool depths: aqua surface caustics (high ifq), cyan hero,
 * teal current, deep-blue abyss floor. */
static const field_cfg_t FIELD_OCEAN = {
    0x00061420u, 0x00010306u, 4, -1,
    {{ 0.15f,0.24f,0.56f, 0x0038e6d0u, 0.05f,1.5f,0.4f, 0.44f,6.5f,1.1f },
     { 0.40f,0.29f,0.74f, 0x0015c4e8u, 0.06f,1.1f,2.2f, 0.36f,1.6f,3.3f },
     { 0.60f,0.30f,0.48f, 0x000f9c86u, 0.07f,0.9f,3.9f, 0.40f,2.3f,5.0f },
     { 0.84f,0.33f,0.64f, 0x001a54dau, 0.045f,1.3f,5.6f, 0.34f,1.4f,0.7f }},
};

/* "Spring Dawn" — airy but dark: pale-green sky, aqua band, mint hero, pale-gold
 * sunrise on the horizon. */
static const field_cfg_t FIELD_SPRING = {
    0x000a1713u, 0x00050c10u, 4, -1,
    {{ 0.16f,0.22f,0.44f, 0x0074d69cu, 0.05f,1.0f,5.2f, 0.34f,1.4f,1.2f },
     { 0.36f,0.24f,0.55f, 0x002fc8d6u, 0.07f,1.5f,3.6f, 0.40f,4.6f,4.0f },
     { 0.56f,0.28f,0.66f, 0x003fe08cu, 0.06f,1.2f,1.8f, 0.44f,2.2f,0.9f },
     { 0.84f,0.26f,0.50f, 0x00e6c06au, 0.04f,0.9f,0.4f, 0.36f,1.3f,2.1f }},
};

/* "Molten Ember" — volcanic glow rising from the floor: smoky ember, crimson
 * body, molten-orange hero, gold hottest core (flame rays via high ifq). */
static const field_cfg_t FIELD_EMBER = {
    0x00080506u, 0x00160805u, 4, -1,
    {{ 0.45f,0.30f,0.45f, 0x00b52812u, 0.07f,0.85f,5.0f, 0.38f,2.2f,1.0f },
     { 0.72f,0.34f,0.55f, 0x00e01f2bu, 0.05f,0.9f,0.0f, 0.40f,1.6f,0.5f },
     { 0.88f,0.28f,0.80f, 0x00ff5e14u, 0.06f,1.2f,1.6f, 0.42f,5.0f,2.1f },
     { 0.96f,0.20f,0.52f, 0x00ffc24au, 0.04f,1.5f,3.2f, 0.45f,6.0f,4.0f }},
};

/* Maps a wallpaper id to its field config, or NULL for non-field wallpapers. */
static const field_cfg_t *field_cfg_for(int wall) {
    switch (wall) {
        case WALLPAPER_AURORA:   return &FIELD_AURORA;
        case WALLPAPER_NORTHERN: return &FIELD_NORTHERN;
        case WALLPAPER_NEBULA:   return &FIELD_NEBULA;
        case WALLPAPER_DUSK:     return &FIELD_DUSK;
        case WALLPAPER_OCEAN:    return &FIELD_OCEAN;
        case WALLPAPER_SPRING:   return &FIELD_SPRING;
        case WALLPAPER_EMBER:    return &FIELD_EMBER;
        default:                 return 0;
    }
}

/* Cache for the procedural Aurora wallpaper: it depends only on resolution +
 * accent, so render it once and blit the cache thereafter (window drags trigger
 * full_redraw per frame — recomputing 8 soft bands each time would be wasteful). */

void draw_desktop_bg(void) {
    uint64_t fb_w  = console_fb_width();
    uint64_t dt    = desk_top();
    uint64_t dav   = desk_avail();
    uint64_t dbot  = desk_bot();

    /* Smooth per-pixel "field" wallpapers (Aurora + variants): render once into a
     * cache (keyed on resolution + accent + which field), blit thereafter. Full
     * screen (0..fb_h) so it's seamless behind the horizon bar and dock. */
    const field_cfg_t *fcfg = field_cfg_for(g_theme.wallpaper);
    if (fcfg) {
        uint64_t fb_h = console_fb_height();
        if (!g_aurora_cache || g_aurora_cw != fb_w || g_aurora_ch != fb_h ||
            g_aurora_accent != g_theme.accent || g_aurora_wall != g_theme.wallpaper) {
            if (g_aurora_cache && (g_aurora_cw != fb_w || g_aurora_ch != fb_h)) {
                kfree(g_aurora_cache); g_aurora_cache = 0;
            }
            if (!g_aurora_cache) g_aurora_cache = kmalloc(fb_w * fb_h * 4u);
            if (g_aurora_cache) {
                smoothfield_render(g_aurora_cache, fb_w, fb_h, fcfg, g_theme.accent);
                g_aurora_cw = fb_w; g_aurora_ch = fb_h;
                g_aurora_accent = g_theme.accent; g_aurora_wall = g_theme.wallpaper;
            }
        }
        if (g_aurora_cache) console_paste_rect(g_aurora_cache, 0, 0, fb_w, fb_h);
        else console_fill_vgrad(0, 0, fb_w, fb_h, fcfg->base_top, fcfg->base_bot);
        draw_desktop_icons();  /* same post-wallpaper overlays as the switch tail below */
        draw_desktop_info();
        return;
    }

    switch (g_theme.wallpaper) {

    case WALLPAPER_SOLID:
        console_fill_rect(0, dt, fb_w, dav, 0x00111118u);
        break;

    case WALLPAPER_STARS: {
        /* Fill base + sprinkle deterministic "stars" */
        console_fill_rect(0, dt, fb_w, dav, 0x00060610u);
        /* Use a simple LCG-like pseudo-random pattern */
        uint64_t s = 0x12345678u;
        for (int i = 0; i < 600; i++) {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            uint64_t sx = (s >> 33u) % (fb_w > 1u ? fb_w : 1u);
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            uint64_t sy = dt + (s >> 33u) % (dav > 1u ? dav : 1u);
            uint8_t  br = (uint8_t)(((s >> 16u) & 3u) * 30u + 80u);
            uint32_t col = ((uint32_t)br << 16) | ((uint32_t)br << 8) | br;
            console_fill_rect(sx, sy, 1u, 1u, col);
        }
        break;
    }

    case WALLPAPER_GRID: {
        console_fill_rect(0, dt, fb_w, dav, 0x000c0c14u);
        /* Horizontal lines every 32px */
        for (uint64_t y = dt; y < dbot; y += 32)
            console_fill_rect(0, y, fb_w, 1u, 0x00141422u);
        /* Vertical lines every 32px */
        for (uint64_t x = 0; x < fb_w; x += 32)
            console_fill_rect(x, dt, 1u, dav, 0x00141422u);
        /* Accent dots at intersections */
        for (uint64_t y = dt; y < dbot; y += 32)
            for (uint64_t x = 0; x < fb_w; x += 32)
                console_fill_rect(x, y, 2u, 2u, 0x001c2030u);
        break;
    }

    case WALLPAPER_WAVES: {
        /* Diagonal stripe pattern */
        for (uint64_t y = 0; y < dav; y++) {
            /* base gradient */
            uint64_t r2 = 0x10 + y * 8 / (dav > 1 ? dav : 1);
            uint64_t g2 = 0x10 + y * 6 / (dav > 1 ? dav : 1);
            uint64_t b2 = 0x18 + y * 12 / (dav > 1 ? dav : 1);
            if (r2 > 0x18) r2 = 0x18;
            if (g2 > 0x16) g2 = 0x16;
            if (b2 > 0x24) b2 = 0x24;
            uint32_t base = (uint32_t)((r2 << 16) | (g2 << 8) | b2);
            console_fill_rect(0, dt + y, fb_w, 1u, base);
        }
        /* Diagonal stripes overlay — coalesced into horizontal runs so each
         * lit band is a single fill_rect instead of per-pixel 1x1 fills (the
         * old form issued fb_w*dav fill calls every frame). The stripe test
         * ((x+y)/6)%4==0 is constant across runs of up to 6 pixels. */
        for (uint64_t y = 0; y < dav; y++) {
            uint64_t x = 0;
            while (x < fb_w) {
                bool on = (((x + y) / 6u) % 4u == 0u);
                uint64_t x2 = x;
                while (x2 < fb_w && ((((x2 + y) / 6u) % 4u == 0u) == on)) x2++;
                if (on) console_fill_rect(x, dt + y, x2 - x, 1u, 0x00181828u);
                x = x2;
            }
        }
        break;
    }

    case WALLPAPER_IMAGE:
        /* Place the loaded image into the desktop area per the chosen fit mode. */
        if (g_wall_img && g_wall_img_w > 0 && g_wall_img_h > 0) {
            uint64_t iw = g_wall_img_w, ih = g_wall_img_h;
            uint32_t *img = g_wall_img;
            const uint32_t LB = 0x00060810u;   /* letterbox / matte tone */
            switch (g_theme.wall_fit) {
            case WALLFIT_STRETCH:
                console_blit_scaled(img, iw, ih, 0, dt, fb_w, dav);
                break;
            case WALLFIT_FIT: {   /* contain: whole image visible, letterboxed */
                uint64_t sw2, sh2;
                if (fb_w * ih < dav * iw) { sw2 = fb_w;         sh2 = ih * fb_w / iw; }
                else                      { sh2 = dav;          sw2 = iw * dav / ih; }
                console_fill_rect(0, dt, fb_w, dav, LB);
                console_blit_scaled(img, iw, ih, (fb_w - sw2) / 2,
                                    dt + (dav - sh2) / 2, sw2, sh2);
                break;
            }
            case WALLFIT_CENTER: {   /* 1:1, centred (crop if larger, matte if smaller) */
                uint64_t dwn = iw < fb_w ? iw : fb_w;
                uint64_t dhn = ih < dav  ? ih : dav;
                int64_t  sx0 = iw > fb_w ? (int64_t)(iw - fb_w) / 2 : 0;
                int64_t  sy0 = ih > dav  ? (int64_t)(ih - dav)  / 2 : 0;
                console_fill_rect(0, dt, fb_w, dav, LB);
                console_blit_scaled_src(img, iw, ih, sx0, sy0, dwn, dhn,
                                        (fb_w - dwn) / 2, dt + (dav - dhn) / 2, dwn, dhn);
                break;
            }
            default: {   /* WALLFIT_FILL — cover: fill the area, crop overflow */
                uint64_t cw, ch; int64_t sx0, sy0;
                if (iw * dav > fb_w * ih) {         /* image wider than desk → crop sides */
                    cw = ih * fb_w / dav; ch = ih; sx0 = (int64_t)(iw - cw) / 2; sy0 = 0;
                } else {                            /* taller → crop top/bottom */
                    cw = iw; ch = iw * dav / fb_w; sx0 = 0; sy0 = (int64_t)(ih - ch) / 2;
                }
                console_blit_scaled_src(img, iw, ih, sx0, sy0, cw, ch, 0, dt, fb_w, dav);
                break;
            }
            }
        } else {
            /* Fallback gradient if no image loaded */
            console_fill_rect(0, dt, fb_w, dav, 0x00101018u);
        }
        break;

    default: {  /* WALLPAPER_GRADIENT — full-screen deep-blue desktop */
        /* Even top→bottom gradient reaching both edges. Endpoints kept in the
         * shared WALL_GRAD_* constants so win_round_corners() can sample the exact
         * backdrop colour at a window's corner (no hardcoded punch colour). */
        console_fill_vgrad(0, dt, fb_w, dav, WALL_GRAD_TOP, WALL_GRAD_BOT);
        /* Soft glow bloom, brightest upper-centre — a few wide blended bands
         * (cheap approximation of a radial light). */
        {
            uint64_t cy = dt + dav * 34u / 100u;
            uint32_t soft = 0x00437fd8u;
            for (int s = 0; s < 5; s++) {
                uint64_t bh = dav * (uint64_t)(9 - s) / 38u; if (bh < 2u) bh = 2u;
                uint64_t by = cy > bh / 2u ? cy - bh / 2u : dt;
                console_blend_rect(0, by, fb_w, bh, soft, (uint8_t)(5 + s * 3));
            }
        }
        break;
    }
    }
    draw_desktop_icons();  /* desktop icons above wallpaper, beneath windows */
    draw_desktop_info();   /* overlay on top of wallpaper, beneath windows */
}

/* ── Full compositing redraw ─────────────────────────────────────────── */

/* Partial repaint for the once-per-second clock tick.
 * Only repaints the two bars + settings window (uptime/memory).
 * Desktop and other windows are unchanged in the backbuffer. */
void tick_redraw(void) {
    draw_status_bar();  /* top strip (STATUS_H px) */
    taskbar_draw();     /* bottom strip (TASKBAR_H px) — clock, volume, FPS */
    if (g_tray_hover >= 0) tray_tip_draw();
    /* Redraw settings only if it is the topmost visible native window.
     * If another window is covering it, fall through to full_redraw so
     * the covering window isn't painted over. */
    bool settings_on_top = false;
    for (int _zi = MAX_WINS - 1; _zi >= 0; _zi--) {
        window_t *_tw = &g_wins[g_z[_zi]];
        if (!_tw->active || _tw->state == WIN_HIDDEN || _tw->anim_phase != ANIM_NONE) continue;
        settings_on_top = (_tw->type == WIN_SETTINGS);
        break;
    }
    __attribute__((weak)) int ipc_window_count(void);
    bool any_ipc = (ipc_window_count && ipc_window_count() > 0);
    if (settings_on_top && !any_ipc) {
        for (int _ti = 0; _ti < MAX_WINS; _ti++) {
            window_t *_tw = &g_wins[_ti];
            if (!_tw->active || _tw->state == WIN_HIDDEN ||
                _tw->type != WIN_SETTINGS || _tw->anim_phase != ANIM_NONE) continue;
            win_draw_chrome(_tw, false);
            settings_render(_tw);
            break;
        }
    } else {
        full_redraw();
    }
}

void full_redraw(void) {
#ifdef __linux__
    static int s_last_src = -1;
    if (g_redraw_src != s_last_src) {
        fprintf(stderr, "[redraw_src] %d\n", g_redraw_src);
        s_last_src = g_redraw_src;
    }
#endif
    draw_desktop_bg();
    draw_status_bar();
    bool suppress_term = false;
    /* Snap-to-half preview: draw before windows so the dragged window appears on top */
    if (g_snap_preview && g_dragging) {
        uint64_t dl = desk_left(), dw = desk_availw();
        uint64_t pw = dw / 2u;
        uint64_t px = (g_snap_preview == 2) ? dl + dw / 2u : dl;
        uint64_t py = desk_top();
        uint64_t ph = desk_avail();
        console_fill_rect(px, py, pw, ph, 0x00101c30u);
        console_fill_rect(px,      py,      pw, 1u,   0x003060c0u);
        console_fill_rect(px,      py+ph-1u, pw, 1u,   0x003060c0u);
        console_fill_rect(px,      py,       1u, ph,   0x003060c0u);
        console_fill_rect(px+pw-1u,py,       1u, ph,   0x003060c0u);
    }
    /* Window pass: render bottom-to-top using z-order */
    for (int zi = 0; zi < MAX_WINS; zi++) {
        int i = g_z[zi];
        window_t *w = &g_wins[i];
        if (!w->active || w->state == WIN_HIDDEN) continue;
        if (w->anim_phase == ANIM_CLOSE && w->anim_step > ANIM_TICKS) continue;
        /* Soft drop shadow: blended bands under/right of the window. Drawn over
         * freshly-composited content beneath, so alpha never accumulates. */
        if (g_theme.fx_shadows && w->anim_phase == ANIM_NONE && w->state == WIN_NORMAL) {
            static const uint8_t sh_a[4] = { 44, 30, 18, 8 };
            for (uint64_t s = 1; s <= 4; s++) {
                uint8_t a = sh_a[s - 1];
                console_blend_rect(w->x + s, w->y + w->h + (s - 1u), w->w, 1u, 0x00000000u, a);
                console_blend_rect(w->x + w->w + (s - 1u), w->y + s, 1u, w->h, 0x00000000u, a);
            }
        }
        if (w->anim_phase != ANIM_NONE) {
            /* Animated frame: draw a scaled placeholder rect */
            int _sidx2 = (w->anim_step >= 1 && w->anim_step <= ANIM_TICKS) ? w->anim_step - 1 : ANIM_TICKS - 1;
            int _sc2 = (w->anim_phase == ANIM_OPEN) ? g_anim_open_scale[_sidx2] : g_anim_close_scale[_sidx2];
            uint64_t aw = w->w * (uint64_t)_sc2 / 100u;
            uint64_t ah = w->h * (uint64_t)_sc2 / 100u;
            if (aw < 4) aw = 4;
            if (ah < 4) ah = 4;
            uint64_t ax = w->x + (w->w - aw) / 2u;
            uint64_t ay = w->y + (w->h - ah) / 2u;
            if (g_theme.fx_glass) {
                /* iOS-style liquid-glass materialize: a frosted translucent pane
                 * scaling up, blended over the composited desktop so it shows
                 * through like glass, with a glossy specular rim + top sheen and
                 * rounded corners. Settles into the solid window on the final
                 * frame. Redrawn over fresh content each tick, so no accumulation. */
                uint32_t a = 128u + (uint32_t)_sc2;          /* 164..228: firmer as it grows */
                if (a > 230u) a = 230u;
                console_blend_rect(ax, ay, aw, ah, 0x00243a5eu, (uint8_t)a);
                uint64_t sheen_h = ah / 5u; if (sheen_h < 1u) sheen_h = 1u;
                console_blend_rect(ax, ay, aw, sheen_h, 0x00ffffffu, 24u);   /* top sheen */
                /* glossy specular rim — brighter top/left */
                console_blend_rect(ax,             ay,            aw, 1u, 0x00d0e6ffu, 170u);
                console_blend_rect(ax,             ay,            1u, ah, 0x00bcd8ffu, 145u);
                console_blend_rect(ax,             ay + ah - 1u,  aw, 1u, 0x008fb4e0u, 95u);
                console_blend_rect(ax + aw - 1u,   ay,            1u, ah, 0x008fb4e0u, 95u);
                /* rounded corners: reveal the backdrop */
                int R = 6;
                for (int r = 0; r < R; r++) {
                    int dyc = R - r; int rr = R * R - dyc * dyc; if (rr < 0) rr = 0;
                    int sq = 0; while ((sq + 1) * (sq + 1) <= rr) sq++;
                    int nn = R - sq; if (nn <= 0) continue; if ((uint64_t)nn > aw) nn = (int)aw;
                    uint32_t ct = desktop_bg_at(ay + (uint64_t)r);
                    uint32_t cb = desktop_bg_at(ay + ah - 1u - (uint64_t)r);
                    console_fill_rect(ax,                     ay + (uint64_t)r,        (uint64_t)nn, 1u, ct);
                    console_fill_rect(ax + aw - (uint64_t)nn, ay + (uint64_t)r,        (uint64_t)nn, 1u, ct);
                    console_fill_rect(ax,                     ay + ah - 1u - (uint64_t)r, (uint64_t)nn, 1u, cb);
                    console_fill_rect(ax + aw - (uint64_t)nn, ay + ah - 1u - (uint64_t)r, (uint64_t)nn, 1u, cb);
                }
            } else {
                console_fill_rect(ax, ay, aw, ah, COL_WIN_BG);
                uint64_t tbh = ah < 24u ? ah : 24u;
                console_fill_rect(ax, ay, aw, tbh, 0x00141e2cu);
                console_fill_rect(ax, ay, aw, 1u, 0x00304060u);
                console_fill_rect(ax, ay + ah - 1u, aw, 1u, 0x00304060u);
                console_fill_rect(ax, ay, 1u, ah, 0x00304060u);
                console_fill_rect(ax + aw - 1u, ay, 1u, ah, 0x00304060u);
            }
            if (w->type != WIN_TERM) suppress_term = true;
            continue;
        }
        win_draw_chrome(w, true);
        win_render_content(w);
        if (w->type != WIN_TERM) suppress_term = true;
    }
    /* Corner-rounding pass: overwrite corner pixels with desktop background */
    for (int zi = 0; zi < MAX_WINS; zi++) {
        int i = g_z[zi];
        window_t *w = &g_wins[i];
        if (!w->active || w->state == WIN_HIDDEN) continue;
        if (w->anim_phase != ANIM_NONE) continue;
        if (w->w >= 8u && w->h >= 8u) win_round_corners(w);
    }
    /* Resize edge hint overlay */
    if (g_resize_hover_win >= 0 && g_resize_hover_dir != RES_NONE)
        draw_resize_hint(g_resize_hover_win, g_resize_hover_dir);
    /* Suppress console rendering when terminal is hidden (or another window is on top) */
    {
        window_t *term_w = &g_wins[0];
        bool term_hidden = !term_w->active
                        || term_w->state == WIN_HIDDEN
                        || term_w->anim_phase == ANIM_CLOSE;
        if (suppress_term || term_hidden)
            console_set_viewport(0, 0, 0, 0);
    }
    taskbar_draw();
    gui_draw_popups();   /* same overlay set main.c redraws above IPC windows */
}

/* ── Keyboard-shortcuts overlay (Super+/) ─────────────────────────────── */
void help_draw(void) {
    static const struct { const char *key, *desc; } rows[] = {
        { "Super",         "Open the app launcher" },
        { "Super + Left",  "Snap window left" },
        { "Super + Right", "Snap window right" },
        { "Super + Up",    "Maximize window" },
        { "Super + Down",  "Restore window" },
        { "Super + D",     "Show desktop" },
        { "Super + L",     "Lock screen" },
        { "Alt + Tab",     "Switch windows" },
        { "Alt + F4",      "Close window" },
        { "Print Screen",  "Save a screenshot" },
        { "F11 / F12",     "Volume down / up" },
        { "Super + /",     "Show this overlay" },
    };
    const int nrows = (int)(sizeof(rows) / sizeof(rows[0]));
    uint64_t fw = console_font_width(), fh = console_font_height();
    uint64_t fb_w = console_fb_width(), fb_h = console_fb_height();
    uint64_t line_h = fh + 8u;
    uint64_t key_w  = 14u * fw;                       /* key column */
    uint64_t w = key_w + 24u * fw + 64u;              /* + desc column + padding */
    uint64_t h = (uint64_t)nrows * line_h + fh + 46u; /* + title + padding */
    uint64_t x = (fb_w > w) ? (fb_w - w) / 2u : 0u;
    uint64_t y = (fb_h > h) ? (fb_h - h) / 2u : 0u;

    console_fill_vgrad(x, y, w, h, 0x00182238u, 0x000c111du);
    console_fill_rect(x, y, w, 1u, 0x003a5688u);
    console_fill_rect(x, y + h - 1u, w, 1u, 0x00223048u);
    console_fill_rect(x, y, 1u, h, 0x00223048u);
    console_fill_rect(x + w - 1u, y, 1u, h, 0x00223048u);

    const char *title = "Keyboard Shortcuts";
    uint64_t tlen = (uint64_t)gui_strlen(title) * fw;
    gui_draw_str_fg(x + (w - tlen) / 2u, y + 14u, title, 0x006aaddcu);
    console_fill_rect(x + 24u, y + fh + 24u, w - 48u, 1u, 0x00223048u);

    uint64_t ry = y + fh + 34u;
    for (int i = 0; i < nrows; i++, ry += line_h) {
        gui_draw_str_fg(x + 32u,          ry, rows[i].key,  0x0090c8f0u);
        gui_draw_str_fg(x + 32u + key_w,  ry, rows[i].desc, 0x00d0dcecu);
    }
}

/* Draw all popup overlays that must appear on top of IPC windows.
 * Called from main.c after ipc_blit_all / ipc_draw_overlays so they are
 * never covered by IPC app framebuffers. */
void gui_draw_popups(void) {
    /* Reveal the auto-hidden top bar as an overlay while the cursor is at the very
     * top edge and a window is maximized. Drawn here (after the IPC blit) so it sits
     * above IPC/Wayland app framebuffers, not behind them. */
    if (!statusbar_bottom() && g_theme.statusbar &&
        any_window_maximized() && g_statusbar_reveal) {
        g_statusbar_overlay = true;
        draw_status_bar();
        g_statusbar_overlay = false;
    }
    if (g_launcher_open)  launcher_draw();
    if (g_vol_popup_open) vol_popup_draw();
    if (g_cal_popup_open) cal_popup_draw();
    if (g_tray_hover >= 0) tray_tip_draw();
    if (g_ctx_open)       ctx_draw();
    if (g_fb_ctx_open)    fb_ctx_draw();
    if (g_txt_ctx_open)   txt_ctx_draw();
    if (g_icon_ctx_open)  icon_ctx_draw();
    if (g_icon_props_open) icon_props_draw();
    if (g_toast_ticks > 0 && g_toast_msg[0]) {
        uint64_t fb_w2 = console_fb_width();
        uint64_t fw    = console_font_width();
        uint64_t fh    = console_font_height();
        uint64_t th    = fh + 10u;
        uint64_t ty    = desk_bot() - th - 8u;
        uint64_t tlen  = (uint64_t)gui_strlen(g_toast_msg);
        uint64_t tw    = tlen * fw + 24u;
        uint64_t tx    = (fb_w2 > tw) ? (fb_w2 - tw) / 2u : 0u;
        uint32_t tbg   = 0x000e1622u;
        console_fill_rect(tx,     ty,     tw, th, tbg);
        console_fill_rect(tx,     ty,     tw, 1u, 0x00283448u);
        console_fill_rect(tx,     ty+th-1,tw, 1u, 0x00283448u);
        console_fill_rect(tx,     ty,     1u, th, 0x00283448u);
        console_fill_rect(tx+tw-1,ty,     1u, th, 0x00283448u);
        console_fill_rect(tx+1u, ty+1u, 3u, th-2u, g_toast_color);
        gui_draw_str(tx + 12u, ty + (th - fh) / 2u, g_toast_msg, g_toast_color, tbg);
    }

    if (g_help_open)
        help_draw();
}
