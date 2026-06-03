#include "gui_internal.h"

/* ── File browser ────────────────────────────────────────────────────── */

void fb_str_copy(char *dst, const char *src, int maxlen) {
    int i;
    for (i = 0; i < maxlen - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

void fb_path_join(char *out, const char *parent, const char *child) {
    int plen = (int)gui_strlen(parent);
    (void)gui_strlen(child);
    if (plen == 1 && parent[0] == '/') {
        out[0] = '/';
        fb_str_copy(out + 1, child, 126);
    } else {
        fb_str_copy(out, parent, 128);
        out[plen] = '/';
        fb_str_copy(out + plen + 1, child, 127 - plen);
    }
}

void fb_path_parent(char *out, const char *path) {
    int len = (int)gui_strlen(path);
    int i   = len - 1;
    while (i > 0 && path[i] != '/') i--;
    if (i == 0) { out[0] = '/'; out[1] = '\0'; }
    else { fb_str_copy(out, path, i + 1); out[i] = '\0'; }
}

static bool fb_has_ext(const char *name, const char *ext) {
    size_t nl = gui_strlen(name), el = gui_strlen(ext);
    if (nl <= el) return false;
    const char *tail = name + nl - el;
    while (*tail && *ext) if (*tail++ != *ext++) return false;
    return *ext == '\0';
}

bool fb_is_viewable(const char *name) {
    return fb_has_ext(name, ".txt") || fb_has_ext(name, ".log") ||
           fb_has_ext(name, ".md")  || fb_has_ext(name, ".ini") ||
           fb_has_ext(name, ".cfg") || fb_has_ext(name, ".sh")  ||
           fb_has_ext(name, ".c")   || fb_has_ext(name, ".h")   ||
           fb_has_ext(name, ".cpp") || fb_has_ext(name, ".hpp") ||
           fb_has_ext(name, ".s")   || fb_has_ext(name, ".asm") ||
           fb_has_ext(name, ".json")|| fb_has_ext(name, ".xml") ||
           fb_has_ext(name, ".toml")|| fb_has_ext(name, ".py")  ||
           fb_has_ext(name, ".lua") || fb_has_ext(name, ".js")  ||
           fb_has_ext(name, ".ts")  || fb_has_ext(name, ".jsx") ||
           fb_has_ext(name, ".tsx") || fb_has_ext(name, ".mjs") ||
           fb_has_ext(name, ".mk")  || fb_has_ext(name, ".yml") ||
           fb_has_ext(name, ".yaml")|| fb_has_ext(name, ".html")||
           fb_has_ext(name, ".htm") || fb_has_ext(name, ".css") ||
           fb_has_ext(name, ".diff")|| fb_has_ext(name, ".patch");
}

static bool fb_is_image(const char *name) {
    return fb_has_ext(name, ".bmp") || fb_has_ext(name, ".ppm") ||
           fb_has_ext(name, ".pgm") || fb_has_ext(name, ".png") ||
           fb_has_ext(name, ".jpg") || fb_has_ext(name, ".jpeg");
}

/* Returns icon string and sets *col */
static const char *fb_file_icon(const char *name, uint32_t *col) {
    if (fb_has_ext(name, ".txt") || fb_has_ext(name, ".md") ||
        fb_has_ext(name, ".log")) {
        *col = COL_FB_TXT; return "[T]";
    }
    if (fb_has_ext(name, ".ini") || fb_has_ext(name, ".cfg") ||
        fb_has_ext(name, ".json") || fb_has_ext(name, ".xml") ||
        fb_has_ext(name, ".toml") || fb_has_ext(name, ".yml") ||
        fb_has_ext(name, ".yaml")) {
        *col = 0x0090b8a0u; return "[T]";  /* config files: soft green */
    }
    if (fb_has_ext(name, ".html") || fb_has_ext(name, ".htm") ||
        fb_has_ext(name, ".css")) {
        *col = 0x00e0a060u; return "[W]";  /* web files: warm orange */
    }
    if (fb_has_ext(name, ".diff") || fb_has_ext(name, ".patch")) {
        *col = 0x00a0d080u; return "[D]";  /* diff/patch: green */
    }
    if (fb_has_ext(name, ".c") || fb_has_ext(name, ".h") ||
        fb_has_ext(name, ".cpp") || fb_has_ext(name, ".hpp") ||
        fb_has_ext(name, ".s")   || fb_has_ext(name, ".asm") ||
        fb_has_ext(name, ".rs")) {
        *col = COL_FB_CODE; return "[C]";
    }
    if (fb_has_ext(name, ".sh") || fb_has_ext(name, ".py") ||
        fb_has_ext(name, ".lua") || fb_has_ext(name, ".js") ||
        fb_has_ext(name, ".ts") || fb_has_ext(name, ".jsx") ||
        fb_has_ext(name, ".tsx") || fb_has_ext(name, ".mjs")) {
        *col = COL_FB_SCRIPT; return "[S]";
    }
    if (fb_has_ext(name, ".png") || fb_has_ext(name, ".jpg") ||
        fb_has_ext(name, ".bmp") || fb_has_ext(name, ".psf")) {
        *col = COL_FB_IMG; return "[I]";
    }
    if (fb_has_ext(name, ".bin") || fb_has_ext(name, ".o") ||
        fb_has_ext(name, ".elf") || fb_has_ext(name, ".iso")) {
        *col = COL_FB_BIN; return "[B]";
    }
    *col = COL_FB_TXT; return "[ ]";
}

static char s_listbuf[4096];

void fb_load(fb_state_t *fb, const char *path) {
    fb_str_copy(fb->path, path, 128);
    fb->entry_count  = 0;
    fb->scroll       = 0;
    fb->hover_row     = -1;
    fb->sel_row       = -1;
    fb->sel_anchor    = -1;
    fb->path_hov_char = -1;
    fb->toolbar_hover = -1;
    if (fb->size_col_chars < 4 || fb->size_col_chars > 16) fb->size_col_chars = 7;
    fb->col_drag_active = false;
    /* preserve view_mode across directory changes */
    for (int _mi = 0; _mi < FB_MAX_ENTRIES; _mi++) fb->multi_sel[_mi] = false;
    fb->search_query[0] = '\0';
    fb->search_len      = 0;
    /* don't clear search_active here — preserve focus across directory changes */

    size_t n = vfs_listdir(path, s_listbuf, sizeof(s_listbuf) - 1);
    if (n == 0) return;
    s_listbuf[n] = '\0';

    char *p = s_listbuf, *end = s_listbuf + n;
    while (p < end && fb->entry_count < FB_MAX_ENTRIES) {
        char *nl = p;
        while (nl < end && *nl != '\n') nl++;
        int len = (int)(nl - p);
        if (len > 0 && len < 127) {
            /* Skip hidden entries unless show_hidden is set */
            if (!fb->show_hidden && p[0] == '.') {
                p = nl + 1; continue;
            }
            fb_str_copy(fb->entries[fb->entry_count], p, len + 1);
            fb->entries[fb->entry_count][len] = '\0';
            char full[256];
            fb_path_join(full, path, fb->entries[fb->entry_count]);
            fb->is_dir[fb->entry_count] = (vfs_isdir(full) == 1);
            int fsz = fb->is_dir[fb->entry_count] ? 0 : vfs_filesize(full);
            fb->file_sizes[fb->entry_count] = (uint32_t)(fsz > 0 ? fsz : 0);
            fb->entry_count++;
        }
        p = nl + 1;
    }

    /* Sort: directories always first, then by sort_by/sort_rev within each group */
    for (int i = 1; i < fb->entry_count; i++) {
        char     tmp_name[128];
        bool     tmp_dir  = fb->is_dir[i];
        uint32_t tmp_size = fb->file_sizes[i];
        fb_str_copy(tmp_name, fb->entries[i], 128);
        int j = i - 1;
        while (j >= 0) {
            bool a_dir = fb->is_dir[j];
            bool b_dir = tmp_dir;
            int cmp;
            if (a_dir && !b_dir) { cmp = -1; }
            else if (!a_dir && b_dir) { cmp = 1; }
            else {
                if (fb->sort_by == 1) {
                    uint32_t sa = fb->file_sizes[j];
                    uint32_t sb = tmp_size;
                    cmp = (sa < sb) ? -1 : (sa > sb) ? 1 : 0;
                } else {
                    const char *a = fb->entries[j];
                    const char *b = tmp_name;
                    int k = 0;
                    while (a[k] && b[k] && a[k] == b[k]) k++;
                    unsigned char ca = (unsigned char)(a[k] >= 'A' && a[k] <= 'Z' ? a[k]+32 : a[k]);
                    unsigned char cb = (unsigned char)(b[k] >= 'A' && b[k] <= 'Z' ? b[k]+32 : b[k]);
                    cmp = (ca < cb) ? -1 : (ca > cb) ? 1 : 0;
                }
                if (fb->sort_rev) cmp = -cmp;
            }
            if (cmp <= 0) break;
            fb_str_copy(fb->entries[j + 1], fb->entries[j], 128);
            fb->is_dir[j + 1]    = fb->is_dir[j];
            fb->file_sizes[j + 1] = fb->file_sizes[j];
            j--;
        }
        fb_str_copy(fb->entries[j + 1], tmp_name, 128);
        fb->is_dir[j + 1]    = tmp_dir;
        fb->file_sizes[j + 1] = tmp_size;
    }
}

void fb_navigate(fb_state_t *fb, const char *path) {
    /* Push current path to back stack, clear forward stack */
    if (fb->hist_depth < FB_HIST_MAX)
        fb_str_copy(fb->hist[fb->hist_depth++], fb->path, 128);
    fb->fwd_depth = 0;
    fb_load(fb, path);
}

void fb_back(fb_state_t *fb) {
    if (fb->hist_depth == 0) return;
    /* Push current to forward stack */
    if (fb->fwd_depth < FB_HIST_MAX)
        fb_str_copy(fb->fwd_hist[fb->fwd_depth++], fb->path, 128);
    char prev[128];
    fb_str_copy(prev, fb->hist[--fb->hist_depth], 128);
    fb_load(fb, prev);
}

void fb_forward(fb_state_t *fb) {
    if (fb->fwd_depth == 0) return;
    /* Push current to back stack */
    if (fb->hist_depth < FB_HIST_MAX)
        fb_str_copy(fb->hist[fb->hist_depth++], fb->path, 128);
    char next[128];
    fb_str_copy(next, fb->fwd_hist[--fb->fwd_depth], 128);
    fb_load(fb, next);
}

/* Case-insensitive substring match */
static bool fb_name_matches(const char *name, const char *query) {
    if (!query || !query[0]) return true;  /* empty query = match all */
    /* simple case-insensitive substring search */
    for (size_t i = 0; name[i]; i++) {
        size_t j = 0;
        while (query[j] && name[i+j]) {
            char nc = name[i+j], qc = query[j];
            if (nc >= 'A' && nc <= 'Z') nc += 32;
            if (qc >= 'A' && qc <= 'Z') qc += 32;
            if (nc != qc) break;
            j++;
        }
        if (!query[j]) return true;
    }
    return false;
}

/* Draw one button in the toolbar */
static void fb_draw_toolbar_btn(uint64_t bx, uint64_t by, uint64_t bw, uint64_t bh,
                                 const char *label, bool enabled, bool hovered) {
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();
    uint32_t bg  = !enabled   ? COL_FB_BTN_DIS
                 : hovered    ? COL_FB_BTN_HOV
                 : COL_FB_BTN;
    uint32_t fg  = enabled ? COL_FB_BTN_FG  : COL_FB_BTN_DIS_FG;
    console_fill_rect(bx, by, bw, bh, bg);
    /* 1-px border */
    uint32_t top_col = !enabled ? 0x00161a22u : hovered ? 0x005898e8u : 0x00304858u;
    console_fill_rect(bx, by, bw, 1, top_col);
    console_fill_rect(bx, by+bh-1, bw, 1, 0x00090c12u);
    uint64_t llen = (uint64_t)gui_strlen(label);
    uint64_t lpx  = bx + (bw - llen*fw)/2u;
    uint64_t lpy  = by + (bh - fh)/2u;
    gui_draw_str(lpx, lpy, label, fg, bg);
}

/* Full file browser render */
/* Redraw only the two toolbar rows of the file browser (nav + search).
 * Called from fb_on_motion when only toolbar/path hover changed, avoiding
 * the expensive full file-list repaint (~2M pixels) on every cursor move
 * near the title bar area. */
static void fb_render_toolbar(window_t *w) {
    win_draw_chrome(w, true);
    uint64_t fw  = console_font_width();
    uint64_t fh  = console_font_height();
    uint64_t ix  = w->x + BORDER;
    uint64_t iy  = w->y + TITLE_H;
    uint64_t iw  = w->w - 2u * BORDER;

    /* ── Toolbar row 1: nav buttons + path bar ── */
    uint64_t r1_h   = FB_ROW1_H;
    uint64_t btn_h  = fh + 6u;
    uint64_t btn_y  = iy + (r1_h - btn_h) / 2u;
    bool can_back   = (w->fb.hist_depth > 0);
    bool can_up     = !(w->fb.path[0] == '/' && w->fb.path[1] == '\0');

    console_fill_rect(ix, iy, iw, r1_h, COL_FB_TOOLBAR);
    console_fill_rect(ix, iy + r1_h - 1, iw, 1, COL_FB_SEP);

    bool can_fwd  = (w->fb.fwd_depth > 0);
    int tbhov     = w->fb.toolbar_hover;
    uint64_t bb_x = ix + 4u;
    fb_draw_toolbar_btn(bb_x, btn_y, FB_BTN_W, btn_h, "<", can_back, tbhov == 0);

    uint64_t fb_x = bb_x + FB_BTN_W + 2u;
    fb_draw_toolbar_btn(fb_x, btn_y, FB_BTN_W, btn_h, ">", can_fwd,  tbhov == 1);

    uint64_t ub_x = fb_x + FB_BTN_W + 4u;
    fb_draw_toolbar_btn(ub_x, btn_y, FB_BTN_W, btn_h, "^", can_up,   tbhov == 2);

    uint64_t rb_x = ub_x + FB_BTN_W + 4u;
    fb_draw_toolbar_btn(rb_x, btn_y, FB_BTN_W, btn_h, "R", true,     tbhov == 3);

    /* View-mode toggle button: "=" list, "#" icons */
    uint64_t vb_x = rb_x + FB_BTN_W + 4u;
    const char *view_icon = (w->fb.view_mode == FB_VIEW_ICONS) ? "=" : "#";
    fb_draw_toolbar_btn(vb_x, btn_y, FB_BTN_W, btn_h, view_icon, true, tbhov == 4);

    /* Path bar */
    uint64_t pb_x = vb_x + FB_BTN_W + 6u;
    uint64_t pb_w = iw - (pb_x - ix) - 4u;
    console_fill_rect(pb_x, btn_y, pb_w, btn_h, COL_FB_PATH_BG);
    console_fill_rect(pb_x, btn_y, pb_w, 1, 0x00253545u);
    console_fill_rect(pb_x, btn_y+btn_h-1, pb_w, 1, 0x00050810u);
    uint64_t pb_max = (pb_w > 2u*fw) ? (pb_w - 2u*fw) / fw : 0u;
    {
        /* Draw path with segment hover highlighting */
        const char *path = w->fb.path;
        int plen = (int)gui_strlen(path);
        uint64_t py2 = btn_y + (btn_h - fh) / 2u;
        int hov = w->fb.path_hov_char;

        /* Find hovered segment [seg0, seg1) */
        int seg0 = 0, seg1 = 0;
        if (hov >= 0 && hov < plen) {
            if (path[hov] == '/') {
                seg0 = hov; seg1 = hov + 1;
            } else {
                seg0 = hov; while (seg0 > 0 && path[seg0-1] != '/') seg0--;
                seg1 = hov; while (seg1 < plen && path[seg1] != '/') seg1++;
            }
        }

        for (int ci = 0; ci < plen && (uint64_t)ci < pb_max; ci++) {
            bool in_seg = (hov >= 0) && (ci >= seg0 && ci < seg1);
            uint32_t fg = in_seg ? 0x00e8f4ffu : COL_FB_PATH_FG;
            uint32_t bg = in_seg ? 0x00182840u : COL_FB_PATH_BG;
            uint64_t cx = pb_x + fw + (uint64_t)ci * fw;
            if (in_seg && ci == seg0)
                console_fill_rect(cx, btn_y + 1u, (uint64_t)(seg1 - seg0) * fw, btn_h - 2u, bg);
            unsigned char ch = (unsigned char)path[ci];
            if (ch >= 32 && ch < 127)
                console_render_glyph(cx, py2, ch, fg, bg);
        }
    }

    /* ── Toolbar row 2: search bar (or create-file/dir input overlay) ── */
    uint64_t r2_y = iy + r1_h;
    uint64_t r2_h = r1_h;
    console_fill_rect(ix, r2_y, iw, r2_h, COL_FB_TOOLBAR);
    console_fill_rect(ix, r2_y + r2_h - 1, iw, 1, COL_FB_SEP);

    if (w->fb.input_active) {
        /* New file/dir creation or rename prompt */
        const char *prompt = w->fb.input_is_rename ? "Rename:"
                           : (w->fb.input_isdir    ? "New Dir:" : "New File:");
        uint64_t pi_x = ix + 4u;
        gui_draw_str(pi_x, r2_y + (r2_h - fh)/2u, prompt, 0x0050a8e0u, COL_FB_TOOLBAR);
        int prompt_len = w->fb.input_is_rename ? 8 : (w->fb.input_isdir ? 9 : 10);
        uint64_t pi_label_w = (uint64_t)prompt_len * fw;
        uint64_t pi_x2 = pi_x + pi_label_w;
        uint64_t pi_w  = iw - (pi_x2 - ix) - 4u;
        uint32_t pi_bg = 0x00001830u;
        console_fill_rect(pi_x2, r2_y + (r2_h - btn_h)/2u, pi_w, btn_h, pi_bg);
        console_fill_rect(pi_x2, r2_y + (r2_h - btn_h)/2u, pi_w, 1u, 0x004488ccu);
        uint64_t piq_max = (pi_w > 2*fw) ? (pi_w - 2*fw) / fw - 1u : 0;
        /* scroll so cursor stays in view */
        int pi_scroll = w->fb.input_cursor - (int)piq_max + 1;
        if (pi_scroll < 0) pi_scroll = 0;
        /* draw visible portion of input text */
        int vis_end = pi_scroll + (int)piq_max;
        if (vis_end > w->fb.input_len) vis_end = w->fb.input_len;
        int vis_len = vis_end - pi_scroll;
        if (vis_len > 0) {
            char vis_buf[128];
            for (int _vi = 0; _vi < vis_len; _vi++) vis_buf[_vi] = w->fb.input_buf[pi_scroll + _vi];
            vis_buf[vis_len] = '\0';
            gui_draw_str(pi_x2 + fw, r2_y + (r2_h - btn_h)/2u + (btn_h - fh)/2u,
                         vis_buf, COL_FB_SEARCH_FG, pi_bg);
        }
        if ((g_gui_tick / 25u) % 2u == 0u) {
            uint64_t cur_x = pi_x2 + fw + (uint64_t)(w->fb.input_cursor - pi_scroll) * fw;
            if (cur_x < pi_x2 + pi_w - fw)
                console_fill_rect(cur_x, r2_y + (r2_h - btn_h)/2u + 2u,
                                  2u, btn_h - 4u, 0x004488ccu);
        }
    } else {
        /* Search icon label */
        uint64_t si_x = ix + 4u;
        gui_draw_str(si_x, r2_y + (r2_h - fh)/2u, "Search:", COL_FB_SB_FG, COL_FB_TOOLBAR);
        uint64_t sb_x   = si_x + 8u * fw;
        uint64_t srch_w = iw - (sb_x - ix) - 4u;
        uint32_t sb_bg  = w->fb.search_active ? COL_FB_SEARCH_ACT : COL_FB_SEARCH_BG;
        console_fill_rect(sb_x, r2_y + (r2_h - btn_h)/2u, srch_w, btn_h, sb_bg);
        console_fill_rect(sb_x, r2_y + (r2_h - btn_h)/2u, srch_w, 1,
                          w->fb.search_active ? COL_FB_SEARCH_CUR : 0x00253545u);
        uint64_t sq_max = (srch_w > 2*fw) ? (srch_w - 2*fw) / fw - 1u : 0;
        if (w->fb.search_len > 0) {
            gui_draw_str_clip(sb_x + fw, r2_y + (r2_h - btn_h)/2u + (btn_h - fh)/2u,
                              w->fb.search_query, COL_FB_SEARCH_FG, sb_bg, sq_max);
        } else {
            gui_draw_str_clip(sb_x + fw, r2_y + (r2_h - btn_h)/2u + (btn_h - fh)/2u,
                              "Type to filter...", COL_FB_MUTED, sb_bg, sq_max);
        }
        if (w->fb.search_active && (g_gui_tick / 25u) % 2u == 0u) {
            uint64_t cur_x = sb_x + fw + (uint64_t)w->fb.search_len * fw;
            if (cur_x < sb_x + srch_w - fw)
                console_fill_rect(cur_x, r2_y + (r2_h - btn_h)/2u + 2u,
                                  2u, btn_h - 4u, COL_FB_SEARCH_CUR);
        }
    }
}

void fb_render(window_t *w) {
    fb_render_toolbar(w);
    uint64_t fw  = console_font_width();
    uint64_t fh  = console_font_height();
    uint64_t ix  = w->x + BORDER;
    uint64_t iy  = w->y + TITLE_H;
    uint64_t iw  = w->w - 2u * BORDER;
    uint64_t ih  = w->h - TITLE_H - BORDER;
    uint64_t tb_h = FB_TOOLBAR_H;

    /* ── Body: sidebar + file list ── */
    uint64_t body_y = iy + tb_h;
    uint64_t sb_h_avail = ih - tb_h - FB_STATUSBAR_H;

    /* Sidebar */
    uint64_t sb_w = FB_SIDEBAR_W;
    console_fill_rect(ix, body_y, sb_w, sb_h_avail, COL_FB_SIDEBAR);
    console_fill_rect(ix + sb_w, body_y, 1, sb_h_avail, COL_FB_SEP);

    /* Sidebar header */
    console_fill_rect(ix, body_y, sb_w, fh + 4u, COL_FB_TOOLBAR);
    gui_draw_str(ix + 4u, body_y + 2u, "Places", 0x00506878u, COL_FB_TOOLBAR);

    static const char *sb_labels[] = { "Root /", "/bin", "/etc", "/dev", "/usr", "/tmp", "/home", NULL };
    static const char *sb_paths[]  = { "/",     "/bin", "/etc", "/dev", "/usr", "/tmp", "/home", NULL };
    uint64_t sb_row_h = fh + 6u;
    uint64_t sb_row_y = body_y + fh + 4u;
    for (int i = 0; sb_labels[i] != NULL; i++) {
        if (sb_row_y + sb_row_h > body_y + sb_h_avail) break;
        bool active = gui_streq(w->fb.path, sb_paths[i]);
        uint32_t bg = active ? COL_FB_SB_SEL : COL_FB_SIDEBAR;
        uint32_t fg = active ? COL_FB_SB_SEL_FG : COL_FB_SB_FG;
        console_fill_rect(ix, sb_row_y, sb_w, sb_row_h, bg);
        if (active)
            console_fill_rect(ix, sb_row_y, 3, sb_row_h, COL_FB_BTN_ACT);
        gui_draw_str(ix + 7u, sb_row_y + (sb_row_h - fh) / 2u,
                     sb_labels[i], fg, bg);
        sb_row_y += sb_row_h;
    }
    /* fill remainder of sidebar */
    if (sb_row_y < body_y + sb_h_avail)
        console_fill_rect(ix, sb_row_y, sb_w, body_y + sb_h_avail - sb_row_y, COL_FB_SIDEBAR);

    /* ── File list panel ── */
    uint64_t lx = ix + sb_w + 1u;
    uint64_t lw = iw - sb_w - 1u;
    uint64_t ly = body_y;
    uint64_t lh = sb_h_avail;

    if (w->fb.view_mode == FB_VIEW_LIST) {
    /* ── LIST VIEW ── */
    /* Column header */
    uint64_t hdr_h = fh + 4u;
    console_fill_rect(lx, ly, lw, hdr_h, COL_FB_TOOLBAR);
    console_fill_rect(lx, ly + hdr_h - 1, lw, 1, COL_FB_SEP);
    uint64_t icon_col_w = (FB_ICON_COLS + 1u) * fw;
    uint64_t name_col_x = lx + icon_col_w + 4u;
    uint64_t size_col_w = (uint64_t)w->fb.size_col_chars * fw;
    uint64_t size_col_x = lx + lw - size_col_w - 8u;
    (void)name_col_x;
    {
        bool name_active = (w->fb.sort_by == 0);
        bool size_active = (w->fb.sort_by == 1);
        bool name_hov    = (w->fb.header_hover == 0);
        bool size_hov    = (w->fb.header_hover == 1);
        char ind = w->fb.sort_rev ? 'v' : '^';
        uint32_t name_col_fg  = name_active ? 0x0090b8d8u
                              : name_hov    ? 0x00607888u : 0x00384858u;
        uint32_t size_col_fg2 = size_active ? 0x0090b8d8u
                              : size_hov    ? 0x00607888u : 0x00384858u;
        uint32_t name_hdr_bg = name_hov ? 0x00182030u : COL_FB_TOOLBAR;
        uint32_t size_hdr_bg = size_hov ? 0x00182030u : COL_FB_TOOLBAR;
        if (name_hov)
            console_fill_rect(lx, ly, size_col_x - lx, hdr_h, name_hdr_bg);
        if (size_hov)
            console_fill_rect(size_col_x, ly, lx + lw - size_col_x, hdr_h, size_hdr_bg);
        char name_hdr[12] = "   Name ";
        name_hdr[7] = name_active ? ind : ' ';
        name_hdr[8] = '\0';
        gui_draw_str(lx + 2u, ly + 2u, name_hdr, name_col_fg, name_hdr_bg);
        char size_hdr[8] = "Size ";
        size_hdr[4] = size_active ? ind : ' ';
        size_hdr[5] = '\0';
        gui_draw_str(size_col_x, ly + 2u, size_hdr, size_col_fg2, size_hdr_bg);
        uint32_t sep_col = w->fb.col_drag_active ? 0x005888b0u : 0x00223040u;
        console_fill_rect(size_col_x - 1u, ly, 1u, hdr_h, sep_col);
    }
    ly += hdr_h;
    lh -= hdr_h;

    uint64_t row_h   = FB_ROW_H;
    uint64_t max_rows = lh / row_h;
    console_fill_rect(lx, ly, lw, lh, COL_FB_LIST_BG);
    {
        uint32_t _sc = w->fb.col_drag_active ? 0x00253545u : 0x00182030u;
        console_fill_rect(size_col_x - 1u, ly, 1u, lh, _sc);
    }

    int row_idx = 0;
    int skipped = 0;
    int srch_visible = 0;
    for (int i = 0; i < w->fb.entry_count && row_idx < (int)max_rows; i++) {
        if (!fb_name_matches(w->fb.entries[i], w->fb.search_query)) continue;
        srch_visible++;
        if (skipped < w->fb.scroll) { skipped++; continue; }
        uint64_t ry = ly + (uint64_t)row_idx * row_h;
        bool hov = (i == w->fb.hover_row);
        bool sel = (i == w->fb.sel_row) || w->fb.multi_sel[i];
        bool matched = (w->fb.search_len > 0);
        uint32_t row_bg = sel ? COL_FB_SEL :
                          hov ? COL_FB_HOV :
                          matched ? COL_FB_MATCH_HL :
                          (row_idx & 1) ? COL_FB_LIST_ALT : COL_FB_LIST_BG;
        console_fill_rect(lx, ry, lw, row_h, row_bg);
        if (!w->fb.is_dir[i] && g_fb_clip_path[0]) {
            char _cp_check[256];
            fb_path_join(_cp_check, w->fb.path, w->fb.entries[i]);
            if (gui_streq(_cp_check, g_fb_clip_path)) {
                uint32_t clip_col = g_fb_clip_is_cut ? 0x00e8a040u : 0x0040d080u;
                console_fill_rect(lx, ry, 3u, row_h, clip_col);
            }
        }
        const char *icon;
        uint32_t icon_fg;
        if (w->fb.is_dir[i]) {
            icon = "[/]";
            icon_fg = COL_FB_DIR;
        } else {
            icon = fb_file_icon(w->fb.entries[i], &icon_fg);
        }
        gui_draw_str(lx + 2u, ry + (row_h - fh) / 2u, icon, icon_fg, row_bg);
        uint64_t name_avail = size_col_x > name_col_x + fw ? size_col_x - name_col_x - fw : fw;
        uint64_t name_max   = name_avail / fw;
        const char *name = w->fb.entries[i];
        uint32_t name_fg = w->fb.is_dir[i] ? COL_FB_DIR : icon_fg;
        if (w->fb.is_dir[i]) {
            char dirbuf[130];
            size_t nl = gui_strlen(name);
            for (size_t k = 0; k < nl && k < 127; k++) dirbuf[k] = name[k];
            dirbuf[nl < 127 ? nl : 127] = '/';
            dirbuf[nl < 127 ? nl+1 : 128] = '\0';
            gui_draw_str_clip(name_col_x, ry + (row_h - fh) / 2u,
                              dirbuf, name_fg, row_bg, name_max);
        } else {
            gui_draw_str_clip(name_col_x, ry + (row_h - fh) / 2u,
                              name, name_fg, row_bg, name_max);
        }
        if (!w->fb.is_dir[i]) {
            uint32_t sz = w->fb.file_sizes[i];
            char szbuf[16];
            if (sz >= 1024u * 1024u) {
                char n[8]; gui_itoa((int)(sz >> 20), n, 8);
                int si2 = 0; const char *p2;
                for (p2=n; *p2 && si2<12; ) szbuf[si2++]=*p2++;
                for (p2=" MB"; *p2 && si2<14; ) szbuf[si2++]=*p2++;
                szbuf[si2]='\0';
            } else if (sz >= 1024u) {
                char n[8]; gui_itoa((int)(sz >> 10), n, 8);
                int si2 = 0; const char *p2;
                for (p2=n; *p2 && si2<12; ) szbuf[si2++]=*p2++;
                for (p2=" KB"; *p2 && si2<14; ) szbuf[si2++]=*p2++;
                szbuf[si2]='\0';
            } else {
                char n[8]; gui_itoa((int)sz, n, 8);
                int si2 = 0; const char *p2;
                for (p2=n; *p2 && si2<12; ) szbuf[si2++]=*p2++;
                for (p2=" B"; *p2 && si2<14; ) szbuf[si2++]=*p2++;
                szbuf[si2]='\0';
            }
            uint64_t slen2 = (uint64_t)gui_strlen(szbuf);
            uint64_t sx2   = size_col_x + (size_col_w > slen2 * fw
                                           ? size_col_w - slen2 * fw : 0u);
            gui_draw_str(sx2, ry + (row_h - fh) / 2u, szbuf, 0x00405060u, row_bg);
        }
        row_idx++;
    }
    if (row_idx == 0) {
        const char *msg = (w->fb.search_len > 0 && srch_visible == 0 && w->fb.entry_count > 0)
                          ? "(no matches)"
                          : (w->fb.entry_count == 0) ? "(empty directory)" : NULL;
        if (msg) {
            uint64_t mx2 = lx + (lw > (uint64_t)gui_strlen(msg)*fw ? (lw - (uint64_t)gui_strlen(msg)*fw)/2u : 0u);
            uint64_t my2 = ly + lh/2u - fh/2u;
            gui_draw_str(mx2, my2, msg, COL_FB_MUTED, COL_FB_LIST_BG);
        }
    }
    if (w->fb.entry_count > (int)max_rows) {
        uint64_t sb_x  = lx + lw - 6u;
        uint64_t sb_y  = ly;
        uint64_t sb_th = lh;
        console_fill_rect(sb_x, sb_y, 6u, sb_th, 0x000a0e16u);
        uint64_t thumb_h = (max_rows * sb_th) / (uint64_t)w->fb.entry_count;
        if (thumb_h < 8) thumb_h = 8;
        uint64_t thumb_y = sb_y + ((uint64_t)w->fb.scroll * (sb_th - thumb_h))
                           / (uint64_t)(w->fb.entry_count - (int)max_rows + 1);
        {
            int32_t _fmx, _fmy; bool _flb, _frb;
            mouse_get_state(&_fmx, &_fmy, &_flb, &_frb);
            int _win = (int)(w - g_wins);
            bool _fb_drag = (g_sb_drag && g_sb_drag_win == _win && !g_sb_drag_horiz);
            bool _fb_hov  = !_fb_drag &&
                            _fmx >= (int32_t)sb_x && _fmx < (int32_t)(sb_x + 6u) &&
                            _fmy >= (int32_t)sb_y  && _fmy < (int32_t)(sb_y + sb_th);
            uint32_t _ftc = _fb_drag ? 0x0058a0d8u : _fb_hov ? 0x00405870u : 0x00304858u;
            console_fill_rect(sb_x + 1u, thumb_y, 4u, thumb_h, _ftc);
        }
    }

    } else {
    /* ── ICON GRID VIEW ── */
    console_fill_rect(lx, ly, lw, lh, COL_FB_LIST_BG);
    uint64_t cell_w  = (fw < 8u ? 80u : fw * 10u);   /* ~80px per cell */
    uint64_t icon_sz = (cell_w > 16u) ? cell_w - 16u : 40u; /* icon square px */
    if (icon_sz > 56u) icon_sz = 56u;
    uint64_t cell_h  = icon_sz + fh + 14u;            /* icon + label + padding */
    uint64_t cols    = (lw > 4u) ? (lw - 4u) / cell_w : 1u;
    if (cols < 1u) cols = 1u;
    uint64_t rows_vis = (lh > 4u) ? (lh - 4u) / cell_h : 1u;
    if (rows_vis < 1u) rows_vis = 1u;
    int max_vis_items = (int)(rows_vis * cols);

    /* Clamp scroll to item granularity */
    int total_vis = 0;
    for (int i = 0; i < w->fb.entry_count; i++)
        if (fb_name_matches(w->fb.entries[i], w->fb.search_query)) total_vis++;
    int max_sc = total_vis - max_vis_items;
    if (max_sc < 0) max_sc = 0;
    if (w->fb.scroll > max_sc) w->fb.scroll = max_sc;
    if (w->fb.scroll < 0) w->fb.scroll = 0;

    int idx = 0, drawn = 0;
    for (int i = 0; i < w->fb.entry_count && drawn < max_vis_items; i++) {
        if (!fb_name_matches(w->fb.entries[i], w->fb.search_query)) continue;
        if (idx < w->fb.scroll) { idx++; continue; }
        int slot  = drawn;
        uint64_t col  = (uint64_t)(slot % (int)cols);
        uint64_t row  = (uint64_t)(slot / (int)cols);
        uint64_t cx2  = lx + 2u + col * cell_w;
        uint64_t cy2  = ly + 2u + row * cell_h;

        bool sel = (i == w->fb.sel_row) || w->fb.multi_sel[i];
        bool hov = (i == w->fb.hover_row);
        uint32_t cell_bg = sel ? COL_FB_SEL : hov ? COL_FB_HOV : COL_FB_LIST_BG;
        console_fill_rect(cx2, cy2, cell_w - 2u, cell_h - 2u, cell_bg);

        /* Draw icon rectangle */
        uint32_t icon_fg;
        const char *icon_txt;
        uint32_t icon_col;
        if (w->fb.is_dir[i]) {
            icon_col = 0x00305898u;  /* blue folder */
            icon_fg  = 0x0090c8ffu;
            icon_txt = "DIR";
        } else {
            icon_txt = fb_file_icon(w->fb.entries[i], &icon_fg);
            icon_col = (icon_fg >> 1) & 0x007f7f7fu;  /* darken for bg */
        }
        uint64_t ix2  = cx2 + (cell_w - 2u - icon_sz) / 2u;
        uint64_t iy2  = cy2 + 4u;
        console_fill_rect(ix2, iy2, icon_sz, icon_sz, icon_col);
        /* Border on icon */
        console_fill_rect(ix2, iy2, icon_sz, 1u, icon_fg);
        console_fill_rect(ix2, iy2 + icon_sz - 1u, icon_sz, 1u, icon_fg);
        console_fill_rect(ix2, iy2, 1u, icon_sz, icon_fg);
        console_fill_rect(ix2 + icon_sz - 1u, iy2, 1u, icon_sz, icon_fg);
        /* Icon label (ext / type) */
        uint64_t itw = (uint64_t)gui_strlen(icon_txt) * fw;
        uint64_t itx = ix2 + (icon_sz > itw ? (icon_sz - itw) / 2u : 0u);
        uint64_t ity = iy2 + (icon_sz - fh) / 2u;
        gui_draw_str(itx, ity, icon_txt, icon_fg, icon_col);

        /* Filename label below icon */
        const char *name = w->fb.entries[i];
        uint64_t label_max = (cell_w > fw + 4u) ? (cell_w - 4u) / fw : 1u;
        uint64_t ly2b = iy2 + icon_sz + 4u;
        uint64_t nlen = (uint64_t)gui_strlen(name);
        uint64_t label_x = cx2 + (nlen * fw < cell_w - 2u
                                  ? (cell_w - 2u - nlen * fw) / 2u : 0u);
        uint32_t label_fg = w->fb.is_dir[i] ? COL_FB_DIR : 0x00c0ccd8u;
        gui_draw_str_clip(label_x, ly2b, name, label_fg, cell_bg, label_max);

        /* Selection ring */
        if (sel) {
            console_fill_rect(cx2, cy2, cell_w - 2u, 2u, g_theme.accent);
            console_fill_rect(cx2, cy2 + cell_h - 4u, cell_w - 2u, 2u, g_theme.accent);
            console_fill_rect(cx2, cy2, 2u, cell_h - 2u, g_theme.accent);
            console_fill_rect(cx2 + cell_w - 4u, cy2, 2u, cell_h - 2u, g_theme.accent);
        }
        drawn++;
        idx++;
    }

    if (drawn == 0 && total_vis == 0) {
        const char *msg = (w->fb.entry_count == 0) ? "(empty directory)" : "(no matches)";
        uint64_t mx2 = lx + (lw > (uint64_t)gui_strlen(msg)*fw ? (lw - (uint64_t)gui_strlen(msg)*fw)/2u : 0u);
        uint64_t my2 = ly + lh/2u - fh/2u;
        gui_draw_str(mx2, my2, msg, COL_FB_MUTED, COL_FB_LIST_BG);
    }

    /* Scrollbar for icon view */
    if (total_vis > max_vis_items) {
        uint64_t sb_x  = lx + lw - 6u;
        uint64_t sb_y  = ly;
        uint64_t sb_th = lh;
        console_fill_rect(sb_x, sb_y, 6u, sb_th, 0x000a0e16u);
        uint64_t thumb_h = ((uint64_t)max_vis_items * sb_th) / (uint64_t)total_vis;
        if (thumb_h < 8) thumb_h = 8;
        uint64_t thumb_y = max_sc > 0
            ? sb_y + ((uint64_t)w->fb.scroll * (sb_th - thumb_h)) / (uint64_t)max_sc
            : sb_y;
        console_fill_rect(sb_x + 1u, thumb_y, 4u, thumb_h, 0x00304858u);
    }
    } /* end icon grid view */

    /* ── Status bar ── */
    uint64_t stbar_y = iy + ih - FB_STATUSBAR_H;
    console_fill_rect(ix, stbar_y, iw, FB_STATUSBAR_H, COL_FB_STATUSBAR);
    console_fill_rect(ix, stbar_y, iw, 1, COL_FB_SEP);

    /* Item count */
    int dirs = 0, files = 0;
    for (int i = 0; i < w->fb.entry_count; i++) {
        if (w->fb.is_dir[i]) dirs++; else files++;
    }
    char sbuf[64];
    char nbuf[16], dbuf[16], fbuf[16];
    gui_itoa(w->fb.entry_count, nbuf, 16);
    gui_itoa(dirs, dbuf, 16);
    gui_itoa(files, fbuf, 16);
    /* build: "N items (D folders, F files)" */
    {
        int si2 = 0;
        const char *p2;
        for (p2=nbuf; *p2 && si2<60; ) sbuf[si2++]=*p2++;
        for (p2=" items ("; *p2 && si2<60; ) sbuf[si2++]=*p2++;
        for (p2=dbuf; *p2 && si2<60; ) sbuf[si2++]=*p2++;
        for (p2=" folder"; *p2 && si2<60; ) sbuf[si2++]=*p2++;
        if (dirs!=1) { sbuf[si2++]='s'; }
        for (p2=", "; *p2 && si2<60; ) sbuf[si2++]=*p2++;
        for (p2=fbuf; *p2 && si2<60; ) sbuf[si2++]=*p2++;
        for (p2=" file"; *p2 && si2<60; ) sbuf[si2++]=*p2++;
        if (files!=1) { sbuf[si2++]='s'; }
        sbuf[si2++]=')'; sbuf[si2]='\0';
    }
    gui_draw_str(ix + 6u, stbar_y + (FB_STATUSBAR_H - fh)/2u,
                 sbuf, COL_FB_STATUS_FG, COL_FB_STATUSBAR);
    /* Hidden-files indicator */
    uint64_t sbuf_px = ix + 6u + (uint64_t)gui_strlen(sbuf) * fw;
    if (w->fb.show_hidden) {
        const char *hind = "  [+hidden]";
        gui_draw_str(sbuf_px, stbar_y + (FB_STATUSBAR_H - fh)/2u,
                     hind, 0x004870a0u, COL_FB_STATUSBAR);
        sbuf_px += (uint64_t)gui_strlen(hind) * fw;
    }
    /* Sort indicator */
    {
        const char *scol = (w->fb.sort_by == 1) ? "size" : "name";
        char sind[16] = "  ";
        int si2 = 2;
        const char *p2 = scol;
        while (*p2 && si2 < 10) sind[si2++] = *p2++;
        sind[si2++] = w->fb.sort_rev ? 'v' : '^';
        sind[si2] = '\0';
        gui_draw_str(sbuf_px, stbar_y + (FB_STATUSBAR_H - fh)/2u,
                     sind, 0x00284050u, COL_FB_STATUSBAR);
        sbuf_px += (uint64_t)si2 * fw;
    }
    /* Clipboard indicator */
    if (g_fb_clip_path[0]) {
        /* Extract filename from clip path */
        const char *_cfn = g_fb_clip_path;
        for (const char *_cp2 = g_fb_clip_path; *_cp2; _cp2++)
            if (*_cp2 == '/') _cfn = _cp2 + 1;
        char clbuf[48]; int ci = 0; const char *cp2;
        for (cp2 = g_fb_clip_is_cut ? "  [cut: " : "  [copy: "; *cp2 && ci < 46; ) clbuf[ci++] = *cp2++;
        for (cp2 = _cfn; *cp2 && ci < 46; ) clbuf[ci++] = *cp2++;
        if (ci < 47) clbuf[ci++] = ']';
        clbuf[ci] = '\0';
        uint32_t cl_col = g_fb_clip_is_cut ? 0x00e8a040u : 0x0040b870u;
        gui_draw_str(sbuf_px, stbar_y + (FB_STATUSBAR_H - fh)/2u,
                     clbuf, cl_col, COL_FB_STATUSBAR);
    }

    /* Toolbar button hint when hovering */
    if (w->fb.toolbar_hover >= 0) {
        static const char *const tb_hints[5] = { "Back", "Forward", "Up directory", "Refresh", "Toggle view (List/Icons)" };
        int thi = w->fb.toolbar_hover;
        if (thi < 5) {
            const char *hint = tb_hints[thi];
            uint64_t hlen = (uint64_t)gui_strlen(hint);
            uint64_t hx = ix + (iw > hlen * fw ? (iw - hlen * fw) / 2u : 0u);
            gui_draw_str(hx, stbar_y + (FB_STATUSBAR_H - fh)/2u,
                         hint, 0x0070b0e0u, COL_FB_STATUSBAR);
        }
    }

    /* Count total selected entries (sel_row + multi_sel) */
    int total_sel = 0;
    for (int _si = 0; _si < w->fb.entry_count; _si++)
        if (_si == w->fb.sel_row || w->fb.multi_sel[_si]) total_sel++;

    /* Status bar right side: multi-select count or single file info */
    if (total_sel > 1) {
        char selbuf[32]; int si2 = 0; const char *p2;
        char nbuf[8]; gui_itoa(total_sel, nbuf, 8);
        for (p2 = nbuf; *p2 && si2 < 8; ) selbuf[si2++] = *p2++;
        for (p2 = " selected"; *p2 && si2 < 28; ) selbuf[si2++] = *p2++;
        selbuf[si2] = '\0';
        uint64_t slen2 = (uint64_t)gui_strlen(selbuf);
        uint64_t sx2   = ix + iw > slen2 * fw + 8u ? ix + iw - slen2 * fw - 8u : ix;
        gui_draw_str(sx2, stbar_y + (FB_STATUSBAR_H - fh)/2u,
                     selbuf, 0x0060a0d0u, COL_FB_STATUSBAR);
    } else {
        int sel = w->fb.sel_row;
        if (sel >= 0 && sel < w->fb.entry_count && !w->fb.is_dir[sel]) {
            uint32_t fsz = w->fb.file_sizes[sel];
            char selbuf[48];
            const char *nm = w->fb.entries[sel];
            char szbuf[12];
            if (fsz >= 1024u * 1024u) { char n[8]; gui_itoa((int)(fsz>>20),n,8); int si2=0; const char*p2; for(p2=n;*p2&&si2<8;) szbuf[si2++]=*p2++; for(p2=" MB";*p2&&si2<10;) szbuf[si2++]=*p2++; szbuf[si2]='\0'; }
            else if (fsz >= 1024u) { char n[8]; gui_itoa((int)(fsz>>10),n,8); int si2=0; const char*p2; for(p2=n;*p2&&si2<8;) szbuf[si2++]=*p2++; for(p2=" KB";*p2&&si2<10;) szbuf[si2++]=*p2++; szbuf[si2]='\0'; }
            else { char n[8]; gui_itoa((int)fsz,n,8); int si2=0; const char*p2; for(p2=n;*p2&&si2<8;) szbuf[si2++]=*p2++; for(p2=" B";*p2&&si2<10;) szbuf[si2++]=*p2++; szbuf[si2]='\0'; }
            {
                int si2=0; const char *p2;
                for(p2=nm; *p2&&si2<38;) selbuf[si2++]=*p2++;
                for(p2=" ("; *p2&&si2<42;) selbuf[si2++]=*p2++;
                for(p2=szbuf; *p2&&si2<46;) selbuf[si2++]=*p2++;
                selbuf[si2++]=')'; selbuf[si2]='\0';
            }
            uint64_t slen2 = (uint64_t)gui_strlen(selbuf);
            uint64_t sx2   = ix + iw > slen2 * fw + 8u ? ix + iw - slen2 * fw - 8u : ix;
            gui_draw_str(sx2, stbar_y + (FB_STATUSBAR_H - fh)/2u,
                         selbuf, 0x00507080u, COL_FB_STATUSBAR);
        }
    }
}
static void fb_inner(window_t *w, uint64_t *ix, uint64_t *iy,
                     uint64_t *iw, uint64_t *ih) {
    *ix = w->x + BORDER;
    *iy = w->y + TITLE_H;
    *iw = w->w - 2u * BORDER;
    *ih = w->h - TITLE_H - BORDER;
}

/* Returns the list area origin and dimensions */
void fb_list_region(window_t *w,
                            uint64_t *lx, uint64_t *ly,
                            uint64_t *lw, uint64_t *lh) {
    uint64_t ix, iy, iw, ih;
    fb_inner(w, &ix, &iy, &iw, &ih);
    uint64_t fh    = console_font_height();
    uint64_t tb_h  = FB_TOOLBAR_H;
    uint64_t hdr_h = fh + 4u;
    uint64_t stbar = FB_STATUSBAR_H;
    *lx = ix + FB_SIDEBAR_W + 1u;
    *lw = iw - FB_SIDEBAR_W - 1u;
    *ly = iy + tb_h + hdr_h;
    *lh = ih - tb_h - hdr_h - stbar;
}

/* Compute file-browser scrollbar thumb geometry. Returns false if no scrollbar needed. */
static bool fb_sb_thumb(window_t *w,
                        uint64_t *sb_x_out, uint64_t *track_y_out, uint64_t *track_h_out,
                        uint64_t *thumb_y_out, uint64_t *thumb_h_out) {
    uint64_t lx, ly, lw, lh;
    fb_list_region(w, &lx, &ly, &lw, &lh);
    uint64_t max_rows = lh / FB_ROW_H;
    int total = w->fb.entry_count;
    if (total <= (int)max_rows) return false;
    int max_sc = total - (int)max_rows;
    *sb_x_out   = lx + lw - 6u;
    *track_y_out = ly;
    *track_h_out = lh;
    uint64_t th  = (max_rows * lh) / (uint64_t)total;
    if (th < 8u) th = 8u;
    uint64_t ty  = ly + ((uint64_t)w->fb.scroll * (lh - th)) / (uint64_t)max_sc;
    *thumb_y_out = ty;
    *thumb_h_out = th;
    return true;
}

/* Returns which entry index is at pixel (mx,my), accounting for search filter */
int fb_hit_row(window_t *w, int32_t mx, int32_t my) {
    uint64_t lx, ly, lw, lh;
    fb_list_region(w, &lx, &ly, &lw, &lh);
    if ((uint64_t)mx < lx || (uint64_t)mx >= lx + lw) return -1;
    if ((uint64_t)my < ly || (uint64_t)my >= ly + lh) return -1;
    uint64_t row_h    = FB_ROW_H;
    uint64_t max_rows = lh / row_h;
    int row = (int)((uint64_t)my - ly) / (int)row_h;
    if (row < 0 || (uint64_t)row >= max_rows) return -1;
    /* Walk entries with filter to map rendered row → entry index */
    int row_idx = 0, skipped = 0;
    for (int i = 0; i < w->fb.entry_count; i++) {
        if (!fb_name_matches(w->fb.entries[i], w->fb.search_query)) continue;
        if (skipped < w->fb.scroll) { skipped++; continue; }
        if (row_idx == row) return i;
        if (++row_idx >= (int)max_rows) break;
    }
    return -1;
}

/* Returns sidebar item index hit (or -1) */
static int fb_hit_sidebar(window_t *w, int32_t mx, int32_t my) {
    uint64_t ix, iy, iw, ih;
    fb_inner(w, &ix, &iy, &iw, &ih);
    (void)iw; (void)ih;
    uint64_t fh   = console_font_height();
    uint64_t tb_h = FB_TOOLBAR_H;
    uint64_t body_y = iy + tb_h;
    uint64_t sb_w   = FB_SIDEBAR_W;

    if ((uint64_t)mx < ix || (uint64_t)mx >= ix + sb_w) return -1;

    uint64_t hdr_h  = fh + 4u;
    uint64_t sb_row_h = fh + 6u;
    uint64_t sb_start = body_y + hdr_h;

    if ((uint64_t)my < sb_start) return -1;
    int item = (int)((uint64_t)my - sb_start) / (int)sb_row_h;
    static const char *sb_paths[] = {"/", "/bin", "/etc", "/dev", "/usr", "/tmp", "/home", NULL};
    int count = 0;
    while (sb_paths[count]) count++;
    if (item < 0 || item >= count) return -1;
    return item;
}

/* Check toolbar hit: 0=back,1=up,2=refresh,3=search_bar,-1=miss */
static int fb_hit_toolbar(window_t *w, int32_t mx, int32_t my) {
    uint64_t ix, iy, iw, ih;
    fb_inner(w, &ix, &iy, &iw, &ih);
    (void)ih;
    uint64_t fh    = console_font_height();
    uint64_t r1_h  = FB_ROW1_H;
    uint64_t btn_h = fh + 6u;
    uint64_t btn_y = iy + (r1_h - btn_h) / 2u;

    /* Row 1 buttons and path bar */
    if ((uint64_t)my >= btn_y && (uint64_t)my < btn_y + btn_h) {
        uint64_t bb_x = ix + 4u;
        uint64_t fb_x = bb_x + FB_BTN_W + 2u;
        uint64_t ub_x = fb_x + FB_BTN_W + 4u;
        uint64_t rb_x = ub_x + FB_BTN_W + 4u;
        uint64_t vb_x = rb_x + FB_BTN_W + 4u;   /* view toggle */
        uint64_t pb_x = vb_x + FB_BTN_W + 6u;
        uint64_t pb_w = iw > (pb_x - ix) + 4u ? iw - (pb_x - ix) - 4u : 0u;
        if ((uint64_t)mx >= bb_x && (uint64_t)mx < bb_x + FB_BTN_W) return 0; /* back */
        if ((uint64_t)mx >= fb_x && (uint64_t)mx < fb_x + FB_BTN_W) return 5; /* forward */
        if ((uint64_t)mx >= ub_x && (uint64_t)mx < ub_x + FB_BTN_W) return 1; /* up */
        if ((uint64_t)mx >= rb_x && (uint64_t)mx < rb_x + FB_BTN_W) return 2; /* refresh */
        if ((uint64_t)mx >= vb_x && (uint64_t)mx < vb_x + FB_BTN_W) return 6; /* view toggle */
        if ((uint64_t)mx >= pb_x && (uint64_t)mx < pb_x + pb_w)     return 4; /* path bar */
    }

    /* Row 2: search bar */
    uint64_t r2_y  = iy + r1_h;
    uint64_t r2_h  = r1_h;
    (void)btn_h;
    if ((uint64_t)my >= r2_y && (uint64_t)my < r2_y + r2_h)
        return 3;

    return -1;
}

/* Returns 0=Name, 1=Size, -1=miss for column header clicks */
static int fb_hit_header(window_t *w, int32_t mx, int32_t my) {
    uint64_t ix, iy, iw, ih;
    fb_inner(w, &ix, &iy, &iw, &ih);
    (void)ih;
    uint64_t fh     = console_font_height();
    uint64_t fw     = console_font_width();
    uint64_t hdr_h  = fh + 4u;
    uint64_t body_y = iy + FB_TOOLBAR_H;
    uint64_t lx     = ix + FB_SIDEBAR_W + 1u;
    uint64_t lw     = iw - FB_SIDEBAR_W - 1u;
    if ((uint64_t)my < body_y || (uint64_t)my >= body_y + hdr_h) return -1;
    if ((uint64_t)mx < lx || (uint64_t)mx >= lx + lw) return -1;
    int scc = w->fb.size_col_chars < 4 ? 7 : w->fb.size_col_chars;
    uint64_t size_col_x = lx + lw - (uint64_t)scc * fw - 8u;
    if ((uint64_t)mx >= size_col_x) return 1;
    return 0;
}

/* Returns true if mx,my is within 3px of the name/size column separator */
static bool fb_hit_col_sep(window_t *w, int32_t mx, int32_t my) {
    uint64_t ix, iy, iw, ih;
    fb_inner(w, &ix, &iy, &iw, &ih);
    (void)ih;
    uint64_t fh     = console_font_height();
    uint64_t fw     = console_font_width();
    uint64_t hdr_h  = fh + 4u;
    uint64_t body_y = iy + FB_TOOLBAR_H;
    uint64_t lx     = ix + FB_SIDEBAR_W + 1u;
    uint64_t lw     = iw - FB_SIDEBAR_W - 1u;
    if ((uint64_t)my < body_y || (uint64_t)my >= body_y + hdr_h) return false;
    int scc = w->fb.size_col_chars < 4 ? 7 : w->fb.size_col_chars;
    uint64_t size_col_x = lx + lw - (uint64_t)scc * fw - 8u;
    int32_t sep_x = (int32_t)size_col_x;
    return (mx >= sep_x - 3 && mx <= sep_x + 3);
}

/* Draw a single file-list row. Used by fb_render_hover_rows for O(1) hover updates. */
static void fb_draw_list_row(window_t *w, int i, int row_idx,
                              uint64_t lx, uint64_t lw, uint64_t ry, uint64_t row_h,
                              uint64_t fh, uint64_t fw,
                              uint64_t name_col_x, uint64_t size_col_x, uint64_t size_col_w) {
    bool hov     = (i == w->fb.hover_row);
    bool sel     = (i == w->fb.sel_row) || w->fb.multi_sel[i];
    bool matched = (w->fb.search_len > 0);
    uint32_t row_bg = sel     ? COL_FB_SEL :
                      hov     ? COL_FB_HOV :
                      matched ? COL_FB_MATCH_HL :
                      (row_idx & 1) ? COL_FB_LIST_ALT : COL_FB_LIST_BG;
    console_fill_rect(lx, ry, lw, row_h, row_bg);
    if (!w->fb.is_dir[i] && g_fb_clip_path[0]) {
        char _cp[256]; fb_path_join(_cp, w->fb.path, w->fb.entries[i]);
        if (gui_streq(_cp, g_fb_clip_path)) {
            uint32_t cc = g_fb_clip_is_cut ? 0x00e8a040u : 0x0040d080u;
            console_fill_rect(lx, ry, 3u, row_h, cc);
        }
    }
    const char *icon; uint32_t icon_fg;
    if (w->fb.is_dir[i]) { icon = "[/]"; icon_fg = COL_FB_DIR; }
    else                  icon = fb_file_icon(w->fb.entries[i], &icon_fg);
    gui_draw_str(lx + 2u, ry + (row_h - fh) / 2u, icon, icon_fg, row_bg);
    uint64_t name_avail = size_col_x > name_col_x + fw ? size_col_x - name_col_x - fw : fw;
    uint64_t name_max   = name_avail / fw;
    const char *name    = w->fb.entries[i];
    uint32_t    name_fg = w->fb.is_dir[i] ? COL_FB_DIR : icon_fg;
    if (w->fb.is_dir[i]) {
        char db[130]; size_t nl = gui_strlen(name);
        for (size_t k = 0; k < nl && k < 127; k++) db[k] = name[k];
        db[nl < 127 ? nl : 127] = '/'; db[nl < 127 ? nl+1 : 128] = '\0';
        gui_draw_str_clip(name_col_x, ry + (row_h - fh) / 2u, db, name_fg, row_bg, name_max);
    } else {
        gui_draw_str_clip(name_col_x, ry + (row_h - fh) / 2u, name, name_fg, row_bg, name_max);
    }
    if (!w->fb.is_dir[i]) {
        uint32_t sz = w->fb.file_sizes[i];
        char sb[16];
        if (sz >= 1024u*1024u) {
            char n[8]; gui_itoa((int)(sz>>20),n,8); int si=0; const char *p;
            for(p=n;*p&&si<12;) sb[si++]=*p++; for(p=" MB";*p&&si<14;) sb[si++]=*p++; sb[si]='\0';
        } else if (sz >= 1024u) {
            char n[8]; gui_itoa((int)(sz>>10),n,8); int si=0; const char *p;
            for(p=n;*p&&si<12;) sb[si++]=*p++; for(p=" KB";*p&&si<14;) sb[si++]=*p++; sb[si]='\0';
        } else {
            char n[8]; gui_itoa((int)sz,n,8); int si=0; const char *p;
            for(p=n;*p&&si<12;) sb[si++]=*p++; for(p=" B";*p&&si<14;) sb[si++]=*p++; sb[si]='\0';
        }
        uint64_t sl = (uint64_t)gui_strlen(sb);
        uint64_t sx = size_col_x + (size_col_w > sl*fw ? size_col_w - sl*fw : 0u);
        gui_draw_str(sx, ry + (row_h - fh) / 2u, sb, 0x00405060u, row_bg);
    }
    /* restore col separator within this row */
    console_fill_rect(size_col_x - 1u, ry, 1u, row_h,
                      w->fb.col_drag_active ? 0x00253545u : 0x00182030u);
}

/* Redraw only the previously-hovered and newly-hovered rows — O(1) cost vs O(N)
 * for a full fb_render.  Falls back to fb_render for icon view. */
static void fb_render_hover_rows(window_t *w, int old_row, int new_row) {
    if (w->fb.view_mode != FB_VIEW_LIST) { fb_render(w); return; }
    uint64_t lx, ly, lw, lh;
    fb_list_region(w, &lx, &ly, &lw, &lh);
    uint64_t fw       = console_font_width();
    uint64_t fh       = console_font_height();
    uint64_t row_h    = FB_ROW_H;
    uint64_t max_rows = lh / row_h;
    uint64_t icon_col_w = (FB_ICON_COLS + 1u) * fw;
    uint64_t name_col_x = lx + icon_col_w + 4u;
    uint64_t size_col_w = (uint64_t)w->fb.size_col_chars * fw;
    uint64_t size_col_x = lx + lw - size_col_w - 8u;
    bool need_old = (old_row >= 0), need_new = (new_row >= 0);
    if (!need_old && !need_new) return;
    int row_idx = 0, skipped = 0;
    for (int i = 0; i < w->fb.entry_count && row_idx < (int)max_rows; i++) {
        if (!fb_name_matches(w->fb.entries[i], w->fb.search_query)) continue;
        if (skipped < w->fb.scroll) { skipped++; continue; }
        if ((need_old && i == old_row) || (need_new && i == new_row)) {
            uint64_t ry = ly + (uint64_t)row_idx * row_h;
            fb_draw_list_row(w, i, row_idx, lx, lw, ry, row_h, fh, fw,
                             name_col_x, size_col_x, size_col_w);
            if (need_old && i == old_row) need_old = false;
            if (need_new && i == new_row) need_new = false;
            if (!need_old && !need_new) break;
        }
        row_idx++;
    }
}

void fb_on_motion(window_t *w, int32_t mx, int32_t my) {
    bool toolbar_changed = false;
    bool body_changed    = false;
    bool hover_row_only  = false;
    int  old_hover       = w->fb.hover_row;

    int new_hover = fb_hit_row(w, mx, my);
    if (new_hover != old_hover) { w->fb.hover_row = new_hover; body_changed = true; hover_row_only = true; }

    /* Path bar hover: track which char in path string the mouse is over */
    {
        int new_phov = -1;
        if (fb_hit_toolbar(w, mx, my) == 4) {
            uint64_t fw2 = console_font_width();
            uint64_t ix2, iy2, iw2, ih2;
            fb_inner(w, &ix2, &iy2, &iw2, &ih2);
            (void)iy2; (void)iw2; (void)ih2;
            /* pb_x: after back+fwd+up+refresh+view buttons */
            uint64_t pb_x = ix2 + 4u + FB_BTN_W + 2u + FB_BTN_W + 4u + FB_BTN_W + 4u + FB_BTN_W + 4u + FB_BTN_W + 6u;
            int64_t  cp   = ((int64_t)mx - (int64_t)(pb_x + fw2)) / (int64_t)fw2;
            if (cp < 0) cp = 0;
            int plen = (int)gui_strlen(w->fb.path);
            new_phov = (cp < plen) ? (int)cp : (plen > 0 ? plen - 1 : -1);
        }
        if (new_phov != w->fb.path_hov_char) { w->fb.path_hov_char = new_phov; toolbar_changed = true; }
    }

    /* Header hover */
    int new_hh = fb_hit_header(w, mx, my);
    if (new_hh != w->fb.header_hover) { w->fb.header_hover = new_hh; body_changed = true; hover_row_only = false; }

    /* Toolbar button hover */
    {
        int _tbh = fb_hit_toolbar(w, mx, my);
        int new_tbh = (_tbh == 0) ? 0 : (_tbh == 5) ? 1 : (_tbh == 1) ? 2 : (_tbh == 2) ? 3 : (_tbh == 6) ? 4 : -1;
        if (new_tbh != w->fb.toolbar_hover) { w->fb.toolbar_hover = new_tbh; toolbar_changed = true; }
    }

    /* Column separator drag */
    if (w->fb.col_drag_active) {
        uint64_t fw2 = console_font_width();
        if (fw2 < 1) fw2 = 1;
        int dx = mx - w->fb.col_drag_start_x;
        int new_chars = w->fb.col_drag_start_chars - (int)(dx / (int)fw2);
        if (new_chars < 4)  new_chars = 4;
        if (new_chars > 16) new_chars = 16;
        if (new_chars != w->fb.size_col_chars) { w->fb.size_col_chars = new_chars; body_changed = true; hover_row_only = false; }
    }

    if (body_changed) {
        if (hover_row_only && w->fb.view_mode == FB_VIEW_LIST) {
            /* Fast path: only the hovered row changed — repaint just old + new rows.
             * ~50x cheaper than a full fb_render on a 2560-wide window. */
            fb_render_hover_rows(w, old_hover, new_hover);
            if (toolbar_changed) fb_render_toolbar(w);
        } else {
            fb_render(w);
        }
    } else if (toolbar_changed) fb_render_toolbar(w);
}


/* Navigate to the path segment the user clicked in the path bar */
static void fb_click_pathbar(window_t *w, int32_t mx) {
    uint64_t fw = console_font_width();
    uint64_t ix, iy, iw, ih;
    fb_inner(w, &ix, &iy, &iw, &ih);
    (void)iy; (void)iw; (void)ih;

    uint64_t bb_x = ix + 4u;
    uint64_t fwd_x = bb_x + FB_BTN_W + 2u;
    uint64_t ub_x = fwd_x + FB_BTN_W + 4u;
    uint64_t rb_x = ub_x + FB_BTN_W + 4u;
    uint64_t vb_x = rb_x + FB_BTN_W + 4u;
    uint64_t pb_x = vb_x + FB_BTN_W + 6u;
    (void)fwd_x;

    /* path text starts one fw to the right of the path bar left edge */
    int64_t text_x  = (int64_t)(pb_x + fw);
    int64_t char_pos = ((int64_t)mx - text_x) / (int64_t)fw;
    if (char_pos < 0) char_pos = 0;

    const char *path = w->fb.path;
    int plen = (int)gui_strlen(path);
    if (char_pos >= (int64_t)plen) return;

    int cp = (int)char_pos;
    char target[128];
    int ti = 0;

    if (path[cp] == '/') {
        /* Clicked on a '/' separator: navigate to everything before it */
        if (cp == 0) {
            fb_navigate(&w->fb, "/");
            return;
        }
        for (; ti < cp && ti < 127; ti++) target[ti] = path[ti];
    } else {
        /* Clicked on a segment: navigate to path up to end of this segment */
        int seg_end = cp;
        while (seg_end < plen && path[seg_end] != '/') seg_end++;
        for (; ti < seg_end && ti < 127; ti++) target[ti] = path[ti];
    }

    target[ti] = '\0';
    if (ti == 0) { target[0] = '/'; target[1] = '\0'; }

    if (!gui_streq(w->fb.path, target))
        fb_navigate(&w->fb, target);
}

void fb_on_click(window_t *w, int32_t mx, int32_t my) {
    static const char *sb_paths[] = {"/", "/bin", "/etc", "/dev", "/usr", "/tmp", "/home", NULL};

    /* Toolbar buttons */
    int tb = fb_hit_toolbar(w, mx, my);
    if (tb == 0) { fb_back(&w->fb); fb_render(w); return; }
    if (tb == 5) { fb_forward(&w->fb); fb_render(w); return; }
    if (tb == 1) {
        if (!(w->fb.path[0]=='/'&&w->fb.path[1]=='\0')) {
            char parent[128];
            fb_path_parent(parent, w->fb.path);
            fb_navigate(&w->fb, parent);
            fb_render(w);
        }
        return;
    }
    if (tb == 2) { fb_load(&w->fb, w->fb.path); fb_render(w); return; }
    if (tb == 6) {
        w->fb.view_mode = (w->fb.view_mode == FB_VIEW_LIST) ? FB_VIEW_ICONS : FB_VIEW_LIST;
        w->fb.scroll = 0;
        fb_render(w);
        return;
    }
    if (tb == 4) { fb_click_pathbar(w, mx); fb_render(w); return; }
    if (tb == 3) {
        /* Search bar clicked — toggle search mode */
        w->fb.search_active = !w->fb.search_active;
        if (!w->fb.search_active) {
            w->fb.search_query[0] = '\0';
            w->fb.search_len      = 0;
        }
        w->fb.scroll = 0;
        fb_render(w);
        return;
    }

    /* Sidebar */
    int sb = fb_hit_sidebar(w, mx, my);
    if (sb >= 0 && sb_paths[sb] != NULL) {
        if (!gui_streq(w->fb.path, sb_paths[sb]))
            fb_navigate(&w->fb, sb_paths[sb]);
        fb_render(w);
        return;
    }

    /* Scrollbar click: thumb drag or track jump */
    {
        uint64_t sbx, tly, tlh, thy, thh;
        if (fb_sb_thumb(w, &sbx, &tly, &tlh, &thy, &thh)) {
            if ((uint64_t)mx >= sbx && (uint64_t)mx < sbx + 6u &&
                (uint64_t)my >= tly && (uint64_t)my < tly + tlh) {
                int max_rows = (int)(tlh / FB_ROW_H);
                int max_sc   = w->fb.entry_count - max_rows;
                if (max_sc < 0) max_sc = 0;
                if ((uint64_t)my >= thy && (uint64_t)my < thy + thh) {
                    /* Clicked on thumb: start drag, cancel inertia */
                    w->fb.scroll_vel = 0; w->fb.scroll_acc = 0;
                    g_sb_drag       = true;
                    g_sb_drag_win   = (int)(w - g_wins);
                    g_sb_drag_y0    = my;
                    g_sb_drag_s0    = w->fb.scroll;
                    g_sb_drag_range = tlh > thh ? tlh - thh : 1u;
                    g_sb_drag_max   = max_sc;
                    g_sb_drag_text  = false;
                    g_sb_drag_horiz = false;
                } else {
                    /* Clicked on track: jump to position */
                    int ns = (int)(((uint64_t)my - tly) * (uint64_t)w->fb.entry_count / tlh);
                    if (ns < 0) ns = 0;
                    if (ns > max_sc) ns = max_sc;
                    w->fb.scroll = ns;
                    fb_render(w);
                }
                return;
            }
        }
    }

    /* Column separator drag — must test before header sort */
    if (fb_hit_col_sep(w, mx, my)) {
        w->fb.col_drag_active      = true;
        w->fb.col_drag_start_x     = mx;
        w->fb.col_drag_start_chars = w->fb.size_col_chars < 4 ? 7 : w->fb.size_col_chars;
        return;
    }

    /* Column header: toggle sort column / direction */
    {
        int hdr = fb_hit_header(w, mx, my);
        if (hdr >= 0) {
            if (w->fb.sort_by == hdr) {
                w->fb.sort_rev = !w->fb.sort_rev;
            } else {
                w->fb.sort_by  = hdr;
                w->fb.sort_rev = false;
            }
            fb_load(&w->fb, w->fb.path);
            fb_render(w);
            return;
        }
    }

    /* File list */
    int idx = fb_hit_row(w, mx, my);
    if (idx < 0) return;

    bool ctrl = kbd_ctrl_down();
    bool shift = kbd_shift_down();

    if (ctrl && !shift) {
        /* Ctrl+click: toggle this entry in multi-selection */
        if (idx == w->fb.sel_row) {
            /* clicking sel_row: promote it to multi_sel and clear sel_row */
            w->fb.multi_sel[idx] = !w->fb.multi_sel[idx];
            if (!w->fb.multi_sel[idx]) w->fb.sel_row = -1;
        } else {
            w->fb.multi_sel[idx] = !w->fb.multi_sel[idx];
            if (w->fb.sel_row < 0) w->fb.sel_row = idx;
        }
        w->fb.sel_anchor = idx;
        fb_render(w);
        return;
    } else if (shift && w->fb.sel_anchor >= 0) {
        /* Shift+click: range select from anchor to idx */
        int lo = w->fb.sel_anchor < idx ? w->fb.sel_anchor : idx;
        int hi = w->fb.sel_anchor < idx ? idx : w->fb.sel_anchor;
        for (int _ri = 0; _ri < w->fb.entry_count; _ri++)
            w->fb.multi_sel[_ri] = (_ri >= lo && _ri <= hi);
        w->fb.sel_row = idx;
        fb_render(w);
        return;
    } else {
        /* Plain click: clear multi-selection */
        for (int _ri = 0; _ri < w->fb.entry_count; _ri++) w->fb.multi_sel[_ri] = false;
        w->fb.sel_row   = idx;
        w->fb.sel_anchor = idx;
    }

    if (w->fb.is_dir[idx]) {
        char newpath[256];
        fb_path_join(newpath, w->fb.path, w->fb.entries[idx]);
        fb_navigate(&w->fb, newpath);
    } else {
        /* Double-click to open files; single click just selects */
        static int      fb_dbl_idx  = -1;
        static uint64_t fb_dbl_tick = 0;
        uint64_t now = pit_ticks();
        if (idx == fb_dbl_idx && now - fb_dbl_tick < 30u) {
            const char *name = w->fb.entries[idx];
            char full[256];
            fb_path_join(full, w->fb.path, name);
            if (fb_is_image(name)) {
                __attribute__((weak)) void gui_spawn_app_with_arg(const char *p, const char *a);
                if (gui_spawn_app_with_arg)
                    gui_spawn_app_with_arg("/bin/fifi-imageviewer", full);
            } else if (fb_is_viewable(name)) {
                text_open(&g_wins[3], full);
                win_show(&g_wins[3], 3);
            }
            fb_dbl_idx = -1;
        } else {
            fb_dbl_idx  = idx;
            fb_dbl_tick = now;
        }
    }
    fb_render(w);
}

/* ── Settings window ─────────────────────────────────────────────────── */

#define SET_PAD     12u
#define SET_ROW_H   (console_font_height() < 14u ? 20u : console_font_height() + 8u)
#define SET_SEC_H   (console_font_height() < 14u ? 22u : console_font_height() + 8u)
#define COL_SET_BG      0x000c1018u
#define COL_SET_SEC_BG  0x00141e28u
#define COL_SET_SEC_FG  0x005898e8u
#define COL_SET_KEY_FG  0x0080a0c8u
#define COL_SET_VAL_FG  0x00d0dce8u
#define COL_SET_SEP     0x00182838u
#define COL_SET_HINT    0x00405060u

/* Visibility helpers for settings scroll — cy is int64 here */
#define SVIS     (cy >= (int64_t)iy && cy < (int64_t)(iy + ih))
#define SBOT     (cy >= (int64_t)(iy + ih))
#define SCY      ((uint64_t)cy)           /* cast for draw calls */
#define SADVBOT  do { if (SBOT) goto settings_done; } while(0)

