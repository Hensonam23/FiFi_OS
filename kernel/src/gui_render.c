#include "gui_internal.h"

/* ── Status bar (top strip, 0..STATUS_H-1) ───────────────────────────── */

void draw_status_bar(void) {
    uint64_t fb_w = console_fb_width();
    uint64_t fw   = console_font_width();
    uint64_t fh   = console_font_height();
    uint32_t bar_bg = 0x0008101cu;

    if (!g_theme.statusbar) {
        /* Clear the status bar strip so it blends into desktop */
        console_fill_rect(0, 0, fb_w, STATUS_H, 0x001a1a2eu);
        return;
    }

    uint64_t sy   = (STATUS_H > fh) ? (STATUS_H - fh) / 2u : 0u;

    console_fill_rect(0, 0, fb_w, STATUS_H, bar_bg);
    console_fill_rect(0, STATUS_H - 1, fb_w, 1, COL_TASKBAR_SEP);

    /* Left: branding */
    gui_draw_str(6u, sy, "FiFi OS", g_theme.accent, bar_bg);

    /* RTC wall clock: HH:MM (respects clock format setting) */
    uint8_t rh = 0, rm = 0, rs_unused = 0;
    rtc_get_time(&rh, &rm, &rs_unused);
    char clk[10];
    uint64_t clk_len;
    if (g_theme.clock_12h) {
        const char *ampm = (rh < 12u) ? "AM" : "PM";
        uint8_t h12 = rh % 12u; if (h12 == 0u) h12 = 12u;
        clk[0] = (char)('0' + h12 / 10u); clk[1] = (char)('0' + h12 % 10u); clk[2] = ':';
        gui_itoa_pad2(rm, clk + 3); clk[5] = ' '; clk[6] = ampm[0]; clk[7] = ampm[1]; clk[8] = '\0';
        clk_len = 8u;
    } else {
        gui_itoa_pad2(rh, clk + 0); clk[2] = ':';
        gui_itoa_pad2(rm, clk + 3); clk[5] = '\0';
        clk_len = 5u;
    }

    /* Uptime */
    uint64_t hz   = pit_get_hz();
    if (!hz) hz   = 100;
    uint64_t secs = pit_ticks() / hz;
    uint64_t mins = secs / 60u;  secs %= 60u;
    uint64_t hrs  = mins / 60u;  mins %= 60u;
    char up[14];
    {
        int i = 0;
        const char *p;
        for (p = "up "; *p; ) up[i++] = *p++;
        gui_itoa_pad2((int)hrs,  up + i); i += 2; up[i++] = ':';
        gui_itoa_pad2((int)mins, up + i); i += 2; up[i++] = ':';
        gui_itoa_pad2((int)secs, up + i); i += 2; up[i] = '\0';
    }

    /* Memory: used / total MB */
    uint64_t total_p = pmm_get_total_pages();
    uint64_t free_p  = pmm_get_free_pages();
    uint64_t used_mb  = ((total_p - free_p) * 4096u) >> 20u;
    uint64_t total_mb = (total_p * 4096u) >> 20u;

    char membuf[32];
    char ub[8], tb[8];
    gui_itoa((int)used_mb,  ub, 8);
    gui_itoa((int)total_mb, tb, 8);
    {
        int i = 0;
        const char *p;
        for (p = ub;    *p && i < 28; ) membuf[i++] = *p++;
        for (p = "/";   *p && i < 28; ) membuf[i++] = *p++;
        for (p = tb;    *p && i < 28; ) membuf[i++] = *p++;
        for (p = "MB";  *p && i < 28; ) membuf[i++] = *p++;
        membuf[i] = '\0';
    }

    /* Right side: clock | mem | uptime */
    uint32_t info_col = 0x00506878u;
    /* clk_len was set when building clk[] above */
    uint64_t mem_len  = (uint64_t)gui_strlen(membuf);
    uint64_t up_len   = (uint64_t)gui_strlen(up);
    uint64_t right_w  = (clk_len + 3u + mem_len + 3u + up_len) * fw + 12u;
    uint64_t rx = fb_w > right_w ? fb_w - right_w : 0u;

    gui_draw_str(rx,                                    sy, clk,    0x0090c8e8u, bar_bg);
    gui_draw_str(rx + (clk_len + 1u) * fw,             sy, "|",    0x00303848u, bar_bg);
    gui_draw_str(rx + (clk_len + 3u) * fw,             sy, membuf, info_col,    bar_bg);
    gui_draw_str(rx + (clk_len + 3u + mem_len + 1u) * fw, sy, "|", 0x00303848u, bar_bg);
    gui_draw_str(rx + (clk_len + 3u + mem_len + 3u) * fw, sy, up,  info_col,    bar_bg);

    /* Center: context path for Files / Text Viewer (topmost visible) */
    {
        const char *ctx = NULL;
        for (int zi = MAX_WINS - 1; zi >= 0; zi--) {
            int si = g_z[zi];
            window_t *cw = &g_wins[si];
            if (!cw->active || cw->state == WIN_HIDDEN) continue;
            if (cw->type == WIN_FILES  && cw->fb.path[0])    ctx = cw->fb.path;
            else if (cw->type == WIN_TEXT && cw->text.path[0]) ctx = cw->text.path;
            break;
        }
        if (ctx) {
            uint64_t brand_w = (7u + 3u) * fw;  /* "FiFi OS" + 3 gap chars */
            uint64_t avail   = rx > brand_w ? rx - brand_w : 0u;
            if (avail > 4u * fw) {
                uint64_t max_ch  = avail / fw;
                uint64_t ctx_len = (uint64_t)gui_strlen(ctx);
                const char *show = ctx;
                if (ctx_len > max_ch && max_ch > 1u) {
                    show = ctx + (ctx_len - max_ch + 1u);
                    while (show > ctx && *show != '/') show++;
                }
                uint64_t show_len = (uint64_t)gui_strlen(show);
                uint64_t ctx_x = brand_w + (avail > show_len * fw ? (avail - show_len * fw) / 2u : 0u);
                gui_draw_str_clip(ctx_x, sy, show, 0x00384e60u, bar_bg, max_ch);
            }
        }
    }
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

void draw_desktop_icons(void) {
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();
    uint64_t dt = desk_top();
    uint64_t db = desk_bot();
    /* Icons column along the right edge, top-down */
    uint64_t icon_x = console_fb_width() - DESK_ICON_W - DESK_ICON_PAD;
    uint64_t icon_y = dt + DESK_ICON_PAD;

    for (int i = 0; i < DESK_ICON_MAX; i++) {
        if (!g_desk_icons[i].active) continue;
        if (icon_y + DESK_ICON_H > db) break;

        bool hov = (g_desk_icon_hover == i);
        bool sel = (g_desk_icon_sel   == i);

        /* Icon background */
        uint32_t bg = sel ? (g_theme.accent & 0x00ffffffu) | 0x00182030u
                     : hov ? 0x001a2840u : 0x00000000u;
        if (bg) console_fill_rect(icon_x - 2u, icon_y - 2u,
                                  DESK_ICON_W + 4u, DESK_ICON_H + 4u, bg);

        /* Colored file-type square */
        const char *name = g_desk_icons[i].label[0]
                           ? g_desk_icons[i].label
                           : desk_icon_basename(g_desk_icons[i].path);
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
                ic = 0x00305030u; ic_txt = "SH";
            }
        }
        uint64_t isz  = 40u;
        uint64_t iix  = icon_x + (DESK_ICON_W - isz) / 2u;
        uint64_t iiy  = icon_y + 4u;
        console_fill_rect(iix, iiy, isz, isz, ic);
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
        gui_draw_str(ttx, tty, ic_txt, 0x00c0d8f0u, ic);

        /* Label below icon (truncate to fit) */
        char lbuf[20];
        strncpy(lbuf, name, sizeof(lbuf) - 1);
        lbuf[sizeof(lbuf) - 1] = '\0';
        uint64_t llen = (uint64_t)gui_strlen(lbuf);
        uint64_t llx  = icon_x + (llen * fw < DESK_ICON_W
                        ? (DESK_ICON_W - llen * fw) / 2u : 0u);
        uint64_t lly  = icon_y + isz + 8u;
        uint32_t lbg  = (bg && g_theme.wallpaper != WALLPAPER_IMAGE) ? bg : 0x00000000u;
        gui_draw_str(llx, lly, lbuf, hov ? 0x00e0efffU : 0x00c0d8f0u, lbg);

        g_desk_icons[i].active = g_desk_icons[i].active; /* touch to suppress warn */
        icon_y += DESK_ICON_H + DESK_ICON_PAD;
    }
}

