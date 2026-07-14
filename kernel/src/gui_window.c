#include "gui_internal.h"

void z_raise(int slot) {
    int pos = -1;
    for (int i = 0; i < MAX_WINS; i++) {
        if (g_z[i] == slot) { pos = i; break; }
    }
    if (pos < 0) return;
    for (int i = pos; i < MAX_WINS - 1; i++)
        g_z[i] = g_z[i + 1];
    g_z[MAX_WINS - 1] = slot;
}

/* True iff built-in window `slot` is the single GLOBALLY topmost window — its raise_z
 * beats every other built-in window AND every IPC window. Hover effects (highlight rows,
 * chrome button hover, resize-edge cursor) must only fire for the topmost window, so a
 * window behind another never repaints itself when the cursor passes over it. */
bool gui_is_topmost(int slot) {
    extern uint32_t ipc_topmost_z(void);
    if (slot < 0 || slot >= MAX_WINS) return false;
    window_t *w = &g_wins[slot];
    if (!w->active || w->state == WIN_HIDDEN || w->anim_phase == ANIM_CLOSE) return false;
    uint32_t my_z = w->raise_z;
    if (ipc_topmost_z() > my_z) return false;
    for (int i = 0; i < MAX_WINS; i++) {
        if (i == slot) continue;
        window_t *o = &g_wins[i];
        if (!o->active || o->state == WIN_HIDDEN || o->anim_phase == ANIM_CLOSE) continue;
        if (o->raise_z > my_z) return false;
    }
    return true;
}

/* Raise window to top of z-stack AND bump its raise_z from the shared counter.
 * Works uniformly for every window including the terminal (slot 0). */
void raise_win(int slot) {
    g_wins[slot].raise_z = g_gui_raise_z++;
    z_raise(slot);
}

/* Fill a horizontal span of the title bar. With glass on, a glossy upper band
 * over a matte lower band reads as frosted glass (opaque — no accumulation on
 * partial repaints); otherwise a plain steel gradient. */
static void titlebar_fill(window_t *w, uint64_t x, uint64_t span, bool active) {
    if (g_theme.fx_glass) {
        uint32_t hi  = active ? 0x003a577fu : 0x00263040u;
        uint32_t mid = active ? 0x00283a5eu : 0x001d2532u;
        uint32_t lo  = active ? 0x001b2942u : 0x00151b24u;
        uint64_t half = TITLE_H / 2u; if (half == 0u) half = 1u;
        console_fill_vgrad(x, w->y, span, half, hi, mid);
        console_fill_vgrad(x, w->y + half, span, TITLE_H - half, mid, lo);
    } else {
        uint32_t gt = active ? 0x00324a72u : 0x00202836u;
        uint32_t gb = active ? 0x001e2c48u : 0x00161c26u;
        console_fill_vgrad(x, w->y, span, TITLE_H, gt, gb);
    }
}

/* Draw the three titlebar window buttons in a conventional flat style:
 * minimize (bottom bar), maximize/restore (square outline), close (X). Each
 * highlights on hover (close highlights red). Deliberately NOT macOS traffic
 * lights — drawn from rectangles so no font glyph is needed. */
