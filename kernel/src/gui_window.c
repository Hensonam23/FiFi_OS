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

void win_draw_chrome(window_t *w, bool fill_content) {
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();

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
    /* Inactive title bar is a clearly-dimmed blue (not near-black) so a deselected
     * window still reads as a window. Active uses the bright accent border colour. */
    uint32_t title_bg = active ? COL_BORDER : 0x00243a5cu;

    /* Compute button positions (needed for both full and partial paths) */
    w->btn_cls_x = w->x + w->w - BTN_W;
    w->btn_max_x = w->btn_cls_x - BTN_W;
    w->btn_min_x = w->btn_max_x - BTN_W;

    uint64_t tpy = w->y + (TITLE_H > fh ? (TITLE_H - fh) / 2u : 0u);

    if (!fill_content) {
        /* Hover-only repaint: only redraw the 3 buttons (3×BTN_W×TITLE_H pixels).
         * Skipping the full-width title bar fill and title text saves ~200KB of
         * backbuffer writes and font rendering on every hover transition at 250fps. */
        uint32_t cls_bg = (g_chrome_win == slot && g_chrome_btn == 1)
                        ? 0x00cc3333u : COL_CLOSE;
        console_fill_rect(w->btn_cls_x, w->y, BTN_W, TITLE_H, cls_bg);
        console_render_glyph(w->btn_cls_x + (BTN_W - fw) / 2u, tpy,
                             'x', COL_TITLE_FG, cls_bg);

        uint32_t max_bg = (g_chrome_win == slot && g_chrome_btn == 2)
                        ? 0x004878a0u : COL_BTN_BG;
        console_fill_rect(w->btn_max_x, w->y, BTN_W, TITLE_H, max_bg);
        console_render_glyph(w->btn_max_x + (BTN_W - fw) / 2u, tpy,
                             w->state == WIN_MAXIMIZED ? '-' : '+',
                             COL_BTN_FG, max_bg);

        uint32_t min_bg = (g_chrome_win == slot && g_chrome_btn == 3)
                        ? 0x004878a0u : COL_BTN_BG;
        console_fill_rect(w->btn_min_x, w->y, BTN_W, TITLE_H, min_bg);
        console_render_glyph(w->btn_min_x + (BTN_W - fw) / 2u, tpy,
                             '_', COL_BTN_FG, min_bg);
        return;
    }

    /* Full repaint path (fill_content=true) ─────────────────────────────── */

    /* Focus ring and full-height border strips only on full redraws. */
    if (active) {
        uint64_t fb_w = console_fb_width();
        uint64_t dtop = desk_top();
        uint64_t dbot = desk_bot();
        uint32_t ring  = 0x00183a6au;
        uint64_t rx    = w->x > 0u        ? w->x - 1u        : 0u;
        uint64_t rw    = w->w + (w->x > 0u ? 2u : 1u);
        if (w->x + w->w < fb_w) { /* clamp rw */ } else { rw = fb_w - rx; }
        if (w->y > dtop)
            console_fill_rect(rx, w->y - 1u, rw, 1u, ring);
        if (w->y + w->h < dbot)
            console_fill_rect(rx, w->y + w->h, rw, 1u, ring);
        if (w->x > 0u)
            console_fill_rect(w->x - 1u, w->y, 1u, w->h, ring);
        if (w->x + w->w < fb_w)
            console_fill_rect(w->x + w->w, w->y, 1u, w->h, ring);
    }

    /* Title bar with subtle gradient: lighter top strip → base color */
    console_fill_rect(w->x, w->y, w->w, TITLE_H, title_bg);
    /* Top highlight line */
    uint32_t title_top = active ? 0x00223a62u : 0x001e2e44u;
    console_fill_rect(w->x, w->y, w->w, 1u, title_top);
    /* Bottom separator line */
    console_fill_rect(w->x, w->y + TITLE_H - 1u, w->w, 1u, 0x0010192au);

    /* Build display title: prefix '*' when text editor has unsaved changes */
    const char *disp_title = w->title;
    char _mod_title[68];
    if (w->type == WIN_TEXT && w->text.edit_modified) {
        _mod_title[0] = '*'; _mod_title[1] = ' ';
        int _mt = 2;
        for (const char *_p = w->title; *_p && _mt < 67; _p++, _mt++) _mod_title[_mt] = *_p;
        _mod_title[_mt] = '\0';
        disp_title = _mod_title;
    }

    /* Title: centered if it fits, left-aligned+clipped if too wide */
    uint64_t tlen     = (uint64_t)gui_strlen(disp_title);
    uint64_t avail    = w->btn_min_x > w->x + 8u ? w->btn_min_x - w->x - 8u : 0u;
    uint64_t max_ch   = fw > 0u ? avail / fw : 0u;
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

    /* Close button — with hover */
    uint32_t cls_bg = (g_chrome_win == slot && g_chrome_btn == 1)
                    ? 0x00cc3333u : COL_CLOSE;
    console_fill_rect(w->btn_cls_x, w->y, BTN_W, TITLE_H, cls_bg);
    console_render_glyph(w->btn_cls_x + (BTN_W - fw) / 2u, tpy,
                         'x', COL_TITLE_FG, cls_bg);

    /* Max button — with hover */
    uint32_t max_bg = (g_chrome_win == slot && g_chrome_btn == 2)
                    ? 0x004878a0u : COL_BTN_BG;
    console_fill_rect(w->btn_max_x, w->y, BTN_W, TITLE_H, max_bg);
    console_render_glyph(w->btn_max_x + (BTN_W - fw) / 2u, tpy,
                         w->state == WIN_MAXIMIZED ? '-' : '+',
                         COL_BTN_FG, max_bg);

    /* Min button — with hover */
    uint32_t min_bg = (g_chrome_win == slot && g_chrome_btn == 3)
                    ? 0x004878a0u : COL_BTN_BG;
    console_fill_rect(w->btn_min_x, w->y, BTN_W, TITLE_H, min_bg);
    console_render_glyph(w->btn_min_x + (BTN_W - fw) / 2u, tpy,
                         '_', COL_BTN_FG, min_bg);

    if (fill_content) {
        console_fill_rect(w->x, w->y + TITLE_H,
                          BORDER, w->h - TITLE_H, COL_BORDER);
        console_fill_rect(w->x + w->w - BORDER, w->y + TITLE_H,
                          BORDER, w->h - TITLE_H, COL_BORDER);
        console_fill_rect(w->x, w->y + w->h - BORDER,
                          w->w, BORDER, COL_BORDER);
    }

    if (fill_content) {
        uint64_t ix = w->x + BORDER;
        uint64_t iy = w->y + TITLE_H;
        uint64_t iw = w->w - 2u * BORDER;
        uint64_t ih = w->h - TITLE_H - BORDER;
        console_fill_rect(ix, iy, iw, ih, COL_WIN_BG);
    }
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
        int len = console_tsb_get_line(line_fe, lbuf, max_cols + 1);
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

/* Punch out 4px rounded corners on a window by overwriting with desktop bg. */
void win_round_corners(const window_t *w) {
    uint64_t x = w->x, y = w->y, W = w->w, H = w->h;
    uint32_t bg = COL_DESKTOP;
    /* Top-left */
    console_fill_rect(x,   y,   3u, 1u, bg);
    console_fill_rect(x,   y+1, 2u, 1u, bg);
    console_fill_rect(x,   y+2, 1u, 1u, bg);
    /* Top-right */
    console_fill_rect(x+W-3u, y,   3u, 1u, bg);
    console_fill_rect(x+W-2u, y+1, 2u, 1u, bg);
    console_fill_rect(x+W-1u, y+2, 1u, 1u, bg);
    /* Bottom-left */
    console_fill_rect(x,   y+H-1u, 3u, 1u, bg);
    console_fill_rect(x,   y+H-2u, 2u, 1u, bg);
    console_fill_rect(x,   y+H-3u, 1u, 1u, bg);
    /* Bottom-right */
    console_fill_rect(x+W-3u, y+H-1u, 3u, 1u, bg);
    console_fill_rect(x+W-2u, y+H-2u, 2u, 1u, bg);
    console_fill_rect(x+W-1u, y+H-3u, 1u, 1u, bg);
}

/* ── Window open / hide / maximize ──────────────────────────────────── */

void win_show(window_t *w, int slot) {
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
        w->x = (fb_w - w->w) / 2u + ox;
        w->y = desk_top() + (avail - w->h) / 2u + oy;
        if (w->x + w->w > fb_w) w->x = fb_w > w->w ? fb_w - w->w : 0;
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
    uint64_t fb_w = console_fb_width();

    if (w->state == WIN_MAXIMIZED) {
        w->x = w->saved_x; w->y = w->saved_y;
        w->w = w->saved_w; w->h = w->saved_h;
        w->state = WIN_NORMAL;
    } else {
        if (!w->half_snapped) {  /* don't overwrite snap-saved dims with half-snap dims */
            w->saved_x = w->x; w->saved_y = w->y;
            w->saved_w = w->w; w->saved_h = w->h;
        }
        w->x = 0; w->y = desk_top();
        w->w = fb_w; w->h = desk_avail();
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

    uint64_t fb_w = console_fb_width();
    /* Per-window type minimum: Settings needs enough width for its content columns */
    uint64_t _fw = console_font_width(), _fh = console_font_height();
    /* Settings minimum: enough for 8 accent swatches + label column + padding */
    uint64_t _settings_min = 2u*(uint64_t)BORDER + 2u*12u + 18u*_fw + 8u*(_fh + 14u);
    if (_settings_min < (uint64_t)MIN_WIN_W) _settings_min = (uint64_t)MIN_WIN_W;
    int64_t  mw  = (w->type == WIN_SETTINGS)
                   ? (int64_t)_settings_min
                   : (int64_t)MIN_WIN_W;
    int64_t  mh   = (int64_t)MIN_WIN_H;
    int64_t  dtop = (int64_t)desk_top();
    int64_t  dbot = (int64_t)desk_bot();

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
    if (nx < 0) nx = 0;
    if (ny < dtop) ny = dtop;
    if (nx + nw > (int64_t)fb_w) {
        if (g_resize_dir == RES_W || g_resize_dir == RES_NW || g_resize_dir == RES_SW)
            nx = (int64_t)fb_w - nw;
        else
            nw = (int64_t)fb_w - nx;
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