/* ── Desktop icon position helper ────────────────────────────────────── */
/* Returns icon index at (mx,my), -1 if none */
int desk_icon_at(int mx, int my) {
    uint64_t dt = desk_top();
    uint64_t db = desk_bot();
    uint64_t icon_x = console_fb_width() - DESK_ICON_W - DESK_ICON_PAD;
    uint64_t icon_y = dt + DESK_ICON_PAD;
    for (int i = 0; i < DESK_ICON_MAX; i++) {
        if (!g_desk_icons[i].active) continue;
        if (icon_y + DESK_ICON_H > db) break;
        if ((uint64_t)mx >= icon_x - 2u && (uint64_t)mx < icon_x + DESK_ICON_W + 2u &&
            (uint64_t)my >= icon_y - 2u && (uint64_t)my < icon_y + DESK_ICON_H + 2u)
            return i;
        icon_y += DESK_ICON_H + DESK_ICON_PAD;
    }
    return -1;
}

/* ── Desktop info overlay (neofetch-style) ───────────────────────────── */

void draw_desktop_info(void) {
    if (!g_theme.desktop_info) return;
    uint64_t fbw  = console_fb_width();
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
        { "OS",       "FiFi OS Alpha v5.0" },
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

    /* Place panel: bottom-left, 24px from edges */
    uint64_t px = 24u;
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
    gui_draw_str(ver_x, ty2, "Alpha v5.0", 0x00384858u, panel_bg);

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
    (void)fbw;
}