static void chrome_buttons(window_t *w, int slot, bool active) {
    uint64_t by  = w->y + 2u;
    uint64_t bh  = (TITLE_H > 4u) ? TITLE_H - 4u : TITLE_H;
    uint64_t cyc = w->y + TITLE_H / 2u;

    uint32_t hov_bg = col_mix(0x001c2636u, g_theme.accent, 120u);  /* accent-tinted hover */
    struct { uint64_t bx; int btn; uint32_t hbg; } btns[3] = {
        { w->btn_min_x, 3, hov_bg },
        { w->btn_max_x, 2, hov_bg },
        { w->btn_cls_x, 1, 0x00c0392bu },   /* close stays red (universal) */
    };
    for (int i = 0; i < 3; i++) {
        bool hov = (g_chrome_win == slot && g_chrome_btn == btns[i].btn);
        uint64_t cxc = btns[i].bx + BTN_W / 2u;
        if (hov) console_fill_rect(btns[i].bx, by, BTN_W, bh, btns[i].hbg);
        uint32_t gc = hov ? 0x00ffffffu : (active ? 0x00cbd6e6u : 0x00808c9cu);
        if (btns[i].btn == 3) {                        /* minimize: bottom bar */
            console_fill_rect(cxc - 5u, cyc + 4u, 10u, 2u, gc);
        } else if (btns[i].btn == 2) {                 /* maximize / restore */
            uint64_t sq = 9u, x0 = cxc - sq / 2u, y0 = cyc - sq / 2u;
            if (w->state == WIN_MAXIMIZED) {           /* restore: back-square hint */
                console_fill_rect(x0 + 3u, y0 - 3u, sq - 2u, 1u, gc);
                console_fill_rect(x0 + sq, y0 - 3u, 1u, sq - 2u, gc);
            }
            console_fill_rect(x0,           y0,           sq, 1u, gc);
            console_fill_rect(x0,           y0 + sq - 1u, sq, 1u, gc);
            console_fill_rect(x0,           y0,           1u, sq, gc);
            console_fill_rect(x0 + sq - 1u, y0,           1u, sq, gc);
        } else {                                       /* close: X */
            for (uint64_t k = 0; k < 9u; k++) {
                console_fill_rect(cxc - 4u + k, cyc - 4u + k, 2u, 2u, gc);
                console_fill_rect(cxc - 4u + k, cyc + 4u - k, 2u, 2u, gc);
            }
        }
    }
}

/* Per-window captured backdrop for the translucent title bar (see-through glass). */
static uint32_t *g_tb_bg[MAX_WINS];
static uint64_t  g_tb_bg_x[MAX_WINS], g_tb_bg_y[MAX_WINS], g_tb_bg_w[MAX_WINS];

void win_titlebar_bg_free(int slot) {
    if (slot < 0 || slot >= MAX_WINS) return;
    if (g_tb_bg[slot]) { kfree(g_tb_bg[slot]); g_tb_bg[slot] = 0; }
    g_tb_bg_w[slot] = 0;
}

/* Paint the entire title bar (background + specular + title text + buttons).
 * With Glass on it is TRANSLUCENT: do_capture=true (full redraw, run after the
 * desktop+lower windows are painted) grabs the fresh backdrop behind the bar and
 * blends a tint over it; do_capture=false (hover repaint) restores that stored
 * backdrop first, so re-blends never accumulate. Glass off = opaque glossy grad. */
