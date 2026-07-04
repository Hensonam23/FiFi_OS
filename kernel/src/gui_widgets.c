#include "gui_internal.h"

/* ── Context menu width helpers (font-scaled) ───────────────────────── */
/* "Show Desktop"/"File Browser" = 12 chars, "Add to Desktop" = 14, "Select All" = 10 */
uint64_t ctx_w(void)     { uint64_t f = console_font_width(); return f ? 12u*f+24u : 168u; }
uint64_t fb_ctx_w(void)  { uint64_t f = console_font_width(); return f ? 14u*f+24u : 192u; }
uint64_t txt_ctx_w(void) { uint64_t f = console_font_width(); return f ? 10u*f+24u : 144u; }

/* ── Launcher popup ──────────────────────────────────────────────────── */

uint64_t launcher_item_h(void) {
    uint64_t fh = console_font_height();
    return (fh + 4u > LAUNCHER_ITEM_H) ? fh + 4u : LAUNCHER_ITEM_H;
}

uint64_t launcher_eff_w(void) {
    uint64_t fw = console_font_width();
    uint64_t max_len = 0;
    for (int i = 0; i < (int)LAUNCHER_ITEMS; i++) {
        uint64_t l = (uint64_t)gui_strlen(g_launcher_items[i]);
        if (l > max_len) max_len = l;
    }
    uint64_t w = max_len * fw + 44u;   /* left icon-dot gutter + padding */
    return (w > LAUNCHER_W) ? w : LAUNCHER_W;
}

uint64_t launcher_lx(void) { return LOGO_X; }
uint64_t launcher_ly(void) {
    return console_fb_height() - TASKBAR_H - LAUNCHER_ITEMS * launcher_item_h() - 2u;
}

void launcher_draw(void) {
    uint64_t lx  = launcher_lx();
    uint64_t ly  = launcher_ly();
    uint64_t fh  = console_font_height();
    uint64_t lw  = launcher_eff_w();
    uint64_t lih = launcher_item_h();
    uint64_t th  = LAUNCHER_ITEMS * lih;

    /* Panel: vertical gradient with soft outline */
    console_fill_vgrad(lx, ly, lw, th, 0x00161d30u, 0x000d111du);

    for (int i = 0; i < (int)LAUNCHER_ITEMS; i++) {
        uint64_t ry = ly + (uint64_t)i * lih;
        const char *label = g_launcher_items[i];
        bool is_sep   = (label[0] == '-' && label[1] == '-');
        bool is_sleep = (i == (int)LAUNCHER_ITEMS - 3);
        bool is_power = (i >= (int)LAUNCHER_ITEMS - 2); /* Restart, Shutdown */
        bool hov = (!is_sep && g_launcher_hover == i);
        if (is_sep) {
            console_fill_rect(lx + 10u, ry + lih/2u, lw - 20u, 1u, 0x00263248u);
            continue;
        }
        if (hov) {
            /* hover pill: accent gradient, inset 3px */
            console_fill_vgrad(lx + 3u, ry + 1u, lw - 6u, lih - 2u,
                               0x003a6cc8u, 0x002a4f9cu);
        }
        /* icon dot: colored bullet in the left gutter */
        uint32_t dot = is_power ? 0x00e05050u :
                       is_sleep ? 0x005090d0u : g_theme.accent;
        uint64_t dy0 = ry + lih / 2u - 2u;
        console_fill_rect(lx + 12u, dy0,      4u, 4u, dot);
        console_fill_rect(lx + 13u, dy0 - 1u, 2u, 6u, dot);
        console_fill_rect(lx + 11u, dy0 + 1u, 6u, 2u, dot);
        /* left-aligned label */
        uint64_t spx = lx + 26u;
        uint64_t spy = ry + (lih > fh ? (lih - fh) / 2u : 0u);
        uint32_t fg  = hov      ? 0x00f2f7ffu :
                       is_power ? 0x00e87068u :
                       is_sleep ? 0x0070a8e0u : COL_LAUNCH_FG;
        gui_draw_str_fg(spx, spy, label, fg);
    }
    /* Outline + accent top edge */
    console_fill_rect(lx, ly, lw, 1u, 0x003a5688u);
    console_fill_rect(lx, ly + th, lw, 1u, 0x00223048u);
    console_fill_rect(lx, ly, 1u, th + 1u, 0x00223048u);
    console_fill_rect(lx + lw - 1u, ly, 1u, th + 1u, 0x00223048u);
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