/* ── Desktop background — wallpaper presets ──────────────────────────── */

void draw_desktop_bg(void) {
    uint64_t fb_w  = console_fb_width();
    uint64_t dt    = desk_top();
    uint64_t dav   = desk_avail();
    uint64_t dbot  = desk_bot();

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
        /* Diagonal stripes overlay */
        for (uint64_t y = 0; y < dav; y++) {
            for (uint64_t x = 0; x < fb_w; x++) {
                if (((x + y) / 6u) % 4u == 0u)
                    console_fill_rect(x, dt + y, 1u, 1u, 0x00181828u);
            }
        }
        break;
    }

    case WALLPAPER_IMAGE:
        /* Scale-blit the loaded wallpaper image to fill the desktop area */
        if (g_wall_img && g_wall_img_w > 0 && g_wall_img_h > 0) {
            console_blit_scaled(g_wall_img, g_wall_img_w, g_wall_img_h,
                                0, dt, fb_w, dav);
        } else {
            /* Fallback gradient if no image loaded */
            console_fill_rect(0, dt, fb_w, dav, 0x00101018u);
        }
        break;

    default:  /* WALLPAPER_GRADIENT */
        /* Subtle vertical gradient: dark navy top → slightly lighter bottom */
        for (uint64_t y = 0; y < dav; y++) {
            uint64_t r2 = 0x1a + y * 6 / (dav > 1 ? dav : 1);
            uint64_t g2 = 0x1a + y * 6 / (dav > 1 ? dav : 1);
            uint64_t b2 = 0x2e + y * 12 / (dav > 1 ? dav : 1);
            if (r2 > 0x20) r2 = 0x20;
            if (g2 > 0x20) g2 = 0x20;
            if (b2 > 0x3a) b2 = 0x3a;
            uint32_t row_col = (uint32_t)((r2 << 16) | (g2 << 8) | b2);
            console_fill_rect(0, dt + y, fb_w, 1u, row_col);
        }
        /* Dot grid overlay */
        for (uint64_t y = dt + 12; y + 1 < dbot; y += 20)
            for (uint64_t x = 12; x + 1 < fb_w; x += 20) {
                uint32_t dot = ((y / 20 + x / 20) & 1) ? 0x00222535u : 0x00232638u;
                console_fill_rect(x, y, 1, 1, dot);
            }
        break;
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
    uint64_t fb_w = console_fb_width();
    draw_desktop_bg();
    draw_status_bar();
    bool suppress_term = false;
    /* (Window drop shadows removed by design — flat windows, no shadow.) */
    /* Snap-to-half preview: draw before windows so the dragged window appears on top */
    if (g_snap_preview && g_dragging) {
        uint64_t px = (g_snap_preview == 2) ? fb_w / 2u : 0u;
        uint64_t pw = fb_w / 2u;
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
            console_fill_rect(ax, ay, aw, ah, COL_WIN_BG);
            /* Title bar strip */
            uint64_t tbh = ah < 24u ? ah : 24u;
            console_fill_rect(ax, ay, aw, tbh, 0x00141e2cu);
            /* Border */
            console_fill_rect(ax, ay, aw, 1u, 0x00304060u);
            console_fill_rect(ax, ay + ah - 1u, aw, 1u, 0x00304060u);
            console_fill_rect(ax, ay, 1u, ah, 0x00304060u);
            console_fill_rect(ax + aw - 1u, ay, 1u, ah, 0x00304060u);
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
    if (g_launcher_open)
        launcher_draw();
    if (g_vol_popup_open)
        vol_popup_draw();
    if (g_ctx_open)
        ctx_draw();
    if (g_fb_ctx_open)
        fb_ctx_draw();
    if (g_txt_ctx_open)
        txt_ctx_draw();

    /* Toast notification overlay */
    if (g_toast_ticks > 0 && g_toast_msg[0]) {
        uint64_t fw = console_font_width();
        uint64_t fh = console_font_height();
        uint64_t th = fh + 10u;
        uint64_t ty = desk_bot() - th - 8u;
        uint64_t tlen = (uint64_t)gui_strlen(g_toast_msg);
        uint64_t tw   = tlen * fw + 24u;
        uint64_t tx   = (fb_w > tw) ? (fb_w - tw) / 2u : 0u;
        uint32_t tbg  = 0x000e1622u;
        console_fill_rect(tx,     ty,     tw, th, tbg);
        console_fill_rect(tx,     ty,     tw, 1u, 0x00283448u);
        console_fill_rect(tx,     ty+th-1,tw, 1u, 0x00283448u);
        console_fill_rect(tx,     ty,     1u, th, 0x00283448u);
        console_fill_rect(tx+tw-1,ty,     1u, th, 0x00283448u);
        console_fill_rect(tx+1u, ty+1u, 3u, th-2u, g_toast_color);
        gui_draw_str(tx + 12u, ty + (th - fh) / 2u, g_toast_msg, g_toast_color, tbg);
    }
}

/* Draw all popup overlays that must appear on top of IPC windows.
 * Called from main.c after ipc_blit_all / ipc_draw_overlays so they are
 * never covered by IPC app framebuffers. */
void gui_draw_popups(void) {
    if (g_launcher_open)  launcher_draw();
    if (g_vol_popup_open) vol_popup_draw();
    if (g_ctx_open)       ctx_draw();
    if (g_fb_ctx_open)    fb_ctx_draw();
    if (g_txt_ctx_open)   txt_ctx_draw();
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
}