static void titlebar_paint(window_t *w, int slot, bool active, bool do_capture) {
    uint64_t fw  = console_font_width();
    uint64_t fh  = console_font_height();
    uint64_t tpy = w->y + (TITLE_H > fh ? (TITLE_H - fh) / 2u : 0u);
    bool translucent = false;

    if (g_theme.fx_glass && slot >= 0 && slot < MAX_WINS) {
        if (do_capture) {
            if (!g_tb_bg[slot] || g_tb_bg_w[slot] != w->w) {
                if (g_tb_bg[slot]) kfree(g_tb_bg[slot]);
                g_tb_bg[slot] = (uint32_t *)kmalloc(w->w * TITLE_H * 4u);
            }
            if (g_tb_bg[slot]) {
                console_capture_rect(g_tb_bg[slot], w->x, w->y, w->w, TITLE_H);
                g_tb_bg_x[slot] = w->x; g_tb_bg_y[slot] = w->y; g_tb_bg_w[slot] = w->w;
                translucent = true;
            }
        } else if (g_tb_bg[slot] && g_tb_bg_w[slot] == w->w &&
                   g_tb_bg_x[slot] == w->x && g_tb_bg_y[slot] == w->y) {
            console_paste_rect(g_tb_bg[slot], w->x, w->y, w->w, TITLE_H);
            translucent = true;
        }
    }

    if (translucent) {
        /* frosted tint over the restored desktop backdrop — the see-through look.
         * The focused tint is derived from the accent so the active window's glass
         * subtly carries the user's palette; unfocused stays neutral+muted. */
        uint32_t tint = active ? col_mix(0x00182234u, g_theme.accent, 70u) : 0x00151c27u;
        uint8_t  a    = active ? 176u : 198u;
        console_blend_rect(w->x, w->y, w->w, TITLE_H, tint, a);
    } else {
        titlebar_fill(w, w->x, w->w, active);   /* opaque glossy fallback */
    }
    /* specular top edge + soft highlight (glass catching light) — a bright accent
     * tint on the focused bar reads as light glancing off the glass rim. */
    console_fill_rect(w->x, w->y, w->w, 1u,
                      active ? col_mix(g_theme.accent, 0x00ffffffu, 96u) : 0x00323d50u);
    if (g_theme.fx_glass)
        console_blend_rect(w->x, w->y + 1u, w->w, 1u, 0x00ffffffu, active ? 42u : 22u);
    console_fill_rect(w->x, w->y + TITLE_H - 1u, w->w, 1u, 0x0010192au);   /* bottom sep */

    /* Title text: prefix '*' for unsaved editor changes; centered or clipped. */
    const char *disp_title = w->title;
    char _mod_title[68];
    if (w->type == WIN_TEXT && w->text.edit_modified) {
        _mod_title[0] = '*'; _mod_title[1] = ' ';
        int _mt = 2;
        for (const char *_p = w->title; *_p && _mt < 67; _p++, _mt++) _mod_title[_mt] = *_p;
        _mod_title[_mt] = '\0';
        disp_title = _mod_title;
    }
    uint64_t tlen   = (uint64_t)gui_strlen(disp_title);
    uint64_t avail  = w->btn_min_x > w->x + 8u ? w->btn_min_x - w->x - 8u : 0u;
    uint64_t max_ch = fw > 0u ? avail / fw : 0u;
    uint64_t tpx;
    if (tlen <= max_ch) {
        tpx = w->x + (w->w - tlen * fw) / 2u;
        if (w->w < tlen * fw) tpx = w->x + 4u;
        gui_draw_str_clip_fg(tpx, tpy, disp_title, COL_TITLE_FG, max_ch);
    } else {
        tpx = w->x + 4u;
        if (max_ch > 3u) {
            gui_draw_str_clip_fg(tpx, tpy, disp_title, COL_TITLE_FG, max_ch - 3u);
            gui_draw_str_fg(tpx + (max_ch - 3u) * fw, tpy, "...", 0x00506878u);
        } else {
            gui_draw_str_clip_fg(tpx, tpy, disp_title, COL_TITLE_FG, max_ch);
        }
    }

    chrome_buttons(w, slot, active);
}

void win_draw_chrome(window_t *w, bool fill_content) {
    int slot = (int)(w - g_wins);
    /* Active (focused) = this window is the single GLOBALLY topmost window, comparing
     * raise_z across every built-in window AND every IPC window. Exactly one window is
     * active at a time; if an IPC window is globally on top, no built-in shows active. */
    extern uint32_t ipc_topmost_z(void);
    uint32_t my_z = g_wins[slot].raise_z;
    uint32_t global_top = ipc_topmost_z();
    for (int _j = 0; _j < MAX_WINS; _j++) {
        window_t *_ow = &g_wins[_j];
        if (!_ow->active || _ow->state == WIN_HIDDEN || _ow->anim_phase == ANIM_CLOSE) continue;
        if (_ow->raise_z > global_top) global_top = _ow->raise_z;
    }
    bool active = (my_z >= global_top);

    /* Compute button positions (needed for both full and partial paths) */
    w->btn_cls_x = w->x + w->w - BTN_W;
    w->btn_max_x = w->btn_cls_x - BTN_W;
    w->btn_min_x = w->btn_max_x - BTN_W;

    if (!fill_content) {
        /* Hover repaint: repaint the whole title bar (restores the captured
         * backdrop so the translucent glass doesn't accumulate) + buttons. */
        titlebar_paint(w, slot, active, false);
        return;
    }

    /* Full repaint path (fill_content=true) ─────────────────────────────── */

    /* Focus ring — a soft accent outline one pixel outside the window. Derived
     * from g_theme.accent (darkened) so it tracks the user's palette. */
    if (active) {
        uint64_t fb_w = console_fb_width();
        uint64_t dtop = desk_top();
        uint64_t dbot = desk_bot();
        uint32_t ring  = col_scale(g_theme.accent, 150u, 255u);
        uint64_t rx    = w->x > 0u        ? w->x - 1u        : 0u;
        uint64_t rw    = w->w + (w->x > 0u ? 2u : 1u);
        if (w->x + w->w >= fb_w) rw = fb_w - rx;   /* clamp rw to screen */
        if (w->y > dtop)
            console_fill_rect(rx, w->y - 1u, rw, 1u, ring);
        if (w->y + w->h < dbot)
            console_fill_rect(rx, w->y + w->h, rw, 1u, ring);
        if (w->x > 0u)
            console_fill_rect(w->x - 1u, w->y, 1u, w->h, ring);
        if (w->x + w->w < fb_w)
            console_fill_rect(w->x + w->w, w->y, 1u, w->h, ring);
    }

    /* Title bar — translucent frosted glass (see-through) or opaque glossy grad,
     * plus specular edge, title text, and traffic-light buttons. */
    titlebar_paint(w, slot, active, true);

    /* Neutral thin frame (accent lives in the focus ring, not the border) */
    uint32_t frame = active ? 0x00283a58u : 0x001d2634u;
    console_fill_rect(w->x, w->y + TITLE_H,
                      BORDER, w->h - TITLE_H, frame);
    console_fill_rect(w->x + w->w - BORDER, w->y + TITLE_H,
                      BORDER, w->h - TITLE_H, frame);
    console_fill_rect(w->x, w->y + w->h - BORDER,
                      w->w, BORDER, frame);

    /* Content background */
    uint64_t ix = w->x + BORDER;
    uint64_t iy = w->y + TITLE_H;
    uint64_t iw = w->w - 2u * BORDER;
    uint64_t ih = w->h - TITLE_H - BORDER;
    console_fill_rect(ix, iy, iw, ih, COL_WIN_BG);
}

/* ── Terminal viewport helpers ───────────────────────────────────────── */

void term_set_viewport(window_t *w) {
    uint64_t ix = w->x + BORDER;
    uint64_t iy = w->y + TITLE_H;
    uint64_t iw = w->w - 2u * BORDER;
    uint64_t ih = w->h - TITLE_H - BORDER;
    console_set_viewport(ix + PAD, iy + PAD, iw - 2u * PAD, ih - 2u * PAD);
    if (g_term_scroll > 0)
        console_set_suppress_draw(true);
    else
        console_set_suppress_draw(false);

    /* Keep the shell's PTY grid matched to the window so text wraps at the window
     * border and re-flows on resize. The console cols/rows now reflect this
     * window's viewport (set just above). Change-guarded so we only send a
     * SIGWINCH when the size actually changes. pty_set_winsize is Linux-only. */
    __attribute__((weak)) void pty_set_winsize(uint16_t cols, uint16_t rows);
    if (pty_set_winsize) {
        uint16_t cols = (uint16_t)console_cols();
        uint16_t rows = (uint16_t)console_rows();
        if (cols >= 20u && rows >= 4u) {
            static uint16_t last_cols = 0, last_rows = 0;
            if (cols != last_cols || rows != last_rows) {
                last_cols = cols;
                last_rows = rows;
                pty_set_winsize(cols, rows);
            }
        }
    }
}

/* Render scrollback content into the terminal window when g_term_scroll > 0. */
void term_render_scrollback(window_t *w) {
    uint64_t ix = w->x + BORDER;
    uint64_t iy = w->y + TITLE_H;
    uint64_t iw = w->w - 2u * BORDER;
    uint64_t ih = w->h - TITLE_H - BORDER;
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();
    if (fw == 0 || fh == 0) return;
    uint64_t cx = ix + PAD;
    uint64_t cy = iy + PAD;
    uint64_t cw = iw > 2u * PAD ? iw - 2u * PAD : 1u;
    uint64_t ch = ih > 2u * PAD ? ih - 2u * PAD : 1u;
    int max_rows = (int)(ch / fh);
    if (max_rows < 1) max_rows = 1;
    int max_cols = (int)(cw / fw);
    if (max_cols < 1) max_cols = 1;

    /* Fill background */
    console_fill_rect(cx, cy, cw, ch, 0x00010508u);

    /* Draw "SCROLLBACK" indicator in top-right */
    {
        static const char *ind = "-- SCROLLBACK --";
        uint64_t ilen = (uint64_t)gui_strlen(ind);
        uint64_t ind_x = cx + (cw > ilen * fw ? cw - ilen * fw - 4u : 0u);
        gui_draw_str(ind_x, cy, ind, 0x00406880u, 0x00010508u);
    }

    /* Render lines from scrollback ring buffer */
    int total_sb = console_tsb_count_lines();
    /* line 0 from end = newest line, line (total_sb-1) = oldest */
    /* We show max_rows lines; top of viewport = g_term_scroll lines from end */
    char lbuf[256];
    for (int row = 0; row < max_rows; row++) {
        int line_fe = g_term_scroll + (max_rows - 1 - row);
        if (line_fe >= total_sb) continue;
        /* Clamp the destination capacity to lbuf: on a wide/hi-DPI terminal
         * max_cols can exceed 255, and console_tsb_get_line treats the 3rd arg
         * as capacity, so an unclamped call smashes this 256-byte stack buffer. */
        int cap = max_cols + 1;
        if (cap > (int)sizeof(lbuf)) cap = (int)sizeof(lbuf);
        int len = console_tsb_get_line(line_fe, lbuf, cap);
        if (len <= 0) continue;
        uint64_t lx = cx;
        uint64_t ly = cy + (uint64_t)row * fh;
        for (int ci = 0; ci < len && ci < max_cols; ci++) {
            console_render_glyph(lx + (uint64_t)ci * fw, ly,
                                 (unsigned char)lbuf[ci], 0x00c8d8ecu, 0x00010508u);
        }
    }

    /* Scrollbar: thin strip on right */
    if (total_sb > max_rows) {
        uint64_t sb_x  = cx + cw - 6u;
        uint64_t sb_h  = ch;
        console_fill_rect(sb_x, cy, 4u, sb_h, 0x00101820u);
        uint64_t thumb_h = (uint64_t)max_rows * sb_h / (uint64_t)total_sb;
        if (thumb_h < 6u) thumb_h = 6u;
        if (thumb_h > sb_h) thumb_h = sb_h;
        int max_scroll = total_sb - max_rows;
        if (max_scroll < 1) max_scroll = 1;
        uint64_t thumb_y = cy + (uint64_t)(max_scroll - g_term_scroll) * (sb_h - thumb_h) / (uint64_t)max_scroll;
        if (thumb_y + thumb_h > cy + sb_h) thumb_y = cy + sb_h - thumb_h;
        console_fill_rect(sb_x + 1u, thumb_y, 2u, thumb_h, 0x00304860u);
    }
}

/* ── Window content render ───────────────────────────────────────────── */

void win_render_content(window_t *w) {
    if (w->type == WIN_TERM) {
        term_set_viewport(w);
        if (g_term_scroll > 0)
            term_render_scrollback(w);
    } else if (w->type == WIN_FILES)
        fb_render(w);
    else if (w->type == WIN_TEXT)
        text_render(w);
    else if (w->type == WIN_SETTINGS)
        settings_render(w);
}

/* Punch out rounded corners on a window by restoring the true desktop backdrop
 * behind the corner pixels. Samples desktop_bg_at() per row so it stays seamless
 * for any wallpaper/palette (the old code assumed one fixed blue tone). A 5px
 * quarter-circle reads as a modern KDE/macOS silhouette. */
void win_round_corners(const window_t *w) {
    uint64_t x = w->x, y = w->y, W = w->w, H = w->h;
    int R = (int)g_theme.corner_radius;
    if (R <= 0) return;
    if (R > 12) R = 12;
    /* Punch a quarter-circle of radius R at each corner, restoring the true
     * backdrop (sampled per row) so it stays seamless for any wallpaper. */
    for (int r = 0; r < R; r++) {
        int dy = R - r;                         /* rows from the circle centre */
        int rr = R * R - dy * dy; if (rr < 0) rr = 0;
        int s = 0; while ((s + 1) * (s + 1) <= rr) s++;  /* isqrt(rr) */
        int n = R - s;                          /* pixels to punch to backdrop */
        if (n <= 0) continue;
        if ((uint64_t)n > W) n = (int)W;
        uint32_t ct = desktop_bg_at(y + (uint64_t)r);
        uint32_t cb = desktop_bg_at(y + H - 1u - (uint64_t)r);
        console_fill_rect(x,                    y + (uint64_t)r,         (uint64_t)n, 1u, ct);
        console_fill_rect(x + W - (uint64_t)n,  y + (uint64_t)r,         (uint64_t)n, 1u, ct);
        console_fill_rect(x,                    y + H - 1u - (uint64_t)r,(uint64_t)n, 1u, cb);
        console_fill_rect(x + W - (uint64_t)n,  y + H - 1u - (uint64_t)r,(uint64_t)n, 1u, cb);
    }
}

/* ── Window open / hide / maximize ──────────────────────────────────── */

void win_show(window_t *w, int slot) {
#ifdef __linux__
    /* The legacy built-in Terminal (no tabs, raw boot shell) is fully replaced
     * by the standalone tabbed /bin/fifi-terminal. Whatever code path tries to
     * show the built-in window, launch the standalone app instead — this is the
     * single choke point, so the old terminal can never surface from ANY route
     * (launcher, taskbar, F-keys, context menus, file flows). */
    if (w->type == WIN_TERM) {
        __attribute__((weak)) void gui_spawn_app(const char *path);
        if (gui_spawn_app) gui_spawn_app("/bin/fifi-terminal");
        return;
    }
#endif
    uint64_t fb_w  = console_fb_width();
    uint64_t avail = desk_avail();

    if (w->w == 0 && w->h == 0) {
        if (w->type == WIN_TERM) {
            w->w = fb_w * 88u / 100u;
            w->h = avail * 90u / 100u;
        } else if (w->type == WIN_SETTINGS) {
            /* Scale width with font so content fits at any DPI (540px = 45*fw for fw=12) */
            uint64_t fw_now = console_font_width();
            w->w = fw_now > 0u ? fw_now * 45u : 540u;
            w->h = avail * 97u / 100u;
            if (w->w > fb_w) w->w = fb_w;
            if (w->h > avail) w->h = avail;
            g_settings_scroll = 0;
        } else {
            w->w = fb_w * 60u / 100u;
            w->h = avail * 85u / 100u;
        }
        /* Centered, with a small per-slot cascade so multiple windows don't pile up at
         * the exact same spot (each stays individually grabbable). */
        uint64_t ox = (uint64_t)slot * 28u;
        uint64_t oy = (uint64_t)slot * 28u;
        uint64_t availw = desk_availw();
        w->x = desk_left() + (availw > w->w ? (availw - w->w) / 2u : 0u) + ox;
        w->y = desk_top() + (avail - w->h) / 2u + oy;
        if (w->x + w->w > desk_right()) w->x = desk_right() > w->w ? desk_right() - w->w : desk_left();
        if (w->y + w->h > desk_bot()) w->y = desk_bot() > w->h ? desk_bot() - w->h : desk_top();
    }

    w->state      = WIN_NORMAL;
    w->raise_z    = g_gui_raise_z++;   /* gets fresh high z so it starts above all other windows */
    z_raise((int)(w - g_wins));
    if (g_theme.animations) {
        w->anim_phase = ANIM_OPEN;
        w->anim_step  = 1;
    } else {
        w->anim_phase = ANIM_NONE;
        w->anim_step  = 0;
    }
    full_redraw();
}

void win_hide(window_t *w, int slot) {
    (void)slot;
    if (w->type == WIN_TERM)
        console_set_viewport(0, 0, 0, 0);
    /* Auto-save text editor content on close if modified */
    if (w->type == WIN_TEXT && w->text.edit_mode && w->text.edit_modified)
        text_save(w);
    g_dragging    = false;
    g_resizing    = false;
    g_snap_preview = 0;
    /* Close any open menus for this window */
    if (g_txt_ctx_win == (int)(w - g_wins)) { g_txt_ctx_open = false; g_txt_ctx_win = -1; }
    if (w->type == WIN_SETTINGS) { g_font_dd_open = 0; g_font_dd_hover = -1; }
    win_titlebar_bg_free((int)(w - g_wins));   /* drop the captured glass backdrop */
    if (g_theme.animations) {
        w->anim_phase = ANIM_CLOSE;
        w->anim_step  = 1;
    } else {
        w->anim_phase = ANIM_NONE;
        w->state      = WIN_HIDDEN;
    }
    full_redraw();
}

void win_maximize_toggle(window_t *w) {
    if (w->state == WIN_MAXIMIZED) {
        w->x = w->saved_x; w->y = w->saved_y;
        w->w = w->saved_w; w->h = w->saved_h;
        w->state = WIN_NORMAL;
    } else {
        if (!w->half_snapped) {  /* don't overwrite snap-saved dims with half-snap dims */
            w->saved_x = w->x; w->saved_y = w->y;
            w->saved_w = w->w; w->saved_h = w->h;
        }
        w->x = desk_left(); w->y = desk_top();
        w->w = desk_availw(); w->h = desk_avail();
        w->state = WIN_MAXIMIZED;
    }
    w->half_snapped = false;
    full_redraw();
}

/* ── Resize ──────────────────────────────────────────────────────────── */

resize_dir_t hit_resize(window_t *w, int32_t mx, int32_t my) {
    if (w->state != WIN_NORMAL) return RES_NONE;

    int32_t wx = (int32_t)w->x;
    int32_t wy = (int32_t)w->y;
    int32_t we = wx + (int32_t)w->w;
    int32_t wb = wy + (int32_t)w->h;
    int32_t m  = (int32_t)RESIZE_MARGIN;
    int32_t cm = m * 2;

    if (mx < wx || mx > we || my < wy || my > wb)
        return RES_NONE;

    /* Corner regions are a cm×cm square at each corner and take priority — this makes
     * the TOP corners grabbable throughout the title-bar row, like a normal WM. */
    bool L = (mx <= wx + cm), R = (mx >= we - cm);
    bool T = (my <= wy + cm), B = (my >= wb - cm);
    if (T && L) return RES_NW;
    if (T && R) return RES_NE;
    if (B && L) return RES_SW;
    if (B && R) return RES_SE;

    /* Edges: a thin m-px strip on each side. The top edge strip sits just above the
     * title-bar drag area; left/right strips resize even within the title-bar row. */
    if (my <= wy + m)  return RES_N;
    if (my >= wb - m)  return RES_S;
    if (mx <= wx + m)  return RES_W;
    if (mx >= we - m)  return RES_E;

    /* Everything else (title-bar middle + content interior) is not a resize target. */
    return RES_NONE;
}

void win_do_resize(window_t *w, int32_t mx, int32_t my) {
    int32_t dx = mx - g_resize_ox;
    int32_t dy = my - g_resize_oy;

    int64_t nx = (int64_t)g_resize_wx0;
    int64_t ny = (int64_t)g_resize_wy0;
    int64_t nw = (int64_t)g_resize_ww0;
    int64_t nh = (int64_t)g_resize_wh0;

    switch (g_resize_dir) {
        case RES_E:  nw += dx; break;
        case RES_W:  nx += dx; nw -= dx; break;
        case RES_S:  nh += dy; break;
        case RES_N:  ny += dy; nh -= dy; break;
        case RES_SE: nw += dx; nh += dy; break;
        case RES_SW: nx += dx; nw -= dx; nh += dy; break;
        case RES_NE: nw += dx; ny += dy; nh -= dy; break;
        case RES_NW: nx += dx; nw -= dx; ny += dy; nh -= dy; break;
        default: break;
    }

    /* Per-window type minimum: Settings needs enough width for its content columns */
    uint64_t _fw = console_font_width(), _fh = console_font_height();
    /* Settings minimum: enough for 8 accent swatches + label column + padding */
    uint64_t _settings_min = 2u*(uint64_t)BORDER + 2u*12u + 18u*_fw + 8u*(_fh + 14u);
    if (_settings_min < (uint64_t)MIN_WIN_W) _settings_min = (uint64_t)MIN_WIN_W;
    int64_t  mw  = (w->type == WIN_SETTINGS)
                   ? (int64_t)_settings_min
                   : (int64_t)MIN_WIN_W;
    int64_t  mh   = (int64_t)MIN_WIN_H;
    int64_t  dtop  = (int64_t)desk_top();
    int64_t  dbot  = (int64_t)desk_bot();
    int64_t  dleft = (int64_t)desk_left();
    int64_t  dright= (int64_t)desk_right();

    if (nw < mw) {
        if (g_resize_dir == RES_W || g_resize_dir == RES_NW || g_resize_dir == RES_SW)
            nx = (int64_t)g_resize_wx0 + (int64_t)g_resize_ww0 - mw;
        nw = mw;
    }
    if (nh < mh) {
        if (g_resize_dir == RES_N || g_resize_dir == RES_NW || g_resize_dir == RES_NE)
            ny = (int64_t)g_resize_wy0 + (int64_t)g_resize_wh0 - mh;
        nh = mh;
    }
    if (nx < dleft) nx = dleft;
    if (ny < dtop) ny = dtop;
    if (nx + nw > dright) {
        if (g_resize_dir == RES_W || g_resize_dir == RES_NW || g_resize_dir == RES_SW)
            nx = dright - nw;
        else
            nw = dright - nx;
    }
    if (ny + nh > dbot) {
        if (g_resize_dir == RES_N || g_resize_dir == RES_NW || g_resize_dir == RES_NE)
            ny = dbot - nh;
        else
            nh = dbot - ny;
    }

    if (nx == (int64_t)w->x && ny == (int64_t)w->y &&
        nw == (int64_t)w->w && nh == (int64_t)w->h) return;

    /* Erase old window footprint with desktop bg so no ghost remains */
    console_fill_rect(w->x, w->y, w->w, w->h, COL_DESKTOP);
    w->x = (uint64_t)nx; w->y = (uint64_t)ny;
    w->w = (uint64_t)nw; w->h = (uint64_t)nh;
    win_draw_chrome(w, true);
    win_render_content(w);
}
