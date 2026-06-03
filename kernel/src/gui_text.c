#include "gui_internal.h"

/* ── Text viewer ─────────────────────────────────────────────────────── */

syn_lang_t detect_lang(const char *path);  /* forward declaration */

void recent_add(const char *path) {
    if (!path || !path[0]) return;
    for (int i = 0; i < g_recent_count; i++) {
        if (gui_streq(g_recent[i], path)) {
            for (int j = i; j < g_recent_count - 1; j++)
                for (int k = 0; k < 128; k++) g_recent[j][k] = g_recent[j+1][k];
            g_recent_count--;
            break;
        }
    }
    if (g_recent_count >= RECENT_MAX) g_recent_count = RECENT_MAX - 1;
    for (int i = g_recent_count; i > 0; i--)
        for (int k = 0; k < 128; k++) g_recent[i][k] = g_recent[i-1][k];
    fb_str_copy(g_recent[0], path, 128);
    g_recent_count++;
}

void text_open(window_t *w, const char *path) {
    fb_str_copy(w->text.path, path, TV_PATH_MAX);
    w->text.scroll      = 0;
    w->text.scroll_vel  = 0;
    w->text.scroll_acc  = 0;
    w->text.data        = NULL;
    w->text.size        = 0;
    w->text.total_lines = 0;
    w->text.srch_active     = false;
    w->text.srch_is_goto    = false;
    w->text.srch_case_fold  = false;
    w->text.srch_len        = 0;
    w->text.srch_buf[0]     = '\0';
    w->text.srch_match_line  = -1;
    w->text.srch_match_col   = 0;
    w->text.srch_total_count = 0;
    w->text.srch_cur_idx     = 0;
    w->text.h_scroll         = 0;
    w->text.max_line_len     = 0;
    w->text.word_wrap        = false;
    w->text.lang             = detect_lang(path);
    w->text.edit_mode        = false;
    w->text.edit_modified    = false;
    if (w->text.edit_buf) { kfree(w->text.edit_buf); w->text.edit_buf = NULL; }
    w->text.edit_size        = 0;
    w->text.edit_cap         = 0;
    w->text.edit_cur         = 0;
    w->text.edit_want_col    = 0;
    w->text.edit_cur_line    = 0;
    w->text.edit_cur_col     = 0;
    w->text.sel_anchor       = -1;
    w->text.sel_end          = -1;
    /* Free any leftover undo/redo ring allocations from a previous edit session */
    for (int _ui = 0; _ui < UNDO_DEPTH; _ui++) {
        if (w->text.undo_ring[_ui].data) { kfree(w->text.undo_ring[_ui].data); w->text.undo_ring[_ui].data = NULL; }
    }
    w->text.undo_head = 0; w->text.undo_count = 0; w->text.undo_in_group = false;
    for (int _ri = 0; _ri < UNDO_DEPTH; _ri++) {
        if (w->text.redo_ring[_ri].data) { kfree(w->text.redo_ring[_ri].data); w->text.redo_ring[_ri].data = NULL; }
    }
    w->text.redo_head = 0; w->text.redo_count = 0;
    w->text.save_as_active  = false;
    w->text.save_as_len     = 0;
    w->text.save_as_buf[0]  = '\0';
    w->text.open_bar_active = false;
    w->text.welcome_hover   = -1;

    vfs_read(path, &w->text.data, &w->text.size);

    if (w->text.data && w->text.size > 0) {
        const char *d = (const char *)w->text.data;
        int cur_len = 0;
        for (uint64_t i = 0; i < w->text.size; i++) {
            if (d[i] == '\r') continue;  /* skip CRLF carriage return */
            if (d[i] == '\n') {
                w->text.total_lines++;
                if (cur_len > w->text.max_line_len) w->text.max_line_len = cur_len;
                cur_len = 0;
            } else {
                cur_len++;
            }
        }
        if (d[w->text.size - 1] != '\n') {
            w->text.total_lines++;
            if (cur_len > w->text.max_line_len) w->text.max_line_len = cur_len;
        }
    }

    /* Build title: "basename (N lines)" */
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
    int ti = 0;
    while (base[ti] && ti < 50) { w->text.title_buf[ti] = base[ti]; ti++; }
    if (w->text.total_lines > 0) {
        const char *lbl = " (";
        for (int k = 0; lbl[k] && ti < 60; k++) w->text.title_buf[ti++] = lbl[k];
        char lnbuf[10];
        gui_itoa(w->text.total_lines, lnbuf, 10);
        for (int k = 0; lnbuf[k] && ti < 61; k++) w->text.title_buf[ti++] = lnbuf[k];
        if (ti < 62) w->text.title_buf[ti++] = ')';
    }
    w->text.title_buf[ti] = '\0';
    w->title = w->text.title_buf;
    recent_add(path);
}

/* ── Text editor helpers ─────────────────────────────────────────────── */

void edit_sel_clear(text_state_t *ts) { ts->sel_anchor = -1; ts->sel_end = -1; }

/* Returns canonical [lo, hi) byte range from sel_anchor/sel_end; lo==hi = no selection */
void edit_sel_range(const text_state_t *ts, int32_t *lo, int32_t *hi) {
    if (ts->sel_anchor < 0) { *lo = *hi = 0; return; }
    if (ts->sel_anchor <= ts->sel_end) { *lo = ts->sel_anchor; *hi = ts->sel_end; }
    else                               { *lo = ts->sel_end;    *hi = ts->sel_anchor; }
}

void edit_copy_to_clip(text_state_t *ts) {
    if (ts->sel_anchor < 0 || !ts->edit_buf) return;
    int32_t lo, hi; edit_sel_range(ts, &lo, &hi);
    uint32_t len = (uint32_t)(hi - lo);
    if (len == 0) return;
    if (g_clipboard) { kfree(g_clipboard); g_clipboard = NULL; }
    g_clipboard = (uint8_t *)kmalloc(len + 1u);
    if (!g_clipboard) return;
    for (uint32_t k = 0; k < len; k++) g_clipboard[k] = ts->edit_buf[lo + k];
    g_clipboard[len] = '\0';
    g_clipboard_len = len;
}

void edit_set_clipboard(const uint8_t *data, uint32_t len) {
    if (g_clipboard) { kfree(g_clipboard); g_clipboard = NULL; g_clipboard_len = 0; }
    if (!data || len == 0) return;
    g_clipboard = (uint8_t *)kmalloc(len + 1u);
    if (!g_clipboard) return;
    for (uint32_t k = 0; k < len; k++) g_clipboard[k] = data[k];
    g_clipboard[len] = '\0';
    g_clipboard_len  = len;
}

/* Update cached edit_cur_line/col by scanning edit_buf up to edit_cur. */
void edit_sync_pos(text_state_t *ts) {
    ts->edit_cur_line = 0;
    ts->edit_cur_col  = 0;
    if (!ts->edit_buf) return;
    for (uint32_t i = 0; i < ts->edit_cur; i++) {
        if (ts->edit_buf[i] == '\n') { ts->edit_cur_line++; ts->edit_cur_col = 0; }
        else ts->edit_cur_col++;
    }
}

/* Recount total_lines and max_line_len from edit_buf or data. */
void edit_recount(window_t *w) {
    text_state_t *ts = &w->text;
    const char *d;
    uint64_t sz;
    if (ts->edit_mode && ts->edit_buf) {
        d  = (const char *)ts->edit_buf;
        sz = (uint64_t)ts->edit_size;
    } else {
        d  = (const char *)ts->data;
        sz = ts->size;
    }
    ts->total_lines  = 0;
    ts->max_line_len = 0;
    int cur_len = 0;
    for (uint64_t i = 0; i < sz; i++) {
        if (d[i] == '\n') {
            ts->total_lines++;
            if (cur_len > ts->max_line_len) ts->max_line_len = cur_len;
            cur_len = 0;
        } else cur_len++;
    }
    if (sz > 0 && d[sz-1] != '\n') {
        ts->total_lines++;
        if (cur_len > ts->max_line_len) ts->max_line_len = cur_len;
    }
    if (ts->total_lines == 0) ts->total_lines = 1;  /* empty file = 1 empty line */
}

/* Map mouse position to edit byte offset.
 * Rows outside the visible area are allowed so edit_scroll_to_cursor() can
 * auto-scroll the view when dragging beyond the top or bottom edge. */
uint32_t text_xy_to_offset(window_t *w, int32_t mx, int32_t my) {
    text_state_t *ts = &w->text;
    if (!ts->edit_buf) return 0;
    uint64_t fiy  = w->y + TITLE_H;
    uint64_t fh   = console_font_height();
    uint64_t fw   = console_font_width();
    uint64_t gtot = ts->total_lines > 0 ? (uint64_t)ts->total_lines : 1u;
    uint64_t gw = 1; { uint64_t t=gtot; while(t>=10){t/=10;gw++;} gw=(gw+2u)*fw; }
    uint64_t tx = w->x + BORDER + gw + 1u;
    /* Allow out-of-bounds y for auto-scroll; use floor division for negative values */
    int64_t rel_y = (int64_t)my - (int64_t)(fiy + PAD);
    int click_row;
    if (rel_y >= 0) click_row = (int)(rel_y / (int64_t)fh);
    else            click_row = (int)((rel_y - (int64_t)fh + 1) / (int64_t)fh);
    int64_t rel_x = (int64_t)mx - (int64_t)(tx + PAD);
    if (rel_x < 0) rel_x = 0;
    int click_col = (int)(rel_x / (int64_t)fw) + ts->h_scroll;
    if (click_col < 0) click_col = 0;
    int target_line = ts->scroll + click_row;
    if (target_line < 0) target_line = 0;
    if (target_line >= ts->total_lines) target_line = ts->total_lines - 1;
    int ln = 0;
    uint32_t bi = 0;
    while (bi < ts->edit_size && ln < target_line) {
        if (ts->edit_buf[bi] == '\n') ln++;
        bi++;
    }
    int cl = 0;
    while (bi < ts->edit_size && ts->edit_buf[bi] != '\n' && cl < click_col) {
        bi++; cl++;
    }
    return bi;
}

/* text_xy_to_offset variant for read-only mode (uses ts->data/ts->size). */
uint32_t text_xy_to_offset_ro(window_t *w, int32_t mx, int32_t my) {
    text_state_t *ts = &w->text;
    const uint8_t *data = (const uint8_t *)ts->data;
    uint32_t dsize = (uint32_t)ts->size;
    if (!data || dsize == 0) return 0;
    uint64_t fiy  = w->y + TITLE_H;
    uint64_t fh   = console_font_height();
    uint64_t fw   = console_font_width();
    uint64_t gtot = ts->total_lines > 0 ? (uint64_t)ts->total_lines : 1u;
    uint64_t gw = 1; { uint64_t t=gtot; while(t>=10){t/=10;gw++;} gw=(gw+2u)*fw; }
    uint64_t tx = w->x + BORDER + gw + 1u;
    int64_t rel_y = (int64_t)my - (int64_t)(fiy + PAD);
    int click_row;
    if (rel_y >= 0) click_row = (int)(rel_y / (int64_t)fh);
    else            click_row = (int)((rel_y - (int64_t)fh + 1) / (int64_t)fh);
    int64_t rel_x = (int64_t)mx - (int64_t)(tx + PAD);
    if (rel_x < 0) rel_x = 0;
    int click_col = (int)(rel_x / (int64_t)fw) + ts->h_scroll;
    if (click_col < 0) click_col = 0;
    int target_line = ts->scroll + click_row;
    if (target_line < 0) target_line = 0;
    if (target_line >= ts->total_lines) target_line = ts->total_lines - 1;
    int ln = 0; uint32_t bi = 0;
    while (bi < dsize && ln < target_line) { if (data[bi] == '\n') ln++; bi++; }
    int cl = 0;
    while (bi < dsize && data[bi] != '\n' && cl < click_col) { bi++; cl++; }
    return bi;
}

/* Scroll the text viewer to ensure the cursor is visible. */
void edit_scroll_to_cursor(window_t *w) {
    text_state_t *ts = &w->text;
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();
    uint64_t ih = w->h - TITLE_H - BORDER;
    uint64_t tv_status_h = fh + 4u;
    uint64_t ih_text = ih > tv_status_h ? ih - tv_status_h : 1u;
    uint64_t max_rows = ih_text > 2u * PAD ? (ih_text - 2u * PAD) / fh : 1u;
    if (max_rows < 1) max_rows = 1;
    /* Vertical */
    if (ts->edit_cur_line < ts->scroll)
        ts->scroll = ts->edit_cur_line;
    else if (ts->edit_cur_line >= ts->scroll + (int)max_rows)
        ts->scroll = ts->edit_cur_line - (int)max_rows + 1;
    /* Horizontal (no-wrap mode only) */
    if (!ts->word_wrap && fw > 0) {
        uint64_t gutter_w = 4u * fw + 6u;  /* 4-digit line numbers */
        uint64_t iw = w->w - 2u * BORDER;
        uint64_t avail_w = iw > gutter_w + 8u ? iw - gutter_w - 8u : 1u;
        int max_cols = (int)(avail_w / fw);
        if (max_cols < 1) max_cols = 1;
        if (ts->edit_cur_col < ts->h_scroll)
            ts->h_scroll = ts->edit_cur_col;
        else if (ts->edit_cur_col >= ts->h_scroll + max_cols)
            ts->h_scroll = ts->edit_cur_col - max_cols + 1;
        if (ts->h_scroll < 0) ts->h_scroll = 0;
    }
}

/* Grow edit_buf by at least extra bytes. Returns false on OOM. */
static bool edit_grow(text_state_t *ts, uint32_t extra) {
    if (ts->edit_size + extra <= ts->edit_cap) return true;
    uint32_t new_cap = ts->edit_cap + extra + 4096u;
    uint8_t *nb = (uint8_t *)kmalloc(new_cap);
    if (!nb) return false;
    for (uint32_t i = 0; i < ts->edit_size; i++) nb[i] = ts->edit_buf[i];
    if (ts->edit_buf) kfree(ts->edit_buf);
    ts->edit_buf = nb;
    ts->edit_cap = new_cap;
    return true;
}

/* Enter edit mode: allocate mutable buffer from current file data. */
void text_enter_edit(window_t *w) {
    text_state_t *ts = &w->text;
    if (ts->edit_mode) return;
    uint32_t cap = (uint32_t)ts->size + 4096u;
    if (cap < 4096u) cap = 4096u;
    uint8_t *buf = (uint8_t *)kmalloc(cap);
    if (!buf) { gui_toast("No memory for edit", 0x00e88060u); return; }
    const uint8_t *src = (const uint8_t *)ts->data;
    uint32_t wpos = 0;
    for (uint32_t i = 0; i < (uint32_t)ts->size; i++) {
        if (src[i] != '\r') buf[wpos++] = src[i];  /* strip CRLF → LF */
    }
    ts->edit_buf      = buf;
    ts->edit_size     = wpos;
    ts->edit_cap      = cap;
    ts->edit_cur      = 0;
    ts->edit_want_col = 0;
    ts->edit_modified = false;
    ts->sel_anchor    = -1;
    ts->sel_end       = -1;
    /* Clear undo ring */
    for (int _ui = 0; _ui < UNDO_DEPTH; _ui++) {
        if (ts->undo_ring[_ui].data) { kfree(ts->undo_ring[_ui].data); ts->undo_ring[_ui].data = NULL; }
    }
    ts->undo_head     = 0;
    ts->undo_count    = 0;
    ts->undo_in_group = false;
    /* Clear redo ring */
    for (int _ri = 0; _ri < UNDO_DEPTH; _ri++) {
        if (ts->redo_ring[_ri].data) { kfree(ts->redo_ring[_ri].data); ts->redo_ring[_ri].data = NULL; }
    }
    ts->redo_head = 0; ts->redo_count = 0;
    ts->edit_mode     = true;
    edit_sync_pos(ts);
    edit_recount(w);
}

/* Exit edit mode and free the mutable buffer. */
void text_exit_edit(window_t *w) {
    text_state_t *ts = &w->text;
    if (!ts->edit_mode) return;
    if (ts->edit_buf) { kfree(ts->edit_buf); ts->edit_buf = NULL; }
    /* Free undo ring */
    for (int _ui = 0; _ui < UNDO_DEPTH; _ui++) {
        if (ts->undo_ring[_ui].data) { kfree(ts->undo_ring[_ui].data); ts->undo_ring[_ui].data = NULL; }
    }
    ts->undo_head = 0; ts->undo_count = 0;
    /* Free redo ring */
    for (int _ri = 0; _ri < UNDO_DEPTH; _ri++) {
        if (ts->redo_ring[_ri].data) { kfree(ts->redo_ring[_ri].data); ts->redo_ring[_ri].data = NULL; }
    }
    ts->redo_head = 0; ts->redo_count = 0;
    ts->edit_mode      = false;
    ts->edit_modified  = false;
    ts->edit_size      = 0;
    ts->edit_cap       = 0;
    ts->edit_cur       = 0;
    ts->sel_anchor     = -1;
    ts->sel_end        = -1;
    ts->save_as_active  = false;
    ts->save_as_len     = 0;
    ts->open_bar_active = false;
}

/* Save edit_buf to the VFS. */
void text_save(window_t *w) {
    text_state_t *ts = &w->text;
    if (!ts->edit_mode || !ts->edit_buf || !ts->path[0]) return;
    vfs_write(ts->path, ts->edit_buf, (uint64_t)ts->edit_size);
    ts->edit_modified = false;
    gui_toast("Saved", 0x0080e8b0u);
}

/* Insert one byte at cursor. */
bool edit_insert(text_state_t *ts, uint8_t c) {
    if (!edit_grow(ts, 1)) return false;
    uint32_t i = ts->edit_size;
    while (i > ts->edit_cur) { ts->edit_buf[i] = ts->edit_buf[i-1]; i--; }
    ts->edit_buf[ts->edit_cur] = c;
    ts->edit_size++;
    ts->edit_cur++;
    ts->edit_modified = true;
    if (c == '\n') { ts->edit_cur_line++; ts->edit_cur_col = 0; }
    else           { ts->edit_cur_col++; }
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
    return true;
}

/* Delete selected region; move cursor to lo. Returns true if anything deleted. */
bool edit_delete_selection(text_state_t *ts) {
    if (ts->sel_anchor < 0 || !ts->edit_buf) return false;
    int32_t lo, hi; edit_sel_range(ts, &lo, &hi);
    uint32_t len = (uint32_t)(hi - lo);
    if (len == 0) { edit_sel_clear(ts); return false; }
    for (uint32_t k = (uint32_t)lo; k < ts->edit_size - len; k++)
        ts->edit_buf[k] = ts->edit_buf[k + len];
    ts->edit_size -= len;
    if (ts->edit_size < ts->edit_cap) ts->edit_buf[ts->edit_size] = '\0';
    ts->edit_cur = (uint32_t)lo;
    ts->edit_modified = true;
    edit_sel_clear(ts);
    edit_sync_pos(ts);
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
    return true;
}

/* Paste clipboard at current cursor (replaces selection if any). */
void edit_paste(window_t *w) {
    if (!g_clipboard || g_clipboard_len == 0) return;
    text_state_t *ts = &w->text;
    if (!ts->edit_buf) return;
    edit_delete_selection(ts);
    if (!edit_grow(ts, g_clipboard_len)) return;
    uint32_t c = ts->edit_cur;
    uint32_t len = g_clipboard_len;
    /* Shift tail right */
    for (uint32_t k = ts->edit_size; k > c; k--)
        ts->edit_buf[k - 1u + len] = ts->edit_buf[k - 1u];
    /* Insert */
    for (uint32_t k = 0; k < len; k++) ts->edit_buf[c + k] = g_clipboard[k];
    ts->edit_size += len;
    if (ts->edit_size < ts->edit_cap) ts->edit_buf[ts->edit_size] = '\0';
    ts->edit_cur += len;
    ts->edit_modified = true;
    edit_sync_pos(ts);
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
}

/* Push current buffer state onto the undo ring before a destructive edit. */
void edit_push_undo(text_state_t *ts) {
    if (!ts->edit_buf) return;
    int slot = ts->undo_head % UNDO_DEPTH;
    if (ts->undo_ring[slot].data) { kfree(ts->undo_ring[slot].data); ts->undo_ring[slot].data = NULL; }
    ts->undo_ring[slot].data = (uint8_t *)kmalloc(ts->edit_size + 1u);
    if (!ts->undo_ring[slot].data) return;
    for (uint32_t i = 0; i < ts->edit_size; i++) ts->undo_ring[slot].data[i] = ts->edit_buf[i];
    ts->undo_ring[slot].data[ts->edit_size] = '\0';
    ts->undo_ring[slot].size   = ts->edit_size;
    ts->undo_ring[slot].cursor = ts->edit_cur;
    ts->undo_head = (ts->undo_head + 1) % UNDO_DEPTH;
    if (ts->undo_count < UNDO_DEPTH) ts->undo_count++;
    /* New edit invalidates redo history */
    for (int _ri = 0; _ri < UNDO_DEPTH; _ri++) {
        if (ts->redo_ring[_ri].data) { kfree(ts->redo_ring[_ri].data); ts->redo_ring[_ri].data = NULL; }
    }
    ts->redo_head = 0; ts->redo_count = 0;
}

/* Restore from the top of the undo ring (Ctrl+Z). */
void edit_pop_undo(window_t *w) {
    text_state_t *ts = &w->text;
    if (ts->undo_count == 0 || !ts->edit_buf) return;
    /* Save current state to redo ring before overwriting */
    {
        int rslot = ts->redo_head;
        if (ts->redo_ring[rslot].data) { kfree(ts->redo_ring[rslot].data); ts->redo_ring[rslot].data = NULL; }
        ts->redo_ring[rslot].data = (uint8_t *)kmalloc(ts->edit_size + 1u);
        if (ts->redo_ring[rslot].data) {
            for (uint32_t _i = 0; _i < ts->edit_size; _i++) ts->redo_ring[rslot].data[_i] = ts->edit_buf[_i];
            ts->redo_ring[rslot].data[ts->edit_size] = '\0';
            ts->redo_ring[rslot].size   = ts->edit_size;
            ts->redo_ring[rslot].cursor = ts->edit_cur;
            ts->redo_head = (ts->redo_head + 1) % UNDO_DEPTH;
            if (ts->redo_count < UNDO_DEPTH) ts->redo_count++;
        }
    }
    ts->undo_head = (ts->undo_head - 1 + UNDO_DEPTH) % UNDO_DEPTH;
    ts->undo_count--;
    int slot = ts->undo_head;
    if (!ts->undo_ring[slot].data) return;
    uint32_t sz = ts->undo_ring[slot].size;
    if (sz >= ts->edit_cap) {
        uint8_t *nb = (uint8_t *)kmalloc(sz + 4096u);
        if (!nb) { ts->undo_head = (ts->undo_head + 1) % UNDO_DEPTH; ts->undo_count++; return; }
        kfree(ts->edit_buf); ts->edit_buf = nb; ts->edit_cap = sz + 4096u;
    }
    for (uint32_t i = 0; i < sz; i++) ts->edit_buf[i] = ts->undo_ring[slot].data[i];
    ts->edit_size = sz;
    if (ts->edit_size < ts->edit_cap) ts->edit_buf[ts->edit_size] = '\0';
    ts->edit_cur = ts->undo_ring[slot].cursor;
    kfree(ts->undo_ring[slot].data); ts->undo_ring[slot].data = NULL;
    edit_sel_clear(ts);
    edit_sync_pos(ts);
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
    ts->edit_modified = true;
}

/* Restore from the redo ring (Ctrl+Y). Pushes to undo without clearing redo. */
void edit_pop_redo(window_t *w) {
    text_state_t *ts = &w->text;
    if (ts->redo_count == 0 || !ts->edit_buf) return;
    /* Push current state to undo ring directly (not via edit_push_undo — that would clear redo) */
    {
        int uslot = ts->undo_head;
        if (ts->undo_ring[uslot].data) { kfree(ts->undo_ring[uslot].data); ts->undo_ring[uslot].data = NULL; }
        ts->undo_ring[uslot].data = (uint8_t *)kmalloc(ts->edit_size + 1u);
        if (ts->undo_ring[uslot].data) {
            for (uint32_t _i = 0; _i < ts->edit_size; _i++) ts->undo_ring[uslot].data[_i] = ts->edit_buf[_i];
            ts->undo_ring[uslot].data[ts->edit_size] = '\0';
            ts->undo_ring[uslot].size   = ts->edit_size;
            ts->undo_ring[uslot].cursor = ts->edit_cur;
            ts->undo_head = (ts->undo_head + 1) % UNDO_DEPTH;
            if (ts->undo_count < UNDO_DEPTH) ts->undo_count++;
        }
    }
    /* Restore from redo top */
    ts->redo_head = (ts->redo_head - 1 + UNDO_DEPTH) % UNDO_DEPTH;
    ts->redo_count--;
    int rslot = ts->redo_head;
    if (!ts->redo_ring[rslot].data) return;
    uint32_t sz = ts->redo_ring[rslot].size;
    if (sz >= ts->edit_cap) {
        uint8_t *nb = (uint8_t *)kmalloc(sz + 4096u);
        if (!nb) { ts->redo_head = (ts->redo_head + 1) % UNDO_DEPTH; ts->redo_count++; return; }
        kfree(ts->edit_buf); ts->edit_buf = nb; ts->edit_cap = sz + 4096u;
    }
    for (uint32_t i = 0; i < sz; i++) ts->edit_buf[i] = ts->redo_ring[rslot].data[i];
    ts->edit_size = sz;
    if (ts->edit_size < ts->edit_cap) ts->edit_buf[ts->edit_size] = '\0';
    ts->edit_cur = ts->redo_ring[rslot].cursor;
    kfree(ts->redo_ring[rslot].data); ts->redo_ring[rslot].data = NULL;
    edit_sel_clear(ts);
    edit_sync_pos(ts);
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
    ts->edit_modified = true;
}

/* Delete byte before cursor (Backspace). */
void edit_del_before(text_state_t *ts) {
    if (ts->edit_cur == 0) return;
    ts->edit_cur--;
    uint8_t removed = ts->edit_buf[ts->edit_cur];
    for (uint32_t i = ts->edit_cur; i < ts->edit_size - 1u; i++)
        ts->edit_buf[i] = ts->edit_buf[i+1];
    ts->edit_size--;
    ts->edit_modified = true;
    /* Sync position (simple: just re-scan) */
    if (removed == '\n') { ts->edit_cur_line--; edit_sync_pos(ts); }
    else                 { ts->edit_cur_col--; }
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
}

/* Delete byte at cursor (Delete key). */
void edit_del_at(text_state_t *ts) {
    if (ts->edit_cur >= ts->edit_size) return;
    for (uint32_t i = ts->edit_cur; i < ts->edit_size - 1u; i++)
        ts->edit_buf[i] = ts->edit_buf[i+1];
    ts->edit_size--;
    ts->edit_modified = true;
    /* Cursor position doesn't change */
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
}

/* Delete the word immediately before the cursor (Ctrl+Backspace). */
void edit_del_word_before(text_state_t *ts) {
    if (!ts->edit_buf || ts->edit_cur == 0) return;
    uint32_t end = ts->edit_cur;
    while (ts->edit_cur > 0) {
        uint8_t c = ts->edit_buf[ts->edit_cur - 1u];
        if (c != ' ' && c != '\t' && c != '\n') break;
        ts->edit_cur--;
    }
    while (ts->edit_cur > 0) {
        uint8_t c = ts->edit_buf[ts->edit_cur - 1u];
        if (c == ' ' || c == '\t' || c == '\n') break;
        ts->edit_cur--;
    }
    uint32_t del = end - ts->edit_cur;
    if (del == 0) return;
    for (uint32_t k = ts->edit_cur; k < ts->edit_size - del; k++) ts->edit_buf[k] = ts->edit_buf[k + del];
    ts->edit_size -= del;
    if (ts->edit_size < ts->edit_cap) ts->edit_buf[ts->edit_size] = '\0';
    ts->edit_modified = true;
    edit_sync_pos(ts);
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
}

/* Delete the word immediately after the cursor (Ctrl+Delete). */
void edit_del_word_at(text_state_t *ts) {
    if (!ts->edit_buf || ts->edit_cur >= ts->edit_size) return;
    uint32_t start = ts->edit_cur;
    while (ts->edit_cur < ts->edit_size) {
        uint8_t c = ts->edit_buf[ts->edit_cur];
        if (c == ' ' || c == '\t' || c == '\n') break;
        ts->edit_cur++;
    }
    while (ts->edit_cur < ts->edit_size) {
        uint8_t c = ts->edit_buf[ts->edit_cur];
        if (c != ' ' && c != '\t' && c != '\n') break;
        ts->edit_cur++;
    }
    uint32_t del = ts->edit_cur - start;
    ts->edit_cur = start;
    if (del == 0) return;
    for (uint32_t k = start; k < ts->edit_size - del; k++) ts->edit_buf[k] = ts->edit_buf[k + del];
    ts->edit_size -= del;
    if (ts->edit_size < ts->edit_cap) ts->edit_buf[ts->edit_size] = '\0';
    ts->edit_modified = true;
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
}

/* Move cursor left one byte. */
void edit_move_left(text_state_t *ts) {
    if (ts->edit_cur == 0) return;
    ts->edit_cur--;
    if (ts->edit_buf[ts->edit_cur] == '\n') { ts->edit_cur_line--; edit_sync_pos(ts); }
    else ts->edit_cur_col--;
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
}

/* Move cursor right one byte. */
void edit_move_right(text_state_t *ts) {
    if (ts->edit_cur >= ts->edit_size) return;
    uint8_t c = ts->edit_buf[ts->edit_cur];
    ts->edit_cur++;
    if (c == '\n') { ts->edit_cur_line++; ts->edit_cur_col = 0; }
    else           ts->edit_cur_col++;
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
}

/* Move cursor left by one word (Ctrl+Left). Stops at identifier boundaries. */
void edit_move_word_left(text_state_t *ts) {
    if (!ts->edit_buf || ts->edit_cur == 0) return;
#define _IS_ID(c) (((c)>='a'&&(c)<='z')||((c)>='A'&&(c)<='Z')||((c)>='0'&&(c)<='9')||(c)=='_')
    /* Skip trailing whitespace */
    while (ts->edit_cur > 0) {
        uint8_t c = ts->edit_buf[ts->edit_cur - 1u];
        if (c != ' ' && c != '\t' && c != '\n') break;
        ts->edit_cur--;
    }
    if (ts->edit_cur == 0) goto _wl_done;
    if (_IS_ID(ts->edit_buf[ts->edit_cur - 1u])) {
        while (ts->edit_cur > 0 && _IS_ID(ts->edit_buf[ts->edit_cur - 1u])) ts->edit_cur--;
    } else {
        /* One punctuation char */
        ts->edit_cur--;
    }
_wl_done:
    edit_sync_pos(ts);
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
}

/* Move cursor right by one word (Ctrl+Right). Stops at identifier boundaries. */
void edit_move_word_right(text_state_t *ts) {
    if (!ts->edit_buf || ts->edit_cur >= ts->edit_size) return;
    uint8_t c0 = ts->edit_buf[ts->edit_cur];
    if (c0 == ' ' || c0 == '\t' || c0 == '\n') {
        while (ts->edit_cur < ts->edit_size) {
            uint8_t c = ts->edit_buf[ts->edit_cur];
            if (c != ' ' && c != '\t' && c != '\n') break;
            ts->edit_cur++;
        }
    } else if (_IS_ID(c0)) {
        while (ts->edit_cur < ts->edit_size && _IS_ID(ts->edit_buf[ts->edit_cur])) ts->edit_cur++;
    } else {
        ts->edit_cur++;
    }
#undef _IS_ID
    edit_sync_pos(ts);
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
}

/* Indent (indent=true) or unindent (indent=false) every line touched by the
 * current selection.  If no selection, operates on the current line only. */
void edit_indent_block(text_state_t *ts, bool indent) {
    if (!ts->edit_buf) return;
    int32_t lo, hi;
    if (ts->sel_anchor >= 0) {
        edit_sel_range(ts, &lo, &hi);
    } else {
        lo = hi = (int32_t)ts->edit_cur;
    }
    /* Find start of first selected line */
    uint32_t p = (uint32_t)lo;
    while (p > 0 && ts->edit_buf[p - 1u] != '\n') p--;
    while ((int32_t)p <= hi && p < ts->edit_size) {
        if (indent) {
            if (!edit_grow(ts, 4u)) break;
            /* Shift buffer right by 4 at p */
            for (uint32_t k = ts->edit_size + 3u; k >= p + 4u; k--)
                ts->edit_buf[k] = ts->edit_buf[k - 4u];
            for (int ki = 0; ki < 4; ki++) ts->edit_buf[p + (uint32_t)ki] = ' ';
            ts->edit_size += 4u;
            if (ts->edit_size < ts->edit_cap) ts->edit_buf[ts->edit_size] = '\0';
            if (ts->edit_cur >= p) ts->edit_cur += 4u;
            if (ts->sel_anchor >= (int32_t)p) ts->sel_anchor += 4;
            if (ts->sel_end   >= (int32_t)p) ts->sel_end   += 4;
            hi += 4; p += 4u;
        } else {
            /* Count leading spaces to remove (up to 4) */
            uint32_t rem = 0;
            while (rem < 4u && p + rem < ts->edit_size && ts->edit_buf[p + rem] == ' ') rem++;
            if (rem > 0u) {
                for (uint32_t k = p; k < ts->edit_size - rem; k++) ts->edit_buf[k] = ts->edit_buf[k + rem];
                ts->edit_size -= rem;
                if (ts->edit_size < ts->edit_cap) ts->edit_buf[ts->edit_size] = '\0';
                if (ts->edit_cur >= p + rem) ts->edit_cur -= rem;
                else if (ts->edit_cur > p) ts->edit_cur = p;
                if (ts->sel_anchor >= (int32_t)(p + rem)) ts->sel_anchor -= (int32_t)rem;
                else if (ts->sel_anchor > (int32_t)p)     ts->sel_anchor = (int32_t)p;
                if (ts->sel_end >= (int32_t)(p + rem)) ts->sel_end -= (int32_t)rem;
                else if (ts->sel_end > (int32_t)p)     ts->sel_end = (int32_t)p;
                hi -= (int32_t)rem;
            }
        }
        /* Advance to next line */
        while (p < ts->edit_size && ts->edit_buf[p] != '\n') p++;
        if (p < ts->edit_size) p++;
    }
    ts->edit_modified = true;
    edit_sync_pos(ts);
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
}

/* Toggle line comment on current line or selected lines (Ctrl+/). */
void edit_toggle_comment(window_t *w) {
    text_state_t *ts = &w->text;
    if (!ts->edit_buf) return;
    const char *pfx = (ts->lang == SYN_LANG_SH  || ts->lang == SYN_LANG_PY  || ts->lang == SYN_LANG_ASM
                    || ts->lang == SYN_LANG_YAML || ts->lang == SYN_LANG_TOML|| ts->lang == SYN_LANG_INI
                    || ts->lang == SYN_LANG_MAKE) ? "# "
                    : (ts->lang == SYN_LANG_LUA || ts->lang == SYN_LANG_SQL) ? "-- "
                    : "// ";
    uint32_t plen = (ts->lang == SYN_LANG_SH  || ts->lang == SYN_LANG_PY  || ts->lang == SYN_LANG_ASM
                  || ts->lang == SYN_LANG_YAML || ts->lang == SYN_LANG_TOML || ts->lang == SYN_LANG_INI
                  || ts->lang == SYN_LANG_MAKE) ? 2u
                  : (ts->lang == SYN_LANG_LUA || ts->lang == SYN_LANG_SQL) ? 3u
                  : 3u;

    int32_t lo, hi;
    if (ts->sel_anchor >= 0) { edit_sel_range(ts, &lo, &hi); }
    else { lo = hi = (int32_t)ts->edit_cur; }

    /* Start of first selected line */
    uint32_t p = (uint32_t)lo;
    while (p > 0 && ts->edit_buf[p - 1u] != '\n') p--;

    /* Determine mode: are all non-empty lines already commented? */
    bool removing = true;
    uint32_t sp = p;
    while ((int32_t)sp <= hi && sp < ts->edit_size) {
        uint32_t wp = sp;
        while (wp < ts->edit_size && (ts->edit_buf[wp] == ' ' || ts->edit_buf[wp] == '\t')) wp++;
        if (wp < ts->edit_size && ts->edit_buf[wp] != '\n') {
            bool has = true;
            for (uint32_t k = 0; k < plen; k++) {
                if (wp + k >= ts->edit_size || ts->edit_buf[wp + k] != (uint8_t)pfx[k]) { has = false; break; }
            }
            if (!has) { removing = false; break; }
        }
        while (sp < ts->edit_size && ts->edit_buf[sp] != '\n') sp++;
        if (sp < ts->edit_size) sp++;
    }

    /* Apply to each line */
    while ((int32_t)p <= hi && p <= ts->edit_size) {
        uint32_t wp = p;
        while (wp < ts->edit_size && (ts->edit_buf[wp] == ' ' || ts->edit_buf[wp] == '\t')) wp++;
        bool nonempty = (wp < ts->edit_size && ts->edit_buf[wp] != '\n');
        if (removing && nonempty) {
            bool has = true;
            for (uint32_t k = 0; k < plen; k++) {
                if (wp + k >= ts->edit_size || ts->edit_buf[wp + k] != (uint8_t)pfx[k]) { has = false; break; }
            }
            if (has) {
                for (uint32_t k = wp; k < ts->edit_size - plen; k++) ts->edit_buf[k] = ts->edit_buf[k + plen];
                ts->edit_size -= plen;
                if (ts->edit_size < ts->edit_cap) ts->edit_buf[ts->edit_size] = '\0';
                if (ts->edit_cur >= wp + plen) ts->edit_cur -= plen;
                else if (ts->edit_cur > wp)    ts->edit_cur = wp;
                if (ts->sel_anchor >= (int32_t)(wp + plen)) ts->sel_anchor -= (int32_t)plen;
                else if (ts->sel_anchor > (int32_t)wp)      ts->sel_anchor  = (int32_t)wp;
                if (ts->sel_end >= (int32_t)(wp + plen)) ts->sel_end -= (int32_t)plen;
                else if (ts->sel_end > (int32_t)wp)      ts->sel_end  = (int32_t)wp;
                hi -= (int32_t)plen;
            }
        } else if (!removing && nonempty && edit_grow(ts, plen)) {
            for (uint32_t k = ts->edit_size + plen - 1u; k >= wp + plen; k--)
                ts->edit_buf[k] = ts->edit_buf[k - plen];
            for (uint32_t k = 0; k < plen; k++) ts->edit_buf[wp + k] = (uint8_t)pfx[k];
            ts->edit_size += plen;
            if (ts->edit_size < ts->edit_cap) ts->edit_buf[ts->edit_size] = '\0';
            if (ts->edit_cur >= wp) ts->edit_cur += plen;
            if (ts->sel_anchor >= (int32_t)wp) ts->sel_anchor += (int32_t)plen;
            if (ts->sel_end    >= (int32_t)wp) ts->sel_end    += (int32_t)plen;
            if ((int32_t)wp <= hi) hi += (int32_t)plen;
        }
        while (p < ts->edit_size && ts->edit_buf[p] != '\n') p++;
        if (p < ts->edit_size) p++;
    }
    ts->edit_modified = true;
    edit_sync_pos(ts);
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
}

void text_search_next(window_t *w, bool from_next); /* forward decl */

/* Replace current search match with repl_buf, then find next. */
void text_replace_one(window_t *w) {
    text_state_t *ts = &w->text;
    if (!ts->edit_buf || ts->srch_len == 0) { text_search_next(w, false); return; }
    if (ts->sel_anchor < 0) { text_search_next(w, false); return; }
    int32_t lo, hi; edit_sel_range(ts, &lo, &hi);
    if (hi - lo != ts->srch_len) { text_search_next(w, false); return; }
    edit_push_undo(ts);
    /* Delete match */
    for (uint32_t k = (uint32_t)lo; k < ts->edit_size - (uint32_t)(hi - lo); k++)
        ts->edit_buf[k] = ts->edit_buf[k + (uint32_t)(hi - lo)];
    ts->edit_size -= (uint32_t)(hi - lo);
    ts->edit_cur = (uint32_t)lo;
    if (ts->edit_size < ts->edit_cap) ts->edit_buf[ts->edit_size] = '\0';
    edit_sel_clear(ts);
    /* Insert replacement */
    if (ts->repl_len > 0 && edit_grow(ts, (uint32_t)ts->repl_len)) {
        for (uint32_t k = ts->edit_size + (uint32_t)ts->repl_len - 1u;
                      k >= ts->edit_cur + (uint32_t)ts->repl_len; k--)
            ts->edit_buf[k] = ts->edit_buf[k - (uint32_t)ts->repl_len];
        for (int k = 0; k < ts->repl_len; k++)
            ts->edit_buf[ts->edit_cur + (uint32_t)k] = (uint8_t)ts->repl_buf[k];
        ts->edit_size += (uint32_t)ts->repl_len;
        ts->edit_cur  += (uint32_t)ts->repl_len;
        if (ts->edit_size < ts->edit_cap) ts->edit_buf[ts->edit_size] = '\0';
    }
    ts->edit_modified = true;
    edit_sync_pos(ts);
    /* Set srch_match position to just before edit_cur so from_next starts there */
    ts->srch_match_line = ts->edit_cur_line;
    ts->srch_match_col  = ts->edit_cur_col > 0 ? ts->edit_cur_col - 1 : 0;
    text_search_next(w, true);
}

static inline bool srch_ceq(unsigned char a, unsigned char b, bool fold) {
    if (a == b) return true;
    if (!fold) return false;
    if (a >= 'A' && a <= 'Z') a |= 32;
    if (b >= 'A' && b <= 'Z') b |= 32;
    return a == b;
}

/* Replace all occurrences of srch_buf with repl_buf in the edit buffer. */
int text_replace_all_impl(window_t *w) {
    text_state_t *ts = &w->text;
    if (!ts->edit_buf || ts->srch_len == 0) return 0;
    edit_push_undo(ts);
    int replaced = 0;
    uint32_t pos = 0;
    uint32_t qlen = (uint32_t)ts->srch_len;
    uint32_t rlen = (uint32_t)ts->repl_len;
    bool fold3 = ts->srch_case_fold;
    while (pos + qlen <= ts->edit_size && replaced < 5000) {
        bool hit = true;
        for (uint32_t k = 0; k < qlen; k++) {
            if (!srch_ceq(ts->edit_buf[pos + k], (unsigned char)ts->srch_buf[k], fold3))
                { hit = false; break; }
        }
        if (hit) {
            /* Delete qlen at pos */
            for (uint32_t k = pos; k + qlen < ts->edit_size; k++) ts->edit_buf[k] = ts->edit_buf[k + qlen];
            ts->edit_size -= qlen;
            if (ts->edit_size < ts->edit_cap) ts->edit_buf[ts->edit_size] = '\0';
            /* Insert rlen at pos */
            if (rlen > 0 && edit_grow(ts, rlen)) {
                for (uint32_t k = ts->edit_size + rlen - 1u; k >= pos + rlen; k--)
                    ts->edit_buf[k] = ts->edit_buf[k - rlen];
                for (uint32_t k = 0; k < rlen; k++) ts->edit_buf[pos + k] = (uint8_t)ts->repl_buf[k];
                ts->edit_size += rlen;
                if (ts->edit_size < ts->edit_cap) ts->edit_buf[ts->edit_size] = '\0';
            }
            pos += rlen;
            replaced++;
        } else {
            pos++;
        }
    }
    if (replaced > 0) ts->edit_modified = true;
    edit_sel_clear(ts);
    ts->srch_match_line = -1;
    edit_sync_pos(ts);
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
    return replaced;
}

/* Duplicate the current line, placing the cursor at the start of the copy. */
void edit_dup_line(window_t *w) {
    text_state_t *ts = &w->text;
    if (!ts->edit_buf) return;
    edit_sel_clear(ts);
    uint32_t ls = ts->edit_cur;
    while (ls > 0 && ts->edit_buf[ls - 1u] != '\n') ls--;
    uint32_t le = ts->edit_cur;
    while (le < ts->edit_size && ts->edit_buf[le] != '\n') le++;
    bool has_nl = (le < ts->edit_size);
    if (has_nl) le++;  /* include trailing '\n' */
    uint32_t line_len = le - ls;
    uint32_t dup_start = le;
    ts->edit_cur = le;
    if (!has_nl) edit_insert(ts, '\n');  /* add separator before copy on last line */
    for (uint32_t k = 0; k < line_len; k++) edit_insert(ts, ts->edit_buf[ls + k]);
    /* Position cursor at start of duplicated line */
    ts->edit_cur = dup_start + (has_nl ? 0u : 1u);
    ts->edit_modified = true;
    edit_sync_pos(ts);
    ts->edit_want_col = 0;
}

/* Kill from cursor to end of current line (Ctrl+K).
 * If cursor is already at end of line, removes the newline to join lines. */
void edit_kill_line(text_state_t *ts) {
    if (!ts->edit_buf || ts->edit_cur >= ts->edit_size) return;
    uint32_t c = ts->edit_cur;
    if (ts->edit_buf[c] == '\n') {
        /* Join with next line by deleting the newline */
        for (uint32_t k = c; k < ts->edit_size - 1u; k++) ts->edit_buf[k] = ts->edit_buf[k + 1u];
        ts->edit_size--;
    } else {
        /* Delete from cursor to end of line, leaving the '\n' in place */
        uint32_t end = c;
        while (end < ts->edit_size && ts->edit_buf[end] != '\n') end++;
        uint32_t del = end - c;
        for (uint32_t k = c; k < ts->edit_size - del; k++) ts->edit_buf[k] = ts->edit_buf[k + del];
        ts->edit_size -= del;
    }
    if (ts->edit_size < ts->edit_cap) ts->edit_buf[ts->edit_size] = '\0';
    ts->edit_modified = true;
}

/* Move cursor to end of current line (before \n or end-of-file). */
void edit_move_end(text_state_t *ts) {
    while (ts->edit_cur < ts->edit_size && ts->edit_buf[ts->edit_cur] != '\n')
        ts->edit_cur++;
    edit_sync_pos(ts);
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
}

/* Move cursor up one line. */
void edit_move_up(text_state_t *ts) {
    if (ts->edit_cur_line == 0) return;
    /* Find start of current line */
    uint32_t ls = ts->edit_cur;
    while (ls > 0 && ts->edit_buf[ls-1] != '\n') ls--;
    /* Step back into previous line */
    if (ls == 0) return;
    ls--;  /* skip the \n of prev line */
    /* Find start of previous line */
    uint32_t ls2 = ls;
    while (ls2 > 0 && ts->edit_buf[ls2-1] != '\n') ls2--;
    /* Advance by desired column */
    uint32_t want = ts->edit_want_col;
    uint32_t avail = ls - ls2;
    uint32_t adv = want < avail ? want : avail;
    ts->edit_cur = ls2 + adv;
    ts->edit_cur_line--;
    ts->edit_cur_col = (int)adv;
}

/* Move cursor down one line. */
void edit_move_down(text_state_t *ts) {
    /* Find end of current line (the \n) */
    uint32_t nl = ts->edit_cur;
    while (nl < ts->edit_size && ts->edit_buf[nl] != '\n') nl++;
    if (nl >= ts->edit_size) return;  /* already on last line */
    nl++;  /* skip the \n */
    /* Find end of next line */
    uint32_t nl2 = nl;
    while (nl2 < ts->edit_size && ts->edit_buf[nl2] != '\n') nl2++;
    /* Advance by desired column */
    uint32_t want  = ts->edit_want_col;
    uint32_t avail = nl2 - nl;
    uint32_t adv   = want < avail ? want : avail;
    ts->edit_cur = nl + adv;
    ts->edit_cur_line++;
    ts->edit_cur_col = (int)adv;
}

/* Move current line (or selection) up by one line (Alt+Up). */
void edit_move_line_up(window_t *w) {
    text_state_t *ts = &w->text;
    if (!ts->edit_buf || ts->edit_size == 0) return;
    /* Find start and end of current line */
    uint32_t ls = ts->edit_cur;
    while (ls > 0 && ts->edit_buf[ls-1] != '\n') ls--;
    if (ls == 0) return;  /* already first line */
    uint32_t le = ts->edit_cur;
    while (le < ts->edit_size && ts->edit_buf[le] != '\n') le++;
    /* le now points at '\n' or end-of-buffer */
    uint32_t cur_line_len = le - ls;  /* not counting '\n' */
    /* Find start of previous line */
    uint32_t prev_end = ls - 1;  /* points at '\n' ending prev line */
    uint32_t prev_start = prev_end;
    while (prev_start > 0 && ts->edit_buf[prev_start-1] != '\n') prev_start--;
    uint32_t prev_line_len = prev_end - prev_start;
    /* Build reordered block in a temp buffer (stack) */
    uint32_t total = prev_line_len + 1u + cur_line_len;  /* prev + '\n' + cur */
    if (total > 4096u) return;
    uint8_t tmp[4096];
    for (uint32_t i = 0; i < cur_line_len; i++) tmp[i] = ts->edit_buf[ls + i];
    tmp[cur_line_len] = '\n';
    for (uint32_t i = 0; i < prev_line_len; i++) tmp[cur_line_len + 1u + i] = ts->edit_buf[prev_start + i];
    for (uint32_t i = 0; i < total; i++) ts->edit_buf[prev_start + i] = tmp[i];
    /* Move cursor to same relative col in the (now upper) line */
    uint32_t col = (uint32_t)ts->edit_cur_col;
    if (col > cur_line_len) col = cur_line_len;
    ts->edit_cur = prev_start + col;
    ts->edit_modified = true;
}

/* Move current line (or selection) down by one line (Alt+Down). */
void edit_move_line_down(window_t *w) {
    text_state_t *ts = &w->text;
    if (!ts->edit_buf || ts->edit_size == 0) return;
    /* Find start and end of current line */
    uint32_t ls = ts->edit_cur;
    while (ls > 0 && ts->edit_buf[ls-1] != '\n') ls--;
    uint32_t le = ts->edit_cur;
    while (le < ts->edit_size && ts->edit_buf[le] != '\n') le++;
    if (le >= ts->edit_size) return;  /* already last line */
    uint32_t cur_line_len = le - ls;
    /* Find end of next line */
    uint32_t next_start = le + 1u;
    uint32_t next_end = next_start;
    while (next_end < ts->edit_size && ts->edit_buf[next_end] != '\n') next_end++;
    uint32_t next_line_len = next_end - next_start;
    /* Build reordered block: next + '\n' + cur */
    uint32_t total = next_line_len + 1u + cur_line_len;
    if (total > 4096u) return;
    uint8_t tmp[4096];
    for (uint32_t i = 0; i < next_line_len; i++) tmp[i] = ts->edit_buf[next_start + i];
    tmp[next_line_len] = '\n';
    for (uint32_t i = 0; i < cur_line_len; i++) tmp[next_line_len + 1u + i] = ts->edit_buf[ls + i];
    for (uint32_t i = 0; i < total; i++) ts->edit_buf[ls + i] = tmp[i];
    /* Move cursor to same relative col in the (now lower) line */
    uint32_t col = (uint32_t)ts->edit_cur_col;
    if (col > cur_line_len) col = cur_line_len;
    ts->edit_cur = ls + next_line_len + 1u + col;
    ts->edit_modified = true;
}


/* ── Syntax highlighting ─────────────────────────────────────────────── */

#define SYN_NORMAL   COL_FB_TXT
#define SYN_KEYWORD  0x00569cd6u
#define SYN_TYPE     0x004ec9b0u
#define SYN_FUNC     0x00dcdcaau   /* function call identifier */
#define SYN_COMMENT  0x00608850u
#define SYN_STRING   0x00ce9178u
#define SYN_PREPROC  0x00c586c0u
#define SYN_NUMBER   0x00b5cea8u
#define SYN_VAR      0x009cdcf0u   /* shell $variable */

typedef enum { LS_NORM, LS_CMT_L, LS_CMT_B, LS_STR, LS_STR_SQ, LS_STR3, LS_STR3_SQ, LS_CHR, LS_PP, LS_TMPL, LS_TAG, LS_CMT_HTML, LS_JSON_KEY } lex_st_t;

static bool syn_wch(unsigned char c) {
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';
}

static uint32_t syn_word_col(const char *w, int n, syn_lang_t lang) {
    /* C/C++ */
    if (lang == SYN_LANG_C) {
        static const char *types[] = {
            "bool","size_t","ssize_t","ptrdiff_t","uintptr_t","intptr_t",
            "int8_t","int16_t","int32_t","int64_t",
            "uint8_t","uint16_t","uint32_t","uint64_t",
            "true","false","NULL","nullptr",
            /* C++ standard types */
            "string","wstring","vector","map","unordered_map","set","unordered_set",
            "pair","tuple","optional","variant","any","span","string_view","wstring_view",
            "unique_ptr","shared_ptr","weak_ptr","function","thread","mutex","atomic",NULL
        };
        static const char *keys[] = {
            "auto","break","case","char","const","continue","default","do","double",
            "else","enum","extern","float","for","goto","if","inline","int","long",
            "register","restrict","return","short","signed","sizeof","static","struct",
            "switch","typedef","union","unsigned","void","volatile","while",
            /* C++ */
            "class","namespace","template","typename","public","private","protected",
            "virtual","override","final","explicit","delete","new","operator","friend",
            "constexpr","consteval","constinit","noexcept","this","throw","try","catch",
            "using","decltype","static_assert","concept","requires","co_await","co_return",
            "co_yield","export","import","module",NULL
        };
        for (int i = 0; types[i]; i++) {
            const char *k = types[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_TYPE;
        }
        for (int i = 0; keys[i]; i++) {
            const char *k = keys[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_KEYWORD;
        }
    } else if (lang == SYN_LANG_SH) {
        static const char *keys[] = {
            "if","fi","then","else","elif","for","do","done","while","until",
            "case","esac","in","function","return","exit","export","local",
            "echo","printf","read","shift","break","continue","true","false",NULL
        };
        for (int i = 0; keys[i]; i++) {
            const char *k = keys[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_KEYWORD;
        }
    } else if (lang == SYN_LANG_PY) {
        static const char *types[] = {
            "True","False","None",
            "int","float","str","bool","bytes","list","dict","set","tuple",
            "type","object","Exception","ValueError","TypeError","KeyError",
            "AttributeError","RuntimeError","StopIteration","NotImplementedError",
            "len","range","print","input","open","isinstance","issubclass",
            "hasattr","getattr","setattr","delattr","callable","iter","next",
            "enumerate","zip","map","filter","sorted","reversed","sum","min","max",
            "abs","round","hex","oct","bin","chr","ord","repr","format",
            "super","staticmethod","classmethod","property",NULL
        };
        static const char *keys[] = {
            "def","class","if","elif","else","for","while","with","as",
            "import","from","return","pass","break","continue","and","or",
            "not","in","is","lambda","yield","yield_from","global","nonlocal",
            "try","except","finally","raise","del","assert","async","await",
            "match","case",NULL
        };
        for (int i = 0; types[i]; i++) {
            const char *k = types[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_TYPE;
        }
        for (int i = 0; keys[i]; i++) {
            const char *k = keys[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_KEYWORD;
        }
    } else if (lang == SYN_LANG_ASM) {
        static const char *keys[] = {
            "mov","push","pop","call","ret","jmp","je","jne","jz","jnz",
            "jl","jle","jg","jge","ja","jae","jb","jbe",
            "add","sub","mul","div","imul","idiv","xor","and","or","not",
            "shl","shr","sar","sal","lea","cmp","test",
            "nop","hlt","int","iret","sti","cli","cpuid","syscall","sysret",
            "db","dw","dd","dq","resb","resw","resd","resq",
            "section","global","extern","bits","org","align","equ",NULL
        };
        static const char *regs[] = {
            "rax","rbx","rcx","rdx","rsi","rdi","rsp","rbp",
            "r8","r9","r10","r11","r12","r13","r14","r15",
            "eax","ebx","ecx","edx","esi","edi","esp","ebp",
            "ax","bx","cx","dx","si","di","sp","bp",
            "al","bl","cl","dl","ah","bh","ch","dh",
            "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7",
            "ymm0","ymm1","ymm2","ymm3","ymm4","ymm5","ymm6","ymm7",
            "rip","rflags","cs","ds","es","fs","gs","ss",NULL
        };
        for (int i = 0; keys[i]; i++) {
            const char *k = keys[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_KEYWORD;
        }
        for (int i = 0; regs[i]; i++) {
            const char *k = regs[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_TYPE;
        }
    } else if (lang == SYN_LANG_JSON) {
        static const char *vals[] = { "true","false","null",NULL };
        for (int i = 0; vals[i]; i++) {
            const char *k = vals[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_TYPE;
        }
    } else if (lang == SYN_LANG_LUA) {
        static const char *types[] = { "nil","true","false",NULL };
        static const char *keys[] = {
            "if","then","else","elseif","end","for","while","do",
            "repeat","until","function","local","return","break",
            "goto","and","or","not","in",NULL
        };
        for (int i = 0; types[i]; i++) {
            const char *k = types[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_TYPE;
        }
        for (int i = 0; keys[i]; i++) {
            const char *k = keys[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_KEYWORD;
        }
    } else if (lang == SYN_LANG_JS) {
        static const char *vals[] = { "null","undefined","true","false","NaN","Infinity","this","super","arguments",NULL };
        static const char *keys[] = {
            "var","let","const","function","class","extends","new","delete",
            "return","if","else","for","while","do","break","continue",
            "switch","case","default","try","catch","finally","throw",
            "typeof","instanceof","void","in","of","import","export",
            "from","async","await","yield","static","get","set",NULL
        };
        for (int i = 0; vals[i]; i++) {
            const char *k = vals[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_TYPE;
        }
        for (int i = 0; keys[i]; i++) {
            const char *k = keys[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_KEYWORD;
        }
    } else if (lang == SYN_LANG_MAKE) {
        static const char *keys[] = {
            "ifeq","ifneq","ifdef","ifndef","else","endif",
            "define","endef","export","unexport","override",
            "include","sinclude","vpath","private",NULL
        };
        for (int i = 0; keys[i]; i++) {
            const char *k = keys[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_KEYWORD;
        }
    } else if (lang == SYN_LANG_TOML) {
        static const char *vals[] = { "true","false",NULL };
        for (int i = 0; vals[i]; i++) {
            const char *k = vals[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_TYPE;
        }
    } else if (lang == SYN_LANG_YAML) {
        static const char *vals[] = { "true","false","null","yes","no","on","off",NULL };
        for (int i = 0; vals[i]; i++) {
            const char *k = vals[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_TYPE;
        }
    } else if (lang == SYN_LANG_HTML) {
        static const char *tags[] = {
            "html","head","body","title","meta","link","script","style","base",
            "div","span","p","a","br","hr","img","input","button","label","form",
            "select","option","textarea","table","thead","tbody","tr","th","td",
            "ul","ol","li","nav","header","footer","main","section","article","aside",
            "h1","h2","h3","h4","h5","h6","pre","code","blockquote","strong","em",
            "b","i","u","s","small","sup","sub","kbd","var","samp","cite","abbr",
            "figure","figcaption","canvas","svg","video","audio","source","track",
            "iframe","embed","object","param","details","summary","dialog","slot",
            "template","noscript","area","map","picture","time","address","mark",
            "output","progress","meter","fieldset","legend","datalist","optgroup",
            "colgroup","col","caption","tfoot","ins","del","wbr","data","dfn","ruby",
            "rp","rt","rtc","rb","bdi","bdo","q","dl","dt","dd",NULL
        };
        for (int i = 0; tags[i]; i++) {
            const char *k = tags[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_FUNC;
        }
    } else if (lang == SYN_LANG_CSS) {
        static const char *at_keys[] = {
            "media","import","export","charset","namespace","supports","keyframes",
            "font-face","page","layer","container","property","counter-style",NULL
        };
        for (int i = 0; at_keys[i]; i++) {
            const char *k = at_keys[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_PREPROC;
        }
    } else if (lang == SYN_LANG_INI) {
        static const char *vals[] = { "true","false","yes","no","on","off",NULL };
        for (int i = 0; vals[i]; i++) {
            const char *k = vals[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_TYPE;
        }
    } else if (lang == SYN_LANG_SQL) {
        /* SQL keywords — case-insensitive (stored lowercase) */
        static const char *keys[] = {
            "select","from","where","insert","update","delete","create","drop","alter",
            "table","index","view","database","schema","trigger","procedure","function",
            "join","left","right","inner","outer","full","cross","natural","on","using",
            "as","and","or","not","in","is","null","like","between","exists","any","all",
            "distinct","union","intersect","except","into","values","set","having",
            "order","group","by","limit","offset","asc","desc","with","recursive",
            "case","when","then","else","end","if","begin","commit","rollback",
            "transaction","savepoint","replace","truncate","explain","analyze",
            "foreign","primary","key","references","cascade","restrict","unique",
            "check","default","constraint","add","column","rename","to",NULL
        };
        static const char *types[] = {
            "int","integer","bigint","smallint","tinyint","mediumint","serial","bigserial",
            "float","double","decimal","numeric","real","money","boolean","bool",
            "varchar","char","text","blob","clob","nvarchar","nchar","ntext",
            "date","time","timestamp","datetime","interval","year",
            "binary","varbinary","uuid","json","jsonb","xml","array","bytea",NULL
        };
        for (int i = 0; keys[i]; i++) {
            const char *k = keys[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0;
            while (j < n) {
                unsigned char wc2 = (unsigned char)w[j];
                if (wc2 >= 'A' && wc2 <= 'Z') wc2 = (unsigned char)(wc2 + 32);
                if (wc2 != (unsigned char)k[j]) break;
                j++;
            }
            if (j == n) return SYN_KEYWORD;
        }
        for (int i = 0; types[i]; i++) {
            const char *k = types[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0;
            while (j < n) {
                unsigned char wc2 = (unsigned char)w[j];
                if (wc2 >= 'A' && wc2 <= 'Z') wc2 = (unsigned char)(wc2 + 32);
                if (wc2 != (unsigned char)k[j]) break;
                j++;
            }
            if (j == n) return SYN_TYPE;
        }
    } else if (lang == SYN_LANG_RUST) {
        static const char *types[] = {
            "bool","char","str","i8","i16","i32","i64","i128","u8","u16","u32","u64","u128",
            "f32","f64","isize","usize","String","Vec","HashMap","HashSet","BTreeMap","BTreeSet",
            "Option","Result","Some","None","Ok","Err","Box","Rc","Arc","Cell","RefCell",
            "Mutex","RwLock","Cow","Pin","PhantomData","true","false",NULL
        };
        static const char *keys[] = {
            "fn","let","mut","pub","use","mod","struct","enum","impl","trait","for",
            "while","loop","if","else","return","match","const","static","type","where",
            "self","Self","super","move","unsafe","extern","async","await","dyn","box",
            "ref","continue","break","in","as","crate","macro_rules",NULL
        };
        for (int i = 0; types[i]; i++) {
            const char *k = types[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_TYPE;
        }
        for (int i = 0; keys[i]; i++) {
            const char *k = keys[i]; int kl = 0; while (k[kl]) kl++;
            if (kl != n) continue;
            int j = 0; while (j < n && w[j] == k[j]) j++;
            if (j == n) return SYN_KEYWORD;
        }
    }
    return SYN_NORMAL;
}

/* Detect language from file path extension */
syn_lang_t detect_lang(const char *path) {
    int len = (int)gui_strlen(path);
    if (len < 2) return SYN_LANG_NONE;
    /* C/C++: .c .h .cpp .hpp */
    if (path[len-2]=='.' && (path[len-1]=='c' || path[len-1]=='h')) return SYN_LANG_C;
    if (len>=4 && path[len-4]=='.' && path[len-3]=='c' && path[len-2]=='p' && path[len-1]=='p') return SYN_LANG_C;
    if (len>=4 && path[len-4]=='.' && path[len-3]=='h' && path[len-2]=='p' && path[len-1]=='p') return SYN_LANG_C;
    /* Shell: .sh .bash */
    if (path[len-3]=='.' && path[len-2]=='s' && path[len-1]=='h') return SYN_LANG_SH;
    if (len>=5 && path[len-5]=='.' && path[len-4]=='b' && path[len-3]=='a' && path[len-2]=='s' && path[len-1]=='h') return SYN_LANG_SH;
    /* Python: .py */
    if (path[len-3]=='.' && path[len-2]=='p' && path[len-1]=='y') return SYN_LANG_PY;
    /* Assembly: .s .asm */
    if (path[len-2]=='.' && path[len-1]=='s') return SYN_LANG_ASM;
    if (len>=4 && path[len-4]=='.' && path[len-3]=='a' && path[len-2]=='s' && path[len-1]=='m') return SYN_LANG_ASM;
    /* JSON: .json */
    if (len>=5 && path[len-5]=='.' && path[len-4]=='j' && path[len-3]=='s' && path[len-2]=='o' && path[len-1]=='n') return SYN_LANG_JSON;
    /* Lua: .lua */
    if (len>=4 && path[len-4]=='.' && path[len-3]=='l' && path[len-2]=='u' && path[len-1]=='a') return SYN_LANG_LUA;
    /* JavaScript/TypeScript: .js .ts .mjs .jsx .tsx */
    if (path[len-3]=='.' && path[len-2]=='j' && path[len-1]=='s') return SYN_LANG_JS;
    if (path[len-3]=='.' && path[len-2]=='t' && path[len-1]=='s') return SYN_LANG_JS;
    if (len>=4 && path[len-4]=='.' && path[len-3]=='m' && path[len-2]=='j' && path[len-1]=='s') return SYN_LANG_JS;
    if (len>=4 && path[len-4]=='.' && path[len-3]=='j' && path[len-2]=='s' && path[len-1]=='x') return SYN_LANG_JS;
    if (len>=4 && path[len-4]=='.' && path[len-3]=='t' && path[len-2]=='s' && path[len-1]=='x') return SYN_LANG_JS;
    /* Makefile: Makefile, makefile, GNUmakefile, .mk */
    {
        /* Check filename component for Makefile */
        int last_sep = -1;
        for (int i = 0; i < len; i++) if (path[i]=='/') last_sep = i;
        const char *base = path + last_sep + 1;
        int blen = len - last_sep - 1;
        bool is_makefile = false;
        if (blen == 8) {
            is_makefile = (base[0]=='M'&&base[1]=='a'&&base[2]=='k'&&base[3]=='e'&&
                           base[4]=='f'&&base[5]=='i'&&base[6]=='l'&&base[7]=='e')
                       || (base[0]=='m'&&base[1]=='a'&&base[2]=='k'&&base[3]=='e'&&
                           base[4]=='f'&&base[5]=='i'&&base[6]=='l'&&base[7]=='e');
        } else if (blen == 11) {
            is_makefile = (base[0]=='G'&&base[1]=='N'&&base[2]=='U'&&
                           base[3]=='m'&&base[4]=='a'&&base[5]=='k'&&base[6]=='e'&&
                           base[7]=='f'&&base[8]=='i'&&base[9]=='l'&&base[10]=='e');
        }
        if (is_makefile) return SYN_LANG_MAKE;
        if (len>=3 && path[len-3]=='.' && path[len-2]=='m' && path[len-1]=='k') return SYN_LANG_MAKE;
    }
    /* TOML: .toml */
    if (len>=5 && path[len-5]=='.' && path[len-4]=='t' && path[len-3]=='o' && path[len-2]=='m' && path[len-1]=='l') return SYN_LANG_TOML;
    /* YAML: .yml .yaml */
    if (len>=4 && path[len-4]=='.' && path[len-3]=='y' && path[len-2]=='m' && path[len-1]=='l') return SYN_LANG_YAML;
    if (len>=5 && path[len-5]=='.' && path[len-4]=='y' && path[len-3]=='a' && path[len-2]=='m' && path[len-1]=='l') return SYN_LANG_YAML;
    /* HTML: .html .htm */
    if (len>=5 && path[len-5]=='.' && path[len-4]=='h' && path[len-3]=='t' && path[len-2]=='m' && path[len-1]=='l') return SYN_LANG_HTML;
    if (len>=4 && path[len-4]=='.' && path[len-3]=='h' && path[len-2]=='t' && path[len-1]=='m') return SYN_LANG_HTML;
    /* XML: same tag/comment syntax as HTML */
    if (len>=4 && path[len-4]=='.' && path[len-3]=='x' && path[len-2]=='m' && path[len-1]=='l') return SYN_LANG_HTML;
    /* CSS: .css */
    if (len>=4 && path[len-4]=='.' && path[len-3]=='c' && path[len-2]=='s' && path[len-1]=='s') return SYN_LANG_CSS;
    /* INI/CFG: .ini .cfg */
    if (len>=4 && path[len-4]=='.' && path[len-3]=='i' && path[len-2]=='n' && path[len-1]=='i') return SYN_LANG_INI;
    if (len>=4 && path[len-4]=='.' && path[len-3]=='c' && path[len-2]=='f' && path[len-1]=='g') return SYN_LANG_INI;
    /* Markdown: .md */
    if (len>=3 && path[len-3]=='.' && path[len-2]=='m' && path[len-1]=='d') return SYN_LANG_MD;
    /* Diff/patch: .diff .patch */
    if (len>=5 && path[len-5]=='.' && path[len-4]=='d' && path[len-3]=='i' && path[len-2]=='f' && path[len-1]=='f') return SYN_LANG_DIFF;
    if (len>=6 && path[len-6]=='.' && path[len-5]=='p' && path[len-4]=='a' && path[len-3]=='t' && path[len-2]=='c' && path[len-1]=='h') return SYN_LANG_DIFF;
    /* SQL: .sql */
    if (len>=4 && path[len-4]=='.' && path[len-3]=='s' && path[len-2]=='q' && path[len-1]=='l') return SYN_LANG_SQL;
    /* Rust: .rs */
    if (len>=3 && path[len-3]=='.' && path[len-2]=='r' && path[len-1]=='s') return SYN_LANG_RUST;
    return SYN_LANG_NONE;
}

static void text_update_counts(window_t *w);  /* forward declaration */

/* Search forward/backward through text data.
 * from_next=true: start after current match; false: restart from beginning.
 * Updates srch_match_line/col and auto-scrolls to the match. */
void text_search_next(window_t *w, bool from_next) {
    text_state_t *ts = &w->text;
    if (ts->srch_len == 0) { ts->srch_match_line = -1; return; }
    /* Use edit buffer when in edit mode, otherwise read-only data */
    const char *d = (ts->edit_mode && ts->edit_buf)
                    ? (const char *)ts->edit_buf : (const char *)ts->data;
    uint64_t sz   = (ts->edit_mode && ts->edit_buf) ? (uint64_t)ts->edit_size : ts->size;
    const char *q = ts->srch_buf;
    int qlen      = ts->srch_len;
    bool fold     = ts->srch_case_fold;

    /* Compute byte offset one past the current match start (or 0) */
    uint64_t start = 0;
    if (from_next && ts->srch_match_line >= 0) {
        int ln = 0, cl = 0;
        for (uint64_t i = 0; i < sz; i++) {
            if (ln == ts->srch_match_line && cl == ts->srch_match_col) {
                start = i + 1u;
                break;
            }
            if ((unsigned char)d[i] == '\n') { ln++; cl = 0; } else cl++;
        }
    }

    /* Two-pass scan: [start..sz), then [0..start) for wrap */
    for (int pass = 0; pass < 2; pass++) {
        uint64_t b = (pass == 0) ? start : 0u;
        uint64_t e = (pass == 0) ? sz    : start;
        if (b >= e || b + (uint64_t)qlen > e) { if (start == 0) break; continue; }

        /* Count (line,col) at b */
        int ln = 0, cl = 0;
        for (uint64_t i = 0; i < b; i++) {
            if ((unsigned char)d[i] == '\n') { ln++; cl = 0; } else cl++;
        }

        for (uint64_t i = b; i + (uint64_t)qlen <= e; ) {
            bool hit = true;
            for (int j = 0; j < qlen; j++)
                if (!srch_ceq((unsigned char)d[i+(uint64_t)j], (unsigned char)q[j], fold))
                    { hit = false; break; }
            if (hit) {
                ts->srch_match_line = ln;
                ts->srch_match_col  = cl;
                /* Scroll to center the match vertically */
                {
                    uint64_t _fh = console_font_height();
                    uint64_t _tvsh = _fh + 4u;
                    uint64_t _ih = w->h > TITLE_H + BORDER ? w->h - TITLE_H - BORDER : 1u;
                    uint64_t _iht = _ih > _tvsh ? _ih - _tvsh : 1u;
                    int _mr = (int)((_iht > 2u*PAD ? _iht - 2u*PAD : 1u) / _fh);
                    if (_mr < 1) _mr = 10;
                    int new_scroll = ln - _mr / 2;
                    if (new_scroll < 0) new_scroll = 0;
                    ts->scroll = new_scroll;
                }
                /* In edit mode: move cursor to match and select it */
                if (ts->edit_mode && ts->edit_buf) {
                    ts->edit_cur    = (uint32_t)i;
                    ts->sel_anchor  = (int32_t)i;
                    ts->sel_end     = (int32_t)i + qlen;
                    edit_sync_pos(ts);
                    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
                }
                text_update_counts(w);
                return;
            }
            if ((unsigned char)d[i] == '\n') { ln++; cl = 0; } else cl++;
            i++;
        }
        if (start == 0) break;
    }
    ts->srch_match_line = -1;
    text_update_counts(w);
}

/* Search backward from current match (or file end). Wraps around. O(n). */
void text_search_prev(window_t *w) {
    text_state_t *ts = &w->text;
    if (ts->srch_len == 0) { ts->srch_match_line = -1; return; }
    const char *d = (ts->edit_mode && ts->edit_buf)
                    ? (const char *)ts->edit_buf : (const char *)ts->data;
    uint64_t sz   = (ts->edit_mode && ts->edit_buf) ? (uint64_t)ts->edit_size : ts->size;
    const char *q = ts->srch_buf;
    int qlen      = ts->srch_len;
    bool fold     = ts->srch_case_fold;
    if (!d || sz < (uint64_t)qlen) { ts->srch_match_line = -1; return; }

    /* Find byte offset of current match to know where to stop */
    int64_t cur_off = -1;
    if (ts->srch_match_line >= 0) {
        int ln = 0, cl = 0;
        for (uint64_t i = 0; i < sz; i++) {
            if (ln == ts->srch_match_line && cl == ts->srch_match_col) {
                cur_off = (int64_t)i; break;
            }
            if ((unsigned char)d[i] == '\n') { ln++; cl = 0; } else cl++;
        }
    }
    /* Search from (cur_off - 1) backwards, then wrap */
    int64_t search_start = (cur_off > 0) ? cur_off - 1 : (int64_t)sz - (int64_t)qlen;

    for (int pass = 0; pass < 2; pass++) {
        int64_t hi = (pass == 0) ? search_start : (int64_t)sz - (int64_t)qlen;
        int64_t lo = (pass == 0) ? 0 : search_start + 1;
        if (hi < 0 || hi < lo) continue;
        /* Scan once forward [0..hi] to build cumulative line state, but we need it at each pos.
         * Approach: scan forward keeping running (line,col,byte); record last match seen. */
        int best_ln = -1, best_cl = -1; int64_t best_i = -1;
        int ln = 0, cl = 0;
        for (int64_t i = lo; i <= hi; i++) {
            /* Check match at i */
            if (i + (int64_t)qlen <= (int64_t)sz) {
                bool hit = true;
                for (int j = 0; j < qlen; j++)
                    if (!srch_ceq((unsigned char)d[(uint64_t)i+(uint64_t)j],
                                  (unsigned char)q[j], fold))
                        { hit = false; break; }
                if (hit) { best_ln = ln; best_cl = cl; best_i = i; }
            }
            if ((unsigned char)d[(uint64_t)i] == '\n') { ln++; cl = 0; } else cl++;
        }
        if (best_i >= 0) {
            ts->srch_match_line = best_ln;
            ts->srch_match_col  = best_cl;
            {
                uint64_t _fh = console_font_height();
                uint64_t _tvsh = _fh + 4u;
                uint64_t _ih = w->h > TITLE_H + BORDER ? w->h - TITLE_H - BORDER : 1u;
                uint64_t _iht = _ih > _tvsh ? _ih - _tvsh : 1u;
                int _mr = (int)((_iht > 2u*PAD ? _iht - 2u*PAD : 1u) / _fh);
                if (_mr < 1) _mr = 10;
                int ns = best_ln - _mr / 2;
                if (ns < 0) ns = 0;
                ts->scroll = ns;
            }
            if (ts->edit_mode && ts->edit_buf) {
                ts->edit_cur   = (uint32_t)best_i;
                ts->sel_anchor = (int32_t)best_i;
                ts->sel_end    = (int32_t)best_i + qlen;
                edit_sync_pos(ts);
                ts->edit_want_col = (uint32_t)ts->edit_cur_col;
            }
            text_update_counts(w);
            return;
        }
        if (cur_off < 0) break;
    }
    ts->srch_match_line = -1;
    text_update_counts(w);
}

/* Count total non-overlapping occurrences and set srch_cur_idx for the active match. */
static void text_update_counts(window_t *w) {
    text_state_t *ts = &w->text;
    int qlen = ts->srch_len;
    bool has_data = (ts->edit_mode && ts->edit_buf) || ts->data;
    if (qlen <= 0 || !has_data) { ts->srch_total_count = 0; ts->srch_cur_idx = 0; return; }
    const char *d = (ts->edit_mode && ts->edit_buf)
                    ? (const char *)ts->edit_buf : (const char *)ts->data;
    const char *q = ts->srch_buf;
    uint64_t sz   = (ts->edit_mode && ts->edit_buf) ? (uint64_t)ts->edit_size : ts->size;
    bool fold     = ts->srch_case_fold;
    int cnt = 0, cur_idx = 0;
    int ln  = 0, cl = 0;
    uint64_t i = 0;
    while (i + (uint64_t)qlen <= sz) {
        bool hit = true;
        for (int j = 0; j < qlen; j++)
            if (!srch_ceq((unsigned char)d[i+(uint64_t)j], (unsigned char)q[j], fold))
                { hit=false; break; }
        if (hit) {
            cnt++;
            if (ln == ts->srch_match_line && cl == ts->srch_match_col)
                cur_idx = cnt;
            /* advance past this match, tracking ln/cl */
            for (int j = 0; j < qlen; j++) {
                if ((unsigned char)d[i] == '\n') { ln++; cl = 0; } else cl++;
                i++;
            }
        } else {
            if ((unsigned char)d[i] == '\n') { ln++; cl = 0; } else cl++;
            i++;
        }
    }
    ts->srch_total_count = cnt;
    ts->srch_cur_idx     = cur_idx;
}

#define TV_SRCH_BAR_H  (console_font_height() + 8u)
#define TV_SRCH_BG     0x000d1420u
#define TV_SRCH_BORDER 0x00203040u
#define TV_SRCH_PROMPT 0x004888c8u
#define TV_SRCH_TXT    0x00c8d8ffu
#define TV_SRCH_HL_BG  0x00294060u
#define TV_SRCH_HL_DIM 0x00162030u   /* secondary (non-active) search match bg */
#define TV_SEL_BG      0x001c3e60u   /* text selection background */
#define TV_BRACKET_HL  0x00405820u   /* matched bracket pair highlight */
#define TV_CUR_LINE_BG 0x000e1828u   /* active cursor line background (edit mode) */
#define TV_COL80_BG    0x000b1420u   /* column 80 guide tint */
#define TV_MINIMAP_W   50u           /* minimap panel width in pixels */

/* Compute per-character background: search > cursor-line > normal. */
static inline uint32_t tv_cbg(int col, bool lhm, int ms, int me,
                               bool do_hl, const int *lmc, int lmc_cnt, int qln,
                               bool cur_line) {
    if (lhm && col >= ms && col < me) return TV_SRCH_HL_BG;
    if (do_hl) {
        for (int _i = 0; _i < lmc_cnt; _i++)
            if (col >= lmc[_i] && col < lmc[_i] + qln) return TV_SRCH_HL_DIM;
    }
    if (col == 80) return cur_line ? 0x00141e30u : TV_COL80_BG;
    return cur_line ? TV_CUR_LINE_BG : COL_FB_LIST_BG;
}

void text_render(window_t *w) {
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();
    uint64_t ix = w->x + BORDER;
    uint64_t iy = w->y + TITLE_H;
    uint64_t iw = w->w - 2u * BORDER;
    uint64_t ih = w->h - TITLE_H - BORDER;

    /* Reserve bottom strip for status bar, then search bar when active */
    uint64_t tv_status_h = fh + 4u;
    uint64_t srch_bar_h  = w->text.srch_active
                           ? (w->text.srch_is_repl ? 2u * (fh + 8u) : fh + 8u)
                           : 0u;
    uint64_t save_as_bar_h = (w->text.save_as_active || w->text.open_bar_active) ? (fh + 8u) : 0u;
    uint64_t reserved_h  = tv_status_h + srch_bar_h + save_as_bar_h;
    uint64_t ih_text     = ih > reserved_h ? ih - reserved_h : 1u;

    syn_lang_t hl_lang = w->text.lang;
    bool do_hl = (hl_lang != SYN_LANG_NONE);
    bool lang_hash_cmt = (hl_lang == SYN_LANG_SH || hl_lang == SYN_LANG_PY || hl_lang == SYN_LANG_ASM
                       || hl_lang == SYN_LANG_MAKE || hl_lang == SYN_LANG_TOML || hl_lang == SYN_LANG_YAML
                       || hl_lang == SYN_LANG_INI);
    bool lang_semi_cmt = (hl_lang == SYN_LANG_INI); /* INI: ';' line comments */
    bool lang_c = (hl_lang == SYN_LANG_C);           /* C-only: char literals, preprocessor */
    bool lang_cmt = (hl_lang == SYN_LANG_C || hl_lang == SYN_LANG_JS || hl_lang == SYN_LANG_CSS
                  || hl_lang == SYN_LANG_SQL || hl_lang == SYN_LANG_RUST); /* C-style comments */
    bool lang_sq_str = (hl_lang == SYN_LANG_PY || hl_lang == SYN_LANG_LUA
                     || hl_lang == SYN_LANG_JS || hl_lang == SYN_LANG_TOML
                     || hl_lang == SYN_LANG_YAML || hl_lang == SYN_LANG_CSS
                     || hl_lang == SYN_LANG_INI || hl_lang == SYN_LANG_SQL);
    bool lang_py = (hl_lang == SYN_LANG_PY);     /* Python: triple-quoted strings """...""" */
    bool lang_dash_cmt = (hl_lang == SYN_LANG_LUA || hl_lang == SYN_LANG_SQL); /* -- line comments */
    bool lang_tmpl = (hl_lang == SYN_LANG_JS);    /* JS: backtick template literals */
    bool lang_html = (hl_lang == SYN_LANG_HTML);  /* HTML: tag/comment states */
    bool lang_md   = (hl_lang == SYN_LANG_MD);   /* Markdown: line-based highlighting */
    bool lang_diff = (hl_lang == SYN_LANG_DIFF);  /* Diff/patch: +/-/@@ lines */

    console_fill_rect(ix, iy, iw, ih, COL_FB_LIST_BG);

    bool em = w->text.edit_mode && w->text.edit_buf;
    const char *d2 = em ? (const char *)w->text.edit_buf : (const char *)w->text.data;
    uint64_t    sz = em ? (uint64_t)w->text.edit_size : w->text.size;

    if (!d2 || sz == 0) {
        if (!em && w->text.path[0] == '\0') {
            /* No file loaded — welcome screen with shortcuts and recent files */
            uint64_t fw2 = console_font_width(), fh2 = console_font_height();
            static const char *hl[] = {
                "Text Viewer / Editor",
                "",
                "Ctrl+O  open file  Ctrl+E  new file",
                "or open a file from the Files window",
                "",
                "Ctrl+F  find       Ctrl+G  go to line",
                "Ctrl+R  replace    Ctrl+B  reveal in Files",
                "W       word wrap  j/k     scroll",
                NULL
            };
            int nhl = 0; while (hl[nhl]) nhl++;
            /* Compute total block height: hints + optional recent section */
            int nrec = g_recent_count;
            int rec_rows = nrec > 0 ? nrec + 2 : 0; /* header + blank + entries */
            uint64_t block_h = (uint64_t)(nhl + rec_rows) * fh2 + 4u;
            uint64_t top_y   = iy + (ih > block_h + 8u ? (ih - block_h) / 2u : 4u);
            for (int li = 0; hl[li]; li++) {
                uint64_t ll = (uint64_t)gui_strlen(hl[li]);
                uint64_t lx2 = ll > 0 ? ix + (iw > ll * fw2 ? (iw - ll * fw2) / 2u : PAD) : ix;
                uint32_t lc = (li == 0) ? 0x00506878u : (li < 5) ? 0x00384c60u : 0x00283848u;
                gui_draw_str(lx2, top_y + (uint64_t)li * fh2, hl[li], lc, COL_FB_LIST_BG);
            }
            if (nrec > 0) {
                uint64_t ry = top_y + (uint64_t)nhl * fh2 + fh2;
                /* "Recent Files" section header */
                const char *rec_hdr = "Recent Files";
                uint64_t rhl = (uint64_t)gui_strlen(rec_hdr);
                uint64_t rhx = ix + (iw > rhl * fw2 ? (iw - rhl * fw2) / 2u : PAD);
                gui_draw_str(rhx, ry, rec_hdr, 0x00406078u, COL_FB_LIST_BG);
                ry += fh2;
                int rec_hov = w->text.welcome_hover;
                for (int ri = 0; ri < nrec && ri < RECENT_MAX; ri++) {
                    const char *rpath = g_recent[ri];
                    const char *rbase = rpath;
                    for (const char *rp = rpath; *rp; rp++) if (*rp == '/') rbase = rp + 1;
                    /* "[1] basename" */
                    char rline[136];
                    rline[0] = '['; rline[1] = (char)('1' + ri); rline[2] = ']'; rline[3] = ' ';
                    int rli = 4;
                    for (const char *rp = rbase; *rp && rli < 134; rp++) rline[rli++] = *rp;
                    rline[rli] = '\0';
                    uint64_t rl = (uint64_t)rli;
                    uint64_t rlx = ix + (iw > rl * fw2 ? (iw - rl * fw2) / 2u : PAD);
                    bool rhov = (ri == rec_hov);
                    uint32_t rbg = rhov ? 0x00182030u : COL_FB_LIST_BG;
                    uint32_t rfg = rhov ? 0x0090d0f8u : 0x00405870u;
                    if (rhov) console_fill_rect(ix + 4u, ry + (uint64_t)ri * fh2, iw - 8u, fh2, rbg);
                    gui_draw_str(rlx, ry + (uint64_t)ri * fh2, rline, rfg, rbg);
                }
                /* Show full path of hovered recent file below the list */
                if (rec_hov >= 0 && rec_hov < nrec) {
                    const char *hp = g_recent[rec_hov];
                    uint64_t hpl = (uint64_t)gui_strlen(hp);
                    uint64_t hpx = ix + (iw > hpl * fw2 ? (iw - hpl * fw2) / 2u : PAD);
                    uint64_t hpy = ry + (uint64_t)nrec * fh2 + 2u;
                    gui_draw_str(hpx, hpy, hp, 0x00283848u, COL_FB_LIST_BG);
                }
            }
        } else {
            const char *msg = em ? "(empty buffer)" : "(empty file)";
            gui_draw_str(ix + PAD, iy + PAD, msg, COL_FB_MUTED, COL_FB_LIST_BG);
        }
        return;
    }

    /* Line number gutter */
    uint64_t gutter_chars = 1;
    int tot = w->text.total_lines > 0 ? w->text.total_lines : 1;
    while (tot >= 10) { tot /= 10; gutter_chars++; }
    uint64_t gutter_w  = (gutter_chars + 2u) * fw;
    uint64_t gutter_bg = 0x00090d14u;
    console_fill_rect(ix, iy, gutter_w, ih_text, gutter_bg);
    console_fill_rect(ix + gutter_w, iy, 1u, ih_text, 0x00202830u);

    uint64_t tx        = ix + gutter_w + 1u;
    /* Show minimap when file has more lines than visible rows */
    bool show_mm = (w->text.total_lines > 0 &&
                    w->text.total_lines > (int)((ih_text > 2u * PAD ? ih_text - 2u * PAD : 1u) / fh));
    uint64_t mm_rsv    = show_mm ? (TV_MINIMAP_W + 1u) : 0u;  /* space reserved on right for minimap */
    uint64_t avail_w   = iw > gutter_w + 13u + mm_rsv ? iw - gutter_w - 13u - mm_rsv : 1u;
    uint64_t max_cols  = avail_w > 2u * PAD ? (avail_w - 2u * PAD) / fw : 1u;
    uint64_t max_rows  = ih_text > 2u * PAD ? (ih_text - 2u * PAD) / fh : 1u;
    if (max_cols < 1) max_cols = 1;
    if (max_rows < 1) max_rows = 1;
    uint64_t hs = (!w->text.word_wrap && w->text.h_scroll > 0)
                  ? (uint64_t)w->text.h_scroll : 0u;
    bool ww = w->text.word_wrap;

    int max_scroll = w->text.total_lines - (int)max_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (w->text.scroll > max_scroll) w->text.scroll = max_scroll;
    if (w->text.scroll < 0)         w->text.scroll = 0;

    if (!ww && w->text.max_line_len > 0) {
        int max_hs = w->text.max_line_len - (int)max_cols;
        if (max_hs < 0) max_hs = 0;
        if (w->text.h_scroll > max_hs) w->text.h_scroll = max_hs;
    }

    const char *d   = d2;
    uint64_t    pos = 0;
    int line = 0;

    /* Compute lex state at scroll position by scanning from start */
    lex_st_t lx = LS_NORM;
    if (do_hl) {
        unsigned char pc = 0;
        int tq2 = 0, tq2sq = 0;  /* consecutive closing-quote counters for LS_STR3/LS_STR3_SQ */
        int html_dd = 0;          /* consecutive dashes seen in LS_CMT_HTML (for --> detection) */
        while (pos < sz && line < w->text.scroll) {
            unsigned char c = (unsigned char)d[pos];
            if (c == '\r') { pos++; continue; }  /* skip CRLF carriage return */
            if (c == '\n') {
                /* LS_STR3/LS_STR3_SQ/LS_TMPL/LS_CMT_B/LS_TAG/LS_CMT_HTML persist across newlines */
                if (lx == LS_CMT_L || lx == LS_STR || lx == LS_STR_SQ || lx == LS_CHR || lx == LS_PP)
                    lx = LS_NORM;
                tq2 = 0; tq2sq = 0; html_dd = 0;
                line++;
            } else {
                switch (lx) {
                case LS_NORM:
                    if (lang_cmt && pc=='/' && c=='/') lx = LS_CMT_L;
                    else if (lang_cmt && pc=='/' && c=='*') lx = LS_CMT_B;
                    else if (lang_hash_cmt && c=='#') lx = LS_CMT_L;
                    else if (lang_semi_cmt && c==';') lx = LS_CMT_L;
                    else if (lang_dash_cmt && pc=='-' && c=='-') lx = LS_CMT_L;
                    else if (lang_html && c=='<') {
                        if (pos+3 < sz && (unsigned char)d[pos+1]=='!'
                            && (unsigned char)d[pos+2]=='-' && (unsigned char)d[pos+3]=='-')
                            { lx = LS_CMT_HTML; pos += 3; html_dd = 0; }
                        else
                            lx = LS_TAG;
                    }
                    else if (lang_py && c=='"' && pos+1 < sz && d[pos+1]=='"' && pos+2 < sz && d[pos+2]=='"')
                        { lx = LS_STR3; pos += 2; tq2 = 0; }
                    else if (c=='"') lx = LS_STR;
                    else if (lang_c && c=='\'') lx = LS_CHR;
                    else if (lang_py && c=='\'' && pos+1 < sz && d[pos+1]=='\'' && pos+2 < sz && d[pos+2]=='\'')
                        { lx = LS_STR3_SQ; pos += 2; tq2sq = 0; }
                    else if (lang_sq_str && c=='\'') lx = LS_STR_SQ;
                    else if (lang_tmpl && c=='`') lx = LS_TMPL;
                    else if (lang_c && c=='#') lx = LS_PP;
                    break;
                case LS_CMT_B:
                    if (pc=='*' && c=='/') lx = LS_NORM;
                    break;
                case LS_TAG:
                    if (c=='>') lx = LS_NORM;
                    break;
                case LS_CMT_HTML:
                    if (c=='-') html_dd++;
                    else if (c=='>' && html_dd >= 2) { lx = LS_NORM; html_dd = 0; }
                    else html_dd = 0;
                    break;
                case LS_STR:
                    if (c=='"' && pc!='\\') lx = LS_NORM;
                    break;
                case LS_STR_SQ:
                    if (c=='\'' && pc!='\\') lx = LS_NORM;
                    break;
                case LS_STR3:
                    if (c=='"') { if (++tq2 >= 3) { lx = LS_NORM; tq2 = 0; } }
                    else tq2 = 0;
                    break;
                case LS_STR3_SQ:
                    if (c=='\'') { if (++tq2sq >= 3) { lx = LS_NORM; tq2sq = 0; } }
                    else tq2sq = 0;
                    break;
                case LS_CHR:
                    if (c=='\'' && pc!='\\') lx = LS_NORM;
                    break;
                case LS_TMPL:
                    if (c=='`' && pc!='\\') lx = LS_NORM;
                    break;
                default: break;
                }
            }
            pc = c;
            pos++;
        }
    } else {
        /* Non-highlighted: fast skip */
        while (pos < sz && line < w->text.scroll) {
            while (pos < sz && d[pos] != '\n') pos++;
            if (pos < sz) pos++;
            line++;
        }
    }

    /* Selection range for highlight (only in edit mode) */
    bool sel_active = w->text.sel_anchor >= 0;
    int32_t sel_lo = 0, sel_hi = 0;
    if (sel_active) edit_sel_range(&w->text, &sel_lo, &sel_hi);
    if (sel_lo >= sel_hi) sel_active = false;

    /* Bracket matching: pre-compute matching bracket position for edit mode */
    uint32_t bm_cur = UINT32_MAX, bm_match = UINT32_MAX;
    if (em && w->text.edit_cur < w->text.edit_size) {
        unsigned char _bc = w->text.edit_buf[w->text.edit_cur];
        uint32_t _bp = w->text.edit_cur;
        if (_bc == '(' || _bc == '{' || _bc == '[') {
            unsigned char _open = _bc;
            unsigned char _close = (_bc=='(') ? ')' : (_bc=='{') ? '}' : ']';
            int _depth = 0; uint32_t _p = _bp;
            while (_p < w->text.edit_size) {
                if (w->text.edit_buf[_p] == _open)  _depth++;
                else if (w->text.edit_buf[_p] == _close) {
                    _depth--;
                    if (_depth == 0) { bm_cur = _bp; bm_match = _p; break; }
                }
                _p++;
            }
        } else if (_bc == ')' || _bc == '}' || _bc == ']') {
            unsigned char _close = _bc;
            unsigned char _open  = (_bc==')') ? '(' : (_bc=='}') ? '{' : '[';
            int _depth = 0; uint32_t _p = _bp;
            while (1) {
                if (w->text.edit_buf[_p] == _close) _depth++;
                else if (w->text.edit_buf[_p] == _open) {
                    _depth--;
                    if (_depth == 0) { bm_cur = _bp; bm_match = _p; break; }
                }
                if (_p == 0) break; _p--;
            }
        }
    }

    /* Render visible lines */
    int row = 0;
    int log_row = 0;   /* logical line index (word-wrap can make row > log_row) */
    int tqc = 0, tqcsq = 0;  /* consecutive closing-quote counters for LS_STR3/LS_STR3_SQ */
    int html_dd2 = 0;         /* consecutive dashes in LS_CMT_HTML render loop */
    bool tag_name = false;    /* true while still reading HTML tag name (before first space) */
    while (pos <= sz && (uint64_t)row < max_rows) {
        uint64_t py = iy + PAD + (uint64_t)row * fh;

        /* Line number — keyed to logical line, not visual row */
        int linenum = w->text.scroll + log_row + 1;
        bool is_cursor_line = em && ((w->text.scroll + log_row) == w->text.edit_cur_line);
        char lnbuf[8]; gui_itoa(linenum, lnbuf, 8);
        uint64_t ln_len = (uint64_t)gui_strlen(lnbuf);
        uint64_t ln_x   = ix + gutter_w - (ln_len + 1u) * fw;
        /* Current-line highlight: tinted row background and bright line number */
        if (is_cursor_line)
            console_fill_rect(tx, py, avail_w + 10u, fh, TV_CUR_LINE_BG);
        /* Column 80 guide */
        if (hs <= 80u) {
            uint64_t g80_x = tx + PAD + (80u - hs) * fw;
            if (g80_x + 1u < tx + avail_w)
                console_fill_rect(g80_x, py, 1u, fh, TV_COL80_BG);
        }
        uint32_t ln_fg = is_cursor_line ? 0x00708898u : 0x00405060u;
        gui_draw_str(ln_x, py, lnbuf, ln_fg, gutter_bg);

        /* Reset line-scoped states; LS_STR3/LS_STR3_SQ/LS_TMPL/LS_CMT_B/LS_TAG/LS_CMT_HTML persist */
        if (lx == LS_CMT_L || lx == LS_STR || lx == LS_STR_SQ || lx == LS_CHR || lx == LS_PP)
            lx = LS_NORM;
        tqc = 0; tqcsq = 0; html_dd2 = 0; /* tag_name not reset: tag name never spans lines */
        bool line_start = true; /* no non-space char seen yet */

        /* All-match search highlight pre-scan for this logical line */
        int cur_line = w->text.scroll + log_row;
        int qlen     = w->text.srch_len;
        bool fold2   = w->text.srch_case_fold;
        bool do_search_hl = (w->text.srch_active && !w->text.srch_is_goto && qlen > 0);
        int lm_cols[16]; int lm_cnt = 0;
        if (do_search_hl) {
            uint64_t lp = pos; int lc = 0;
            const char *q2 = w->text.srch_buf;
            while (lp < sz && d[lp] != '\n' && lm_cnt < 16) {
                if (lp + (uint64_t)qlen <= sz) {
                    bool hit = true;
                    for (int qi = 0; qi < qlen; qi++)
                        if (!srch_ceq((unsigned char)d[lp+(uint64_t)qi], (unsigned char)q2[qi], fold2))
                            { hit = false; break; }
                    if (hit) {
                        lm_cols[lm_cnt++] = lc;
                        int vis_ms2 = lc - (int)hs;
                        int vis_me2 = (lc + qlen) - (int)hs;
                        if (vis_ms2 < 0) vis_ms2 = 0;
                        if (vis_me2 > (int)max_cols) vis_me2 = (int)max_cols;
                        if (vis_me2 > vis_ms2) {
                            uint64_t hx2 = tx + PAD + (uint64_t)vis_ms2 * fw;
                            uint64_t hw2 = (uint64_t)(vis_me2 - vis_ms2) * fw;
                            uint32_t hbg2 = (cur_line == w->text.srch_match_line &&
                                             lc == w->text.srch_match_col)
                                            ? TV_SRCH_HL_BG : TV_SRCH_HL_DIM;
                            console_fill_rect(hx2, py, hw2, fh, hbg2);
                        }
                    }
                }
                lp++; lc++;
            }
        }
        bool line_has_match = (w->text.srch_active &&
                               w->text.srch_match_line == cur_line &&
                               w->text.srch_match_line >= 0 && qlen > 0);
        int mstart = w->text.srch_match_col;
        int mend   = mstart + qlen;

        uint64_t col = 0;
        unsigned char prev_c = 0;  /* previous non-CR char for escape detection */
        while (pos < sz && d[pos] != '\n'
               && (ww ? (uint64_t)row < max_rows : col < max_cols + hs)) {
            unsigned char c = (unsigned char)d[pos];
            if (c == '\r') { pos++; continue; }   /* skip CRLF carriage return */
            uint32_t cbg = tv_cbg((int)col, line_has_match, mstart, mend,
                                   do_search_hl, lm_cols, lm_cnt, qlen, is_cursor_line);
            if (sel_active && pos >= (uint64_t)sel_lo && pos < (uint64_t)sel_hi) cbg = TV_SEL_BG;
            if (pos == (uint64_t)bm_cur || pos == (uint64_t)bm_match) cbg = TV_BRACKET_HL;

            if (!do_hl) {
                /* Plain rendering */
                if (c >= 32 && c < 127) {
                    if (ww && col >= max_cols) {
                        col = 0; row++;
                        if ((uint64_t)row >= max_rows) break;
                        py = iy + PAD + (uint64_t)row * fh;
                        console_fill_rect(ix, py, gutter_w, fh, gutter_bg);
                    }
                    if (col >= hs)
                        console_render_glyph(tx+PAD+(col-hs)*fw, py, c, COL_FB_TXT, cbg);
                    col++;
                } else if (c == '\t') {
                    uint64_t nxt = (col+4u)&~3u;
                    while (col < nxt && col < max_cols + hs) {
                        if (ww && col >= max_cols) {
                            col = 0; row++;
                            if ((uint64_t)row >= max_rows) break;
                            py = iy + PAD + (uint64_t)row * fh;
                            console_fill_rect(ix, py, gutter_w, fh, gutter_bg);
                            nxt = (col+4u)&~3u;
                        }
                        if (col >= hs) {
                            cbg = tv_cbg((int)col, line_has_match, mstart, mend,
                                          do_search_hl, lm_cols, lm_cnt, qlen, is_cursor_line);
                            if (sel_active && pos >= (uint64_t)sel_lo && pos < (uint64_t)sel_hi) cbg = TV_SEL_BG;
                            console_render_glyph(tx+PAD+(col-hs)*fw, py, ' ', COL_FB_TXT, cbg);
                        }
                        col++;
                    }
                }
                pos++;
                continue;
            }

            /* ── Syntax-aware rendering ── */
            uint32_t color = SYN_NORMAL;

            if (lx == LS_CMT_L || lx == LS_CMT_B) {
                /* Check for block comment end */
                if (lx == LS_CMT_B && c == '*' &&
                    pos+1 < sz && d[pos+1] == '/' && d[pos+1] != '\n') {
                    if (ww && col >= max_cols) {
                        col = 0; row++;
                        if ((uint64_t)row < max_rows) { py = iy + PAD + (uint64_t)row * fh; console_fill_rect(ix, py, gutter_w, fh, gutter_bg); }
                    }
                    if (col >= hs)
                        console_render_glyph(tx+PAD+(col-hs)*fw, py, '*', SYN_COMMENT, cbg);
                    col++; pos++;
                    if ((ww ? (uint64_t)row < max_rows : col < max_cols + hs) && pos < sz && d[pos] != '\n') {
                        cbg = tv_cbg((int)col, line_has_match, mstart, mend,
                                      do_search_hl, lm_cols, lm_cnt, qlen, is_cursor_line);
                        if (sel_active && pos >= (uint64_t)sel_lo && pos < (uint64_t)sel_hi) cbg = TV_SEL_BG;
                        if (col >= hs)
                            console_render_glyph(tx+PAD+(col-hs)*fw, py, '/', SYN_COMMENT, cbg);
                        col++; pos++;
                    }
                    lx = LS_NORM;
                    continue;
                }
                color = SYN_COMMENT;
            } else if (lx == LS_STR) {
                color = SYN_STRING;
                if (c == '"' && prev_c != '\\') lx = LS_NORM;
            } else if (lx == LS_STR_SQ) {
                color = SYN_STRING;
                if (c == '\'' && prev_c != '\\') lx = LS_NORM;
            } else if (lx == LS_STR3) {
                color = SYN_STRING;
                if (c == '"') { if (++tqc >= 3) { lx = LS_NORM; tqc = 0; } }
                else tqc = 0;
            } else if (lx == LS_STR3_SQ) {
                color = SYN_STRING;
                if (c == '\'') { if (++tqcsq >= 3) { lx = LS_NORM; tqcsq = 0; } }
                else tqcsq = 0;
            } else if (lx == LS_CHR) {
                color = SYN_STRING;
                if (c == '\'' && prev_c != '\\') lx = LS_NORM;
            } else if (lx == LS_TMPL) {
                color = SYN_STRING;
                if (c == '`' && prev_c != '\\') lx = LS_NORM;
            } else if (lx == LS_JSON_KEY) {
                color = SYN_KEYWORD;
                if (c == '"' && prev_c != '\\') lx = LS_NORM;
            } else if (lx == LS_TAG) {
                /* HTML tag: tag name (SYN_FUNC) then attributes (SYN_KEYWORD) */
                if (c == '>') {
                    color = SYN_KEYWORD; lx = LS_NORM; tag_name = false;
                } else if (c == '"')  { lx = LS_STR;    color = SYN_STRING; tag_name = false; }
                  else if (c == '\'') { lx = LS_STR_SQ; color = SYN_STRING; tag_name = false; }
                  else if (tag_name && (c == ' ' || c == '\t' || c == '\n' || c == '='))
                    { tag_name = false; color = SYN_NORMAL; }
                  else if (tag_name) color = SYN_FUNC;
                  else               color = SYN_KEYWORD;
            } else if (lx == LS_CMT_HTML) {
                /* HTML comment: --> ends it */
                color = SYN_COMMENT;
                if (c == '-') html_dd2++;
                else if (c == '>' && html_dd2 >= 2) { lx = LS_NORM; html_dd2 = 0; }
                else html_dd2 = 0;
            } else if (lx == LS_PP) {
                color = SYN_PREPROC;
            } else {
                /* NORMAL state: classify next token */

                /* ── Diff/patch: color whole line by first char ── */
                if (lang_diff && line_start) {
                    uint32_t dl_col = SYN_NORMAL;
                    if (c == '+') dl_col = 0x0050c060u;       /* addition: green */
                    else if (c == '-') dl_col = 0x00c05050u;  /* deletion: red */
                    else if (c == '@') dl_col = SYN_KEYWORD;  /* hunk header @@ */
                    else if (c == '\\') dl_col = SYN_COMMENT; /* \ No newline... */
                    if (dl_col != SYN_NORMAL) {
                        uint64_t dp = pos;
                        while (dp < sz && d[dp] != '\n') dp++;
                        int dl = (int)(dp - pos);
                        for (int di = 0; di < dl && (ww ? (uint64_t)row < max_rows : col < max_cols + hs); di++, col++) {
                            if (ww && col >= max_cols) {
                                col = 0; row++;
                                if ((uint64_t)row < max_rows) { py = iy + PAD + (uint64_t)row * fh; console_fill_rect(ix, py, gutter_w, fh, gutter_bg); }
                                else break;
                            }
                            if (col >= hs) {
                                cbg = tv_cbg((int)col, line_has_match, mstart, mend, do_search_hl, lm_cols, lm_cnt, qlen, is_cursor_line);
                                if (sel_active && pos+(uint64_t)di >= (uint64_t)sel_lo && pos+(uint64_t)di < (uint64_t)sel_hi) cbg = TV_SEL_BG;
                                console_render_glyph(tx+PAD+(col-hs)*fw, py, (unsigned char)d[pos+di], dl_col, cbg);
                            }
                        }
                        pos += (uint64_t)dl;
                        line_start = false;
                        continue;
                    }
                }

                /* ── Markdown: line-start constructs ── */
                if (lang_md && line_start) {
                    uint32_t md_col = SYN_NORMAL;
                    /* Headings: # ## ### etc. */
                    if (c == '#') {
                        md_col = SYN_PREPROC;
                    }
                    /* Horizontal rule: --- or *** or ___ */
                    else if ((c == '-' || c == '*' || c == '_') && pos+2 < sz
                             && (unsigned char)d[pos+1] == c && (unsigned char)d[pos+2] == c) {
                        md_col = SYN_COMMENT;
                    }
                    /* Blockquote: > */
                    else if (c == '>') {
                        md_col = SYN_COMMENT;
                    }
                    /* List items: - item, * item, + item, or N. item */
                    else if (c == '-' || c == '*' || c == '+') {
                        if (pos+1 < sz && (d[pos+1] == ' ' || d[pos+1] == '\t'))
                            md_col = SYN_KEYWORD;
                    }
                    if (md_col != SYN_NORMAL) {
                        uint64_t mp = pos;
                        while (mp < sz && d[mp] != '\n') mp++;
                        int ml = (int)(mp - pos);
                        for (int mi = 0; mi < ml && (ww ? (uint64_t)row < max_rows : col < max_cols + hs); mi++, col++) {
                            if (ww && col >= max_cols) {
                                col = 0; row++;
                                if ((uint64_t)row < max_rows) { py = iy + PAD + (uint64_t)row * fh; console_fill_rect(ix, py, gutter_w, fh, gutter_bg); }
                                else break;
                            }
                            if (col >= hs) {
                                cbg = tv_cbg((int)col, line_has_match, mstart, mend, do_search_hl, lm_cols, lm_cnt, qlen, is_cursor_line);
                                if (sel_active && pos+(uint64_t)mi >= (uint64_t)sel_lo && pos+(uint64_t)mi < (uint64_t)sel_hi) cbg = TV_SEL_BG;
                                console_render_glyph(tx+PAD+(col-hs)*fw, py, (unsigned char)d[pos+mi], md_col, cbg);
                            }
                        }
                        pos += (uint64_t)ml;
                        line_start = false;
                        continue;
                    }
                }
                /* ── Markdown inline: code spans `...` ── */
                if (lang_md && c == '`') {
                    lx = LS_TMPL; color = SYN_STRING;  /* backtick span; LS_TMPL exits on '`' */
                }

                if (lang_html && c == '<') {
                    /* Check for HTML comment <!--, DOCTYPE <!, or normal tag */
                    if (pos+3 < sz && (unsigned char)d[pos+1]=='!'
                        && (unsigned char)d[pos+2]=='-' && (unsigned char)d[pos+3]=='-') {
                        lx = LS_CMT_HTML; color = SYN_COMMENT; html_dd2 = 0;
                        /* Consume the <!--  (remaining 3 chars rendered as comment on next iterations) */
                    } else if (pos+1 < sz && (unsigned char)d[pos+1] == '!') {
                        lx = LS_TAG; color = SYN_PREPROC; tag_name = false;  /* <!DOCTYPE */
                    } else {
                        lx = LS_TAG; color = SYN_FUNC; tag_name = true;  /* <tagname */
                    }
                } else if (lang_cmt && c == '/' && pos+1 < sz && d[pos+1] != '\n') {
                    if ((unsigned char)d[pos+1] == '/') { lx = LS_CMT_L; color = SYN_COMMENT; }
                    else if ((unsigned char)d[pos+1] == '*') { lx = LS_CMT_B; color = SYN_COMMENT; }
                } else if (lang_hash_cmt && c == '#') {
                    lx = LS_CMT_L; color = SYN_COMMENT;
                } else if (lang_semi_cmt && c == ';') {
                    lx = LS_CMT_L; color = SYN_COMMENT;
                } else if (hl_lang == SYN_LANG_INI && c == '[' && line_start) {
                    /* INI section header: [section] */
                    uint64_t ip = pos;
                    while (ip < sz && d[ip] != '\n' && d[ip] != ']') ip++;
                    if (ip < sz && d[ip] == ']') ip++;
                    int il = (int)(ip - pos);
                    for (int ii = 0; ii < il && (ww ? (uint64_t)row < max_rows : col < max_cols + hs); ii++, col++) {
                        if (ww && col >= max_cols) {
                            col = 0; row++;
                            if ((uint64_t)row < max_rows) { py = iy + PAD + (uint64_t)row * fh; console_fill_rect(ix, py, gutter_w, fh, gutter_bg); }
                            else break;
                        }
                        if (col >= hs) {
                            cbg = tv_cbg((int)col, line_has_match, mstart, mend, do_search_hl, lm_cols, lm_cnt, qlen, is_cursor_line);
                            if (sel_active && pos+(uint64_t)ii >= (uint64_t)sel_lo && pos+(uint64_t)ii < (uint64_t)sel_hi) cbg = TV_SEL_BG;
                            console_render_glyph(tx+PAD+(col-hs)*fw, py, (unsigned char)d[pos+ii], SYN_PREPROC, cbg);
                        }
                    }
                    pos += (uint64_t)il;
                    line_start = false;
                    continue;
                } else if (lang_dash_cmt && c == '-' && pos+1 < sz && (unsigned char)d[pos+1] == '-') {
                    lx = LS_CMT_L; color = SYN_COMMENT;
                } else if (lang_py && c == '"' && pos+1 < sz && (unsigned char)d[pos+1] == '"' && pos+2 < sz && (unsigned char)d[pos+2] == '"') {
                    lx = LS_STR3; tqc = 0; color = SYN_STRING;
                } else if (c == '"') {
                    /* JSON key detection: scan ahead to closing '"', then check for ':' */
                    if (hl_lang == SYN_LANG_JSON) {
                        uint64_t _qp = pos + 1u;
                        while (_qp < sz && d[_qp] != '\n') {
                            if ((unsigned char)d[_qp] == '\\') { _qp += 2; continue; }
                            if ((unsigned char)d[_qp] == '"')  { _qp++; break; }
                            _qp++;
                        }
                        while (_qp < sz && (d[_qp] == ' ' || d[_qp] == '\t')) _qp++;
                        if (_qp < sz && d[_qp] == ':') {
                            lx = LS_JSON_KEY; color = SYN_KEYWORD;
                        } else {
                            lx = LS_STR; color = SYN_STRING;
                        }
                    } else {
                        lx = LS_STR; color = SYN_STRING;
                    }
                } else if (lang_c && c == '\'') {
                    lx = LS_CHR; color = SYN_STRING;
                } else if (lang_py && c == '\'' && pos+1 < sz && (unsigned char)d[pos+1] == '\'' && pos+2 < sz && (unsigned char)d[pos+2] == '\'') {
                    lx = LS_STR3_SQ; tqcsq = 0; color = SYN_STRING;
                } else if (lang_sq_str && c == '\'') {
                    lx = LS_STR_SQ; color = SYN_STRING;
                } else if (lang_tmpl && c == '`') {
                    lx = LS_TMPL; color = SYN_STRING;
                } else if ((lang_c && line_start) || (hl_lang == SYN_LANG_RUST && c == '#')) {
                    lx = LS_PP; color = SYN_PREPROC;
                } else if (hl_lang == SYN_LANG_TOML && c == '[' && line_start) {
                    /* TOML section header: [section] or [[array]] — scan to end of header */
                    uint64_t hp = pos;
                    while (hp < sz && d[hp] != '\n' && d[hp] != ']') hp++;
                    if (hp < sz && d[hp] == ']') hp++;  /* include closing ] */
                    if (hp < sz && d[hp] == ']') hp++;  /* include second ] for [[array]] */
                    int hl = (int)(hp - pos);
                    for (int hi = 0; hi < hl && (ww ? (uint64_t)row < max_rows : col < max_cols + hs); hi++, col++) {
                        if (ww && col >= max_cols) {
                            col = 0; row++;
                            if ((uint64_t)row < max_rows) { py = iy + PAD + (uint64_t)row * fh; console_fill_rect(ix, py, gutter_w, fh, gutter_bg); }
                            else break;
                        }
                        if (col >= hs) {
                            cbg = tv_cbg((int)col, line_has_match, mstart, mend, do_search_hl, lm_cols, lm_cnt, qlen, is_cursor_line);
                            if (sel_active && pos+(uint64_t)hi >= (uint64_t)sel_lo && pos+(uint64_t)hi < (uint64_t)sel_hi) cbg = TV_SEL_BG;
                            console_render_glyph(tx+PAD+(col-hs)*fw, py, (unsigned char)d[pos+hi], SYN_PREPROC, cbg);
                        }
                    }
                    pos += (uint64_t)hl;
                    line_start = false;
                    continue;
                } else if (hl_lang == SYN_LANG_CSS && c == '@') {
                    lx = LS_PP; color = SYN_PREPROC;
                } else if (hl_lang == SYN_LANG_YAML && c == '-' && line_start) {
                    /* YAML list item `- ` or document separator `---` */
                    if (pos+2 < sz && d[pos+1]=='-' && d[pos+2]=='-') {
                        /* `---` or `...` document boundary — scan to end of line */
                        uint64_t yp = pos;
                        while (yp < sz && d[yp] != '\n') yp++;
                        int yl = (int)(yp - pos);
                        for (int yi = 0; yi < yl && (ww ? (uint64_t)row < max_rows : col < max_cols + hs); yi++, col++) {
                            if (ww && col >= max_cols) {
                                col = 0; row++;
                                if ((uint64_t)row < max_rows) { py = iy + PAD + (uint64_t)row * fh; console_fill_rect(ix, py, gutter_w, fh, gutter_bg); }
                                else break;
                            }
                            if (col >= hs) {
                                cbg = tv_cbg((int)col, line_has_match, mstart, mend, do_search_hl, lm_cols, lm_cnt, qlen, is_cursor_line);
                                if (sel_active && pos+(uint64_t)yi >= (uint64_t)sel_lo && pos+(uint64_t)yi < (uint64_t)sel_hi) cbg = TV_SEL_BG;
                                console_render_glyph(tx+PAD+(col-hs)*fw, py, (unsigned char)d[pos+yi], SYN_PREPROC, cbg);
                            }
                        }
                        pos += (uint64_t)yl;
                        line_start = false;
                        continue;
                    } else if (pos+1 < sz && (d[pos+1]==' ' || d[pos+1]=='\n')) {
                        color = SYN_KEYWORD;  /* `- ` list item bullet */
                    }
                } else if (hl_lang == SYN_LANG_SH && c == '$') {
                    /* Shell variable: $word or ${...} — color $ and the word */
                    uint64_t vp = pos + 1u;
                    if (vp < sz && d[vp] == '{') {
                        /* ${...} — scan to closing } */
                        while (vp < sz && d[vp] != '}' && d[vp] != '\n') vp++;
                        if (vp < sz && d[vp] == '}') vp++;
                    } else {
                        while (vp < sz && syn_wch((unsigned char)d[vp])) vp++;
                    }
                    int vl = (int)(vp - pos);
                    if (vl < 1) vl = 1;
                    for (int vi = 0; vi < vl && (ww ? (uint64_t)row < max_rows : col < max_cols + hs); vi++, col++) {
                        if (ww && col >= max_cols) {
                            col = 0; row++;
                            if ((uint64_t)row < max_rows) { py = iy + PAD + (uint64_t)row * fh; console_fill_rect(ix, py, gutter_w, fh, gutter_bg); }
                            else break;
                        }
                        if (col >= hs) {
                            cbg = tv_cbg((int)col, line_has_match, mstart, mend,
                                          do_search_hl, lm_cols, lm_cnt, qlen, is_cursor_line);
                            if (sel_active && pos+(uint64_t)vi >= (uint64_t)sel_lo && pos+(uint64_t)vi < (uint64_t)sel_hi) cbg = TV_SEL_BG;
                            console_render_glyph(tx+PAD+(col-hs)*fw, py, (unsigned char)d[pos+vi], SYN_VAR, cbg);
                        }
                    }
                    pos += (uint64_t)vl;
                    line_start = false;
                    continue;
                } else if (hl_lang == SYN_LANG_CSS && c == '#') {
                    /* CSS hex color: #rgb #rrggbb #rrggbbaa — scan hex digits */
                    uint64_t hp2 = pos + 1u;
                    while (hp2 < sz) {
                        unsigned char _hc = (unsigned char)d[hp2];
                        if ((_hc>='0'&&_hc<='9')||(_hc>='a'&&_hc<='f')||(_hc>='A'&&_hc<='F')) hp2++;
                        else break;
                    }
                    int hl2 = (int)(hp2 - pos);
                    if (hl2 < 1) hl2 = 1;
                    for (int hi2 = 0; hi2 < hl2 && (ww ? (uint64_t)row < max_rows : col < max_cols + hs); hi2++, col++) {
                        if (ww && col >= max_cols) {
                            col = 0; row++;
                            if ((uint64_t)row < max_rows) { py = iy + PAD + (uint64_t)row * fh; console_fill_rect(ix, py, gutter_w, fh, gutter_bg); }
                            else break;
                        }
                        if (col >= hs) {
                            cbg = tv_cbg((int)col, line_has_match, mstart, mend, do_search_hl, lm_cols, lm_cnt, qlen, is_cursor_line);
                            if (sel_active && pos+(uint64_t)hi2 >= (uint64_t)sel_lo && pos+(uint64_t)hi2 < (uint64_t)sel_hi) cbg = TV_SEL_BG;
                            console_render_glyph(tx+PAD+(col-hs)*fw, py, (unsigned char)d[pos+hi2], SYN_NUMBER, cbg);
                        }
                    }
                    pos += (uint64_t)hl2;
                    line_start = false;
                    continue;
                } else if (c >= '0' && c <= '9') {
                    /* Number token: consume digits, 0x hex, 0b binary, decimals, suffixes */
                    uint64_t np = pos + 1u;
                    if (np < sz && (d[pos]=='0') && (d[np]=='x'||d[np]=='X'||d[np]=='b'||d[np]=='B'))
                        np++;  /* skip 0x / 0b prefix */
                    while (np < sz && d[np] != '\n') {
                        unsigned char _nc = (unsigned char)d[np];
                        bool _num = (_nc>='0'&&_nc<='9')||(_nc>='a'&&_nc<='f')||(_nc>='A'&&_nc<='F')
                                    ||_nc=='.'||_nc=='_';
                        if (!_num) break;
                        np++;
                    }
                    /* Consume optional trailing suffix: u/U/l/L/f/F */
                    while (np < sz) {
                        unsigned char _sc = (unsigned char)d[np];
                        if (_sc=='u'||_sc=='U'||_sc=='l'||_sc=='L'||_sc=='f'||_sc=='F') np++; else break;
                    }
                    int nl = (int)(np - pos);
                    for (int ni = 0; ni < nl && (ww ? (uint64_t)row < max_rows : col < max_cols + hs); ni++, col++) {
                        if (ww && col >= max_cols) {
                            col = 0; row++;
                            if ((uint64_t)row < max_rows) { py = iy + PAD + (uint64_t)row * fh; console_fill_rect(ix, py, gutter_w, fh, gutter_bg); }
                            else break;
                        }
                        if (col >= hs) {
                            cbg = tv_cbg((int)col, line_has_match, mstart, mend, do_search_hl, lm_cols, lm_cnt, qlen, is_cursor_line);
                            if (sel_active && pos+(uint64_t)ni >= (uint64_t)sel_lo && pos+(uint64_t)ni < (uint64_t)sel_hi) cbg = TV_SEL_BG;
                            if (pos == (uint64_t)bm_cur || pos == (uint64_t)bm_match) cbg = TV_BRACKET_HL;
                            console_render_glyph(tx+PAD+(col-hs)*fw, py, (unsigned char)d[pos+ni], SYN_NUMBER, cbg);
                        }
                    }
                    pos += (uint64_t)nl;
                    line_start = false;
                    continue;
                } else if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'
                           || (hl_lang == SYN_LANG_CSS && c == '-' && pos+1 < sz
                               && ((unsigned char)d[pos+1]>='a'&&(unsigned char)d[pos+1]<='z'))) {
                    /* Word: look ahead, classify, render whole word.
                     * CSS extends word chars to include '-' for property names. */
                    int wl = 0;
                    uint64_t wp = pos;
                    bool css_word = (hl_lang == SYN_LANG_CSS);
                    while (wp < sz && d[wp] != '\n'
                           && (syn_wch((unsigned char)d[wp]) || (css_word && d[wp] == '-'
                               && wp+1 < sz && syn_wch((unsigned char)d[wp+1]))))
                        { wl++; wp++; }
                    uint32_t wc = syn_word_col(d+pos, wl, hl_lang);
                    /* Function call: identifier immediately followed by '(' → SYN_FUNC */
                    if (wc == SYN_NORMAL && do_hl && wp < sz && d[wp] == '(')
                        wc = SYN_FUNC;
                    /* Makefile target: word at line start followed by ':' (not '::' macro) */
                    if (wc == SYN_NORMAL && hl_lang == SYN_LANG_MAKE && line_start
                        && wp < sz && d[wp] == ':' && (wp+1 >= sz || d[wp+1] != '='))
                        wc = SYN_FUNC;
                    /* Rust macro call: word immediately followed by '!' → SYN_FUNC */
                    if (wc == SYN_NORMAL && hl_lang == SYN_LANG_RUST && wp < sz && d[wp] == '!')
                        wc = SYN_FUNC;
                    /* TOML key: word followed by optional spaces then '=' */
                    if (wc == SYN_NORMAL && hl_lang == SYN_LANG_TOML) {
                        uint64_t _ap = wp;
                        while (_ap < sz && (d[_ap]==' '||d[_ap]=='\t')) _ap++;
                        if (_ap < sz && d[_ap] == '=') wc = SYN_TYPE;
                    }
                    /* YAML key: word followed by optional spaces then ':' (not '://') */
                    if (wc == SYN_NORMAL && hl_lang == SYN_LANG_YAML) {
                        uint64_t _ap = wp;
                        while (_ap < sz && (d[_ap]==' '||d[_ap]=='\t')) _ap++;
                        if (_ap < sz && d[_ap] == ':' && (_ap+1 >= sz || d[_ap+1] != '/'))
                            wc = SYN_KEYWORD;
                    }
                    /* CSS property: word followed by ':' not '::' — color as SYN_TYPE */
                    if (wc == SYN_NORMAL && hl_lang == SYN_LANG_CSS
                        && wp < sz && d[wp] == ':' && (wp+1 >= sz || d[wp+1] != ':'))
                        wc = SYN_TYPE;
                    /* INI key: word (possibly with dots/hyphens) followed by optional spaces then '=' or ':' */
                    if (wc == SYN_NORMAL && hl_lang == SYN_LANG_INI) {
                        uint64_t _ap = wp;
                        while (_ap < sz && (d[_ap]==' '||d[_ap]=='\t')) _ap++;
                        if (_ap < sz && (d[_ap]=='=' || d[_ap]==':')) wc = SYN_KEYWORD;
                    }
                    for (int wi = 0; wi < wl && (ww ? (uint64_t)row < max_rows : col < max_cols + hs); wi++, col++) {
                        if (ww && col >= max_cols) {
                            col = 0; row++;
                            if ((uint64_t)row < max_rows) { py = iy + PAD + (uint64_t)row * fh; console_fill_rect(ix, py, gutter_w, fh, gutter_bg); }
                            else break;
                        }
                        if (col >= hs) {
                            cbg = tv_cbg((int)col, line_has_match, mstart, mend,
                                          do_search_hl, lm_cols, lm_cnt, qlen, is_cursor_line);
                            if (sel_active && pos+(uint64_t)wi >= (uint64_t)sel_lo && pos+(uint64_t)wi < (uint64_t)sel_hi) cbg = TV_SEL_BG;
                            console_render_glyph(tx+PAD+(col-hs)*fw, py, (unsigned char)d[pos+wi],
                                                 wc, cbg);
                        }
                    }
                    pos += (uint64_t)wl;
                    line_start = false;
                    continue;
                }
            }

            /* Render single character */
            if (c == '\t') {
                uint64_t nxt = (col+4u)&~3u;
                while (col < nxt && col < max_cols + hs) {
                    if (ww && col >= max_cols) {
                        col = 0; row++;
                        if ((uint64_t)row >= max_rows) break;
                        py = iy + PAD + (uint64_t)row * fh;
                        console_fill_rect(ix, py, gutter_w, fh, gutter_bg);
                        nxt = (col+4u)&~3u;
                    }
                    if (col >= hs) {
                        cbg = tv_cbg((int)col, line_has_match, mstart, mend,
                                      do_search_hl, lm_cols, lm_cnt, qlen, is_cursor_line);
                        if (sel_active && pos >= (uint64_t)sel_lo && pos < (uint64_t)sel_hi) cbg = TV_SEL_BG;
                        console_render_glyph(tx+PAD+(col-hs)*fw, py, ' ', color, cbg);
                    }
                    col++;
                }
            } else if (c >= 32 && c < 127) {
                if (ww && col >= max_cols) {
                    col = 0; row++;
                    if ((uint64_t)row < max_rows) {
                        py = iy + PAD + (uint64_t)row * fh;
                        console_fill_rect(ix, py, gutter_w, fh, gutter_bg);
                    }
                }
                if (col >= hs)
                    console_render_glyph(tx+PAD+(col-hs)*fw, py, c, color, cbg);
                col++;
            }
            if (c != ' ' && c != '\t') line_start = false;
            prev_c = c;
            pos++;
        }
        /* Advance past remainder of line */
        while (pos < sz && d[pos] != '\n') pos++;
        if (pos < sz) pos++;
        else if (pos == sz) pos++;
        log_row++;
        row++;
    }

    /* End-of-file tilde markers for rows past end of content */
    while ((uint64_t)row < max_rows) {
        uint64_t py3 = iy + PAD + (uint64_t)row * fh;
        console_fill_rect(ix, py3, gutter_w, fh, gutter_bg);
        console_render_glyph(ix + PAD, py3, '~', 0x00203040u, gutter_bg);
        row++;
    }

    /* Vertical scrollbar / minimap */
    if (w->text.total_lines > (int)max_rows) {
        int _tot_lines = w->text.total_lines > 0 ? w->text.total_lines : 1;

        if (show_mm) {
            /* ── Minimap panel ── */
            uint64_t mm_x = ix + iw - TV_MINIMAP_W;
            console_fill_rect(mm_x, iy, TV_MINIMAP_W, ih_text, 0x00060a10u);
            console_fill_rect(mm_x, iy, 1u, ih_text, 0x00182030u); /* left border */

            /* Single-pass: scan file, paint minimap rows as lines are encountered */
            {
                const char *_md = d2;
                uint64_t _ms = sz > 300000u ? 300000u : sz;
                int _ml = 0;
                bool _lstart = true;
                uint32_t _lc = 0x00101820u;
                for (uint64_t _mi = 0; _mi <= _ms; _mi++) {
                    unsigned char _c = (_mi < _ms) ? (unsigned char)_md[_mi] : '\n';
                    if (_c == '\n') {
                        uint64_t _mmy  = iy + (uint64_t)_ml * ih_text / (uint64_t)_tot_lines;
                        uint64_t _mmy2 = iy + (uint64_t)(_ml + 1) * ih_text / (uint64_t)_tot_lines;
                        if (_mmy2 <= _mmy) _mmy2 = _mmy + 1u;
                        if (_mmy2 > iy + ih_text) _mmy2 = iy + ih_text;
                        if (_mmy < iy + ih_text)
                            console_fill_rect(mm_x + 2u, _mmy, TV_MINIMAP_W - 4u,
                                              _mmy2 - _mmy, _lc);
                        _ml++;
                        _lstart = true;
                        _lc = 0x00101820u;
                        if (_ml >= _tot_lines) break;
                    } else if (_lstart && _c != ' ' && _c != '\t') {
                        _lstart = false;
                        if (_c == '/' || _c == '#' || _c == ';')       _lc = 0x00183828u; /* comment */
                        else if (_c == '"' || _c == '\'' || _c == '`') _lc = 0x00382818u; /* string  */
                        else if (_c == '{' || _c == '(' || _c == '<')  _lc = 0x00142230u; /* bracket */
                        else if (_c >= '0' && _c <= '9')                _lc = 0x00301818u; /* number  */
                        else if (_c >= 'A' && _c <= 'Z')                _lc = 0x00182838u; /* upper   */
                        else                                             _lc = 0x00141e2au; /* normal  */
                    }
                }
            }

            /* Search match ticks on minimap */
            if (!w->text.srch_is_goto && w->text.srch_active && w->text.srch_len > 0) {
                const char *_sd = (em ? (const char *)w->text.edit_buf : (const char *)w->text.data);
                uint64_t    _ss = em ? (uint64_t)w->text.edit_size : w->text.size;
                const char *_sq = w->text.srch_buf;
                int         _sl = w->text.srch_len;
                bool        _cf = w->text.srch_case_fold;
                int _ticks = 0;
                int _ln2 = 0;
                uint64_t _pi2 = 0;
                while (_sd && _pi2 + (uint64_t)_sl <= _ss && _ticks < 200) {
                    bool _hit = true;
                    for (int _j = 0; _j < _sl; _j++)
                        if (!srch_ceq((unsigned char)_sd[_pi2+(uint64_t)_j],
                                      (unsigned char)_sq[_j], _cf))
                            { _hit = false; break; }
                    if (_hit) {
                        uint64_t _ty = iy + (uint64_t)_ln2 * ih_text / (uint64_t)_tot_lines;
                        bool _is_cur = (_ln2 == w->text.srch_match_line);
                        uint32_t _tc = _is_cur ? 0x0060c0e0u : 0x00204860u;
                        console_fill_rect(mm_x + 1u, _ty, TV_MINIMAP_W - 2u, 2u, _tc);
                        _ticks++;
                        for (int _j = 0; _j < _sl; _j++) {
                            if ((unsigned char)_sd[_pi2] == '\n') _ln2++;
                            _pi2++;
                        }
                    } else {
                        if ((unsigned char)_sd[_pi2] == '\n') _ln2++;
                        _pi2++;
                    }
                }
            }

            /* Viewport indicator */
            {
                uint64_t _vph = (uint64_t)max_rows * ih_text / (uint64_t)_tot_lines;
                if (_vph < 4u) _vph = 4u;
                uint64_t _vpy = iy + (uint64_t)w->text.scroll * ih_text / (uint64_t)_tot_lines;
                if (_vpy + _vph > iy + ih_text) _vpy = iy + ih_text > _vph ? iy + ih_text - _vph : iy;
                /* Tinted viewport fill */
                for (uint64_t _vy = _vpy; _vy < _vpy + _vph && _vy < iy + ih_text; _vy++)
                    console_fill_rect(mm_x + 2u, _vy, TV_MINIMAP_W - 4u, 1u, 0x00182840u);
                /* Border lines of viewport */
                console_fill_rect(mm_x + 1u, _vpy,             TV_MINIMAP_W - 2u, 1u, 0x004070a0u);
                console_fill_rect(mm_x + 1u, _vpy + _vph - 1u, TV_MINIMAP_W - 2u, 1u, 0x004070a0u);
                /* Cursor line in edit mode */
                if (em && w->text.total_lines > 0) {
                    uint64_t _cn_y = iy + (uint64_t)w->text.edit_cur_line * ih_text
                                     / (uint64_t)_tot_lines;
                    console_fill_rect(mm_x + 1u, _cn_y, TV_MINIMAP_W - 2u, 2u, 0x0060b8e8u);
                }
            }
        } else {
            /* ── Plain 8px scrollbar (when minimap is not shown) ── */
            uint64_t sb_x = ix + iw - 8u;
            console_fill_rect(sb_x, iy, 8u, ih_text, 0x000a0e16u);

            /* Search match tick marks */
            if (!w->text.srch_is_goto && w->text.srch_active && w->text.srch_len > 0) {
                const char *_sd = (em ? (const char *)w->text.edit_buf : (const char *)w->text.data);
                uint64_t    _ss = em ? (uint64_t)w->text.edit_size : w->text.size;
                const char *_sq = w->text.srch_buf;
                int         _sl = w->text.srch_len;
                bool        _cf = w->text.srch_case_fold;
                int _ticks = 0;
                int _ln2 = 0;
                uint64_t _pi2 = 0;
                while (_sd && _pi2 + (uint64_t)_sl <= _ss && _ticks < 200) {
                    bool _hit = true;
                    for (int _j = 0; _j < _sl; _j++)
                        if (!srch_ceq((unsigned char)_sd[_pi2+(uint64_t)_j],
                                      (unsigned char)_sq[_j], _cf))
                            { _hit = false; break; }
                    if (_hit) {
                        uint64_t _ty = iy + (uint64_t)_ln2 * ih_text / (uint64_t)_tot_lines;
                        bool _is_cur = (_ln2 == w->text.srch_match_line);
                        uint32_t _tc = _is_cur ? 0x0060c0e0u : 0x00204860u;
                        console_fill_rect(sb_x + 1u, _ty, 6u, 2u, _tc);
                        _ticks++;
                        for (int _j = 0; _j < _sl; _j++) {
                            if ((unsigned char)_sd[_pi2] == '\n') _ln2++;
                            _pi2++;
                        }
                    } else {
                        if ((unsigned char)_sd[_pi2] == '\n') _ln2++;
                        _pi2++;
                    }
                }
            }

            uint64_t thumb_h = (max_rows * ih_text) / (uint64_t)_tot_lines;
            if (thumb_h < 8) thumb_h = 8;
            uint64_t thumb_y = iy + ((uint64_t)w->text.scroll * (ih_text - thumb_h))
                               / (uint64_t)(max_scroll > 0 ? max_scroll : 1);
            {
                int32_t _smx, _smy; bool _slb, _srb;
                mouse_get_state(&_smx, &_smy, &_slb, &_srb);
                bool _sb_drag_active = (g_sb_drag && g_sb_drag_win == (int)(w - g_wins) && !g_sb_drag_horiz);
                bool _sb_hov = !_sb_drag_active &&
                               _smx >= (int32_t)sb_x && _smx < (int32_t)(sb_x + 8u) &&
                               _smy >= (int32_t)iy    && _smy < (int32_t)(iy + ih_text);
                uint32_t _tc = _sb_drag_active ? 0x0058a0d8u : _sb_hov ? 0x00405870u : 0x00304858u;
                console_fill_rect(sb_x + 2u, thumb_y, 4u, thumb_h, _tc);
            }
            if (em && w->text.total_lines > 0) {
                uint64_t cn_y = iy + (uint64_t)w->text.edit_cur_line * ih_text
                                / (uint64_t)_tot_lines;
                console_fill_rect(sb_x + 1u, cn_y, 6u, 2u, 0x0060b8e8u);
            }
        }
    }

    /* Horizontal scrollbar — only in no-wrap mode when content is wider than viewport */
    if (!ww && w->text.max_line_len > (int)max_cols && ih_text > 12u) {
        uint64_t hb_y   = iy + ih_text - 8u;
        uint64_t hb_w   = iw > gutter_w + 8u ? iw - gutter_w - 8u : 1u;
        console_fill_rect(ix + gutter_w, hb_y, hb_w, 8u, 0x000a0e16u);
        int max_hs2 = w->text.max_line_len - (int)max_cols;
        if (max_hs2 < 1) max_hs2 = 1;
        uint64_t thumb_w = (max_cols * hb_w) / (uint64_t)w->text.max_line_len;
        if (thumb_w < 8) thumb_w = 8;
        if (thumb_w > hb_w) thumb_w = hb_w;
        uint64_t thumb_x = ix + gutter_w + ((uint64_t)w->text.h_scroll * (hb_w - thumb_w))
                           / (uint64_t)max_hs2;
        if (thumb_x + thumb_w > ix + gutter_w + hb_w)
            thumb_x = ix + gutter_w + hb_w - thumb_w;
        {
            bool _hb_drag = (g_sb_drag && g_sb_drag_win == (int)(w - g_wins) && g_sb_drag_horiz);
            int32_t _hmx, _hmy; bool _hlb, _hrb;
            mouse_get_state(&_hmx, &_hmy, &_hlb, &_hrb);
            bool _hb_hov = !_hb_drag &&
                           _hmy >= (int32_t)hb_y && _hmy < (int32_t)(hb_y + 8u) &&
                           _hmx >= (int32_t)(ix + gutter_w) && _hmx < (int32_t)(ix + gutter_w + hb_w);
            uint32_t _htc = _hb_drag ? 0x0058a0d8u : _hb_hov ? 0x00405870u : 0x00304858u;
            console_fill_rect(thumb_x, hb_y + 2u, thumb_w, 4u, _htc);
        }
    }

    /* Edit-mode cursor (blinking insertion bar) */
    if (em && !w->text.srch_active && (g_gui_tick / 25u) % 2u == 0u) {
        int vis_row = w->text.edit_cur_line - w->text.scroll;
        int vis_col = w->text.edit_cur_col  - (int)hs;
        if (vis_row >= 0 && vis_row < (int)max_rows &&
            vis_col >= 0 && vis_col < (int)max_cols) {
            uint64_t cpx = tx + PAD + (uint64_t)vis_col * fw;
            uint64_t cpy = iy + PAD + (uint64_t)vis_row * fh;
            console_fill_rect(cpx, cpy, 2u, fh, 0x0090c0e0u);
        }
    }

    /* Text viewer status footer */
    {
        uint64_t sfy  = iy + ih_text;
        uint64_t sfbg = 0x00070b12u;
        uint64_t sfsp = iy + ih_text + tv_status_h;  /* top of search bar */
        (void)sfsp;
        console_fill_rect(ix, sfy, iw, tv_status_h, sfbg);
        console_fill_rect(ix, sfy, iw, 1u, 0x00181f2cu);

        /* Left: "L first/total  N KB" */
        char lfbuf[48];
        char l1[12], l2[12];
        int first = w->text.scroll + 1;
        int total = w->text.total_lines > 0 ? w->text.total_lines : 1;
        gui_itoa(first, l1, 12);
        gui_itoa(total, l2, 12);
        {
            int si2 = 0; const char *p2;
            for (p2="L "; *p2 && si2<44; ) lfbuf[si2++]=*p2++;
            for (p2=l1; *p2 && si2<44; ) lfbuf[si2++]=*p2++;
            for (p2="/"; *p2 && si2<44; ) lfbuf[si2++]=*p2++;
            for (p2=l2; *p2 && si2<44; ) lfbuf[si2++]=*p2++;
            /* append line ending indicator */
            if (!em && w->text.data && w->text.size > 1) {
                bool has_crlf = false;
                const char *_fd = (const char *)w->text.data;
                for (uint64_t _fk = 0; _fk+1 < w->text.size; _fk++) {
                    if (_fd[_fk]=='\r' && _fd[_fk+1]=='\n') { has_crlf = true; break; }
                }
                for (p2 = has_crlf ? "  CRLF" : "  LF"; *p2 && si2<44; ) lfbuf[si2++]=*p2++;
            }
            /* append file size */
            uint64_t fsz = em ? (uint64_t)w->text.edit_size : w->text.size;
            if (fsz > 0) {
                char sn[12]; const char *su;
                if (fsz >= 1024u*1024u) {
                    gui_itoa((int)(fsz>>20), sn, 12); su = " MB";
                } else if (fsz >= 1024u) {
                    gui_itoa((int)(fsz>>10), sn, 12); su = " KB";
                } else {
                    gui_itoa((int)fsz, sn, 12); su = " B";
                }
                for (p2="  "; *p2 && si2<44; ) lfbuf[si2++]=*p2++;
                for (p2=sn; *p2 && si2<44; ) lfbuf[si2++]=*p2++;
                for (p2=su; *p2 && si2<44; ) lfbuf[si2++]=*p2++;
            }
            /* Append word count (edit mode: from edit_buf; read mode: from data) */
            {
                const uint8_t *_wdata = NULL;
                uint32_t _wlim = 0;
                if (em && w->text.edit_buf && w->text.edit_size > 0) {
                    _wdata = w->text.edit_buf;
                    _wlim  = w->text.edit_size > 200000u ? 200000u : w->text.edit_size;
                } else if (!em && w->text.data && w->text.size > 0 && w->text.size <= 200000u) {
                    _wdata = (const uint8_t *)w->text.data;
                    _wlim  = (uint32_t)w->text.size;
                }
                if (_wdata && _wlim > 0 && si2 < 42) {
                    uint32_t _wc = 0; bool _iw = false;
                    for (uint32_t _wi = 0; _wi < _wlim; _wi++) {
                        uint8_t _wb = _wdata[_wi];
                        bool _ws = (_wb==' '||_wb=='\t'||_wb=='\n'||_wb=='\r');
                        if (!_ws && !_iw) { _wc++; _iw = true; }
                        else if (_ws) _iw = false;
                    }
                    char _wcn[12]; gui_itoa((int)_wc, _wcn, 12);
                    for (p2="  "; *p2 && si2<44; ) lfbuf[si2++]=*p2++;
                    for (p2=_wcn; *p2 && si2<44; ) lfbuf[si2++]=*p2++;
                    if (si2 < 45) lfbuf[si2++]='w';
                }
            }
            lfbuf[si2]='\0';
        }
        gui_draw_str(ix + gutter_w + PAD, sfy + (tv_status_h - fh)/2u,
                     lfbuf, 0x00405060u, sfbg);

        /* Centre: edit indicator OR language indicator */
        if (em) {
            /* Show [EDIT] or [EDIT*] (modified) */
            const char *etag = w->text.edit_modified ? "[EDIT*]" : "[EDIT]";
            uint64_t et_len = (uint64_t)gui_strlen(etag);
            uint64_t et_x   = ix + (iw - et_len * fw) / 2u;
            gui_draw_str(et_x, sfy + (tv_status_h - fh)/2u, etag, 0x0080c8a0u, sfbg);
        } else {
            const char *lang_tag = (hl_lang == SYN_LANG_C)    ? "C"
                                 : (hl_lang == SYN_LANG_SH)   ? "sh"
                                 : (hl_lang == SYN_LANG_PY)   ? "py"
                                 : (hl_lang == SYN_LANG_ASM)  ? "asm"
                                 : (hl_lang == SYN_LANG_JSON)  ? "json"
                                 : (hl_lang == SYN_LANG_LUA)   ? "lua"
                                 : (hl_lang == SYN_LANG_JS)    ? "js"
                                 : (hl_lang == SYN_LANG_MAKE)  ? "make"
                                 : (hl_lang == SYN_LANG_TOML)  ? "toml"
                                 : (hl_lang == SYN_LANG_YAML)  ? "yaml"
                                 : (hl_lang == SYN_LANG_HTML)  ? "html"
                                 : (hl_lang == SYN_LANG_CSS)   ? "css"
                                 : (hl_lang == SYN_LANG_INI)   ? "ini"
                                 : (hl_lang == SYN_LANG_MD)    ? "md"
                                 : (hl_lang == SYN_LANG_DIFF)  ? "diff"
                                 : (hl_lang == SYN_LANG_SQL)   ? "sql"
                                 : (hl_lang == SYN_LANG_RUST)  ? "rs" : NULL;
            if (lang_tag) {
                uint64_t lt_len = (uint64_t)gui_strlen(lang_tag);
                uint64_t lt_x   = ix + (iw - lt_len * fw) / 2u;
                gui_draw_str(lt_x, sfy + (tv_status_h - fh)/2u, lang_tag, 0x00284878u, sfbg);
            }
        }

        /* Right side: edit cursor pos > selection size > search count > wrap > h_scroll */
        if (em) {
            /* Show selection byte count when active */
            if (w->text.sel_anchor >= 0) {
                int32_t slo, shi; edit_sel_range(&w->text, &slo, &shi);
                int32_t slen = shi - slo;
                if (slen > 0) {
                    char selbuf[20]; char seln[12];
                    int si2 = 0; const char *p2;
                    gui_itoa((int)slen, seln, 12);
                    for (p2=seln; *p2 && si2<16; ) selbuf[si2++]=*p2++;
                    for (p2=" sel"; *p2 && si2<19; ) selbuf[si2++]=*p2++;
                    selbuf[si2]='\0';
                    uint64_t sel_x = ix + iw - (uint64_t)si2 * fw - 10u;
                    gui_draw_str(sel_x, sfy + (tv_status_h - fh)/2u, selbuf, 0x0060a0d8u, sfbg);
                }
            } else {
                char ebuf[20]; int si2 = 0; const char *p2;
                char ln[8], co[8];
                gui_itoa(w->text.edit_cur_line + 1, ln, 8);
                gui_itoa(w->text.edit_cur_col  + 1, co, 8);
                for (p2=ln; *p2 && si2<16; ) ebuf[si2++]=*p2++;
                ebuf[si2++]=':';
                for (p2=co; *p2 && si2<18; ) ebuf[si2++]=*p2++;
                ebuf[si2]='\0';
                uint64_t ex2 = ix + iw - (uint64_t)si2 * fw - 10u;
                gui_draw_str(ex2, sfy + (tv_status_h - fh)/2u, ebuf, 0x00506878u, sfbg);
            }
        } else if (!em && w->text.sel_anchor >= 0) {
            /* Read mode: show selection byte count */
            int32_t slo2, shi2; edit_sel_range(&w->text, &slo2, &shi2);
            int32_t slen2 = shi2 - slo2;
            if (slen2 > 0) {
                char selbuf2[20]; char seln2[12];
                int si2 = 0; const char *p2;
                gui_itoa((int)slen2, seln2, 12);
                for (p2=seln2; *p2 && si2<16; ) selbuf2[si2++]=*p2++;
                for (p2=" sel"; *p2 && si2<19; ) selbuf2[si2++]=*p2++;
                selbuf2[si2]='\0';
                uint64_t sel_x2 = ix + iw - (uint64_t)si2 * fw - 10u;
                gui_draw_str(sel_x2, sfy + (tv_status_h - fh)/2u, selbuf2, 0x0060a0d8u, sfbg);
            }
        } else if (w->text.srch_active && !w->text.srch_is_goto && w->text.srch_len > 0) {
            /* Show "N / M" (current match / total) or "no match" */
            char mbuf[24]; int si2 = 0; const char *p2;
            if (w->text.srch_total_count == 0) {
                for (p2 = "no match"; *p2 && si2 < 22; ) mbuf[si2++] = *p2++;
            } else {
                char n1[8], n2[8];
                gui_itoa(w->text.srch_cur_idx, n1, 8);
                gui_itoa(w->text.srch_total_count, n2, 8);
                for (p2 = n1; *p2 && si2 < 22; ) mbuf[si2++] = *p2++;
                for (p2 = " / "; *p2 && si2 < 22; ) mbuf[si2++] = *p2++;
                for (p2 = n2;  *p2 && si2 < 22; ) mbuf[si2++] = *p2++;
            }
            mbuf[si2] = '\0';
            uint32_t mc = (w->text.srch_total_count == 0) ? 0x00705050u : 0x003898c8u;
            uint64_t mx2 = ix + iw - (uint64_t)si2 * fw - 10u;
            gui_draw_str(mx2, sfy + (tv_status_h - fh)/2u, mbuf, mc, sfbg);
        } else if (w->text.word_wrap) {
            const char *ww_str = "WRAP";
            uint64_t ww_len = (uint64_t)gui_strlen(ww_str);
            uint64_t ww_x   = ix + iw - ww_len * fw - 10u;
            gui_draw_str(ww_x, sfy + (tv_status_h - fh)/2u,
                         ww_str, 0x00386858u, sfbg);
        } else if (w->text.h_scroll > 0) {
            char hsbuf[16]; char hsn[12];
            int si2 = 0; const char *p2;
            gui_itoa(w->text.h_scroll, hsn, 12);
            for (p2="col +"; *p2 && si2<14; ) hsbuf[si2++]=*p2++;
            for (p2=hsn;    *p2 && si2<14; ) hsbuf[si2++]=*p2++;
            hsbuf[si2]='\0';
            uint64_t hs_len = (uint64_t)gui_strlen(hsbuf);
            uint64_t hs_x   = ix + iw - hs_len * fw - 10u;
            gui_draw_str(hs_x, sfy + (tv_status_h - fh)/2u,
                         hsbuf, 0x00385870u, sfbg);
        } else if (!em && w->text.total_lines > 1) {
            /* Read mode: show scroll percentage */
            int _tot = w->text.total_lines;
            int _pct = (w->text.scroll * 100) / (_tot > 1 ? _tot - 1 : 1);
            if (_pct < 0) _pct = 0; if (_pct > 100) _pct = 100;
            char pbuf[8]; char pn[8];
            int si2 = 0; const char *p2;
            gui_itoa(_pct, pn, 8);
            for (p2=pn; *p2 && si2<6; ) pbuf[si2++]=*p2++;
            pbuf[si2++]='%'; pbuf[si2]='\0';
            uint64_t p_len = (uint64_t)si2;
            uint64_t p_x   = ix + iw - p_len * fw - 10u;
            gui_draw_str(p_x, sfy + (tv_status_h - fh)/2u, pbuf, 0x00304858u, sfbg);
        }
    }

    /* Search / goto / replace bar */
    if (w->text.srch_active) {
        uint64_t bar_y = iy + ih_text + tv_status_h;
        uint64_t row_h = fh + 8u;
        uint64_t total_bar_h = w->text.srch_is_repl ? 2u * row_h : row_h;
        console_fill_rect(ix, bar_y, iw, total_bar_h, TV_SRCH_BG);
        bool _no_match = (!w->text.srch_is_goto && w->text.srch_len > 0
                          && w->text.srch_total_count == 0);
        console_fill_rect(ix, bar_y, iw, 1u, _no_match ? 0x00803030u : TV_SRCH_BORDER);
        if (_no_match)
            console_fill_rect(ix, bar_y + 1u, 3u, total_bar_h - 1u, 0x00803030u);

        /* ── Find row ── */
        bool find_focused = !w->text.srch_is_repl || !w->text.repl_focused;
        uint64_t px2 = ix + PAD;
        uint64_t py2 = bar_y + 4u;
        if (w->text.srch_is_goto) {
            const char *gp = "Line:";
            for (int k = 0; gp[k]; k++, px2 += fw)
                console_render_glyph(px2, py2, (unsigned char)gp[k], TV_SRCH_PROMPT, TV_SRCH_BG);
            px2 += fw;
        } else if (w->text.srch_is_repl) {
            const char *fp = "Find: ";
            for (int k = 0; fp[k]; k++, px2 += fw)
                console_render_glyph(px2, py2, (unsigned char)fp[k], TV_SRCH_PROMPT, TV_SRCH_BG);
        } else {
            console_render_glyph(px2, py2, '/', TV_SRCH_PROMPT, TV_SRCH_BG); px2 += fw;
            console_render_glyph(px2, py2, ' ', TV_SRCH_PROMPT, TV_SRCH_BG); px2 += fw;
        }
        for (int i = 0; i < w->text.srch_len; i++, px2 += fw)
            console_render_glyph(px2, py2, (unsigned char)w->text.srch_buf[i],
                                 TV_SRCH_TXT, TV_SRCH_BG);
        if (find_focused && (g_gui_tick / 25u) % 2u == 0u)
            console_fill_rect(px2, py2 + fh - 2u, fw > 2u ? fw - 2u : 1u, 2u, TV_SRCH_TXT);
        if (!w->text.srch_is_goto && w->text.srch_len > 0 && w->text.srch_match_line < 0) {
            const char *nm = "  (no match)";
            for (int i = 0; nm[i]; i++, px2 += fw)
                console_render_glyph(px2, py2, (unsigned char)nm[i], 0x00806060u, TV_SRCH_BG);
        }
        /* Case-fold indicator: "[Aa]" right-aligned, lights up when active */
        if (!w->text.srch_is_goto) {
            bool cf = w->text.srch_case_fold;
            uint32_t ci_fg = cf ? 0x0090e0b0u : 0x00304050u;
            uint32_t ci_bg = cf ? 0x00143020u : TV_SRCH_BG;
            const char *ci = "[Aa]";
            uint64_t ci_x = ix + iw - 4u * fw - PAD;
            for (int k = 0; ci[k]; k++, ci_x += fw)
                console_render_glyph(ci_x, py2, (unsigned char)ci[k], ci_fg, ci_bg);
        }

        /* ── Replace row (find+replace mode only) ── */
        if (w->text.srch_is_repl) {
            uint64_t rbar_y = bar_y + row_h;
            console_fill_rect(ix, rbar_y, iw, 1u, TV_SRCH_BORDER);
            uint64_t rpx = ix + PAD;
            uint64_t rpy = rbar_y + 4u;
            const char *rp = "Repl: ";
            for (int k = 0; rp[k]; k++, rpx += fw)
                console_render_glyph(rpx, rpy, (unsigned char)rp[k], TV_SRCH_PROMPT, TV_SRCH_BG);
            for (int i = 0; i < w->text.repl_len; i++, rpx += fw)
                console_render_glyph(rpx, rpy, (unsigned char)w->text.repl_buf[i],
                                     TV_SRCH_TXT, TV_SRCH_BG);
            if (w->text.repl_focused && (g_gui_tick / 25u) % 2u == 0u)
                console_fill_rect(rpx, rpy + fh - 2u, fw > 2u ? fw - 2u : 1u, 2u, TV_SRCH_TXT);
            /* Hint */
            const char *hint = "  Enter=replace  Ctrl+A=all  Tab=switch";
            for (int k = 0; hint[k]; k++, rpx += fw)
                console_render_glyph(rpx, rpy, (unsigned char)hint[k], 0x00304050u, TV_SRCH_BG);
        }
    }

    /* Save-as bar — shown below status when editing a new (path-less) file */
    if (w->text.save_as_active) {
        uint64_t bar_y = iy + ih_text + tv_status_h;
        uint64_t row_h2 = fh + 8u;
        console_fill_rect(ix, bar_y, iw, row_h2, TV_SRCH_BG);
        console_fill_rect(ix, bar_y, iw, 1u, 0x005888c0u);
        uint64_t spx = ix + PAD;
        uint64_t spy = bar_y + 4u;
        const char *sap = "Save as: ";
        for (int k = 0; sap[k]; k++, spx += fw)
            console_render_glyph(spx, spy, (unsigned char)sap[k], TV_SRCH_PROMPT, TV_SRCH_BG);
        for (int i = 0; i < w->text.save_as_len; i++, spx += fw)
            console_render_glyph(spx, spy, (unsigned char)w->text.save_as_buf[i], TV_SRCH_TXT, TV_SRCH_BG);
        if ((g_gui_tick / 25u) % 2u == 0u)
            console_fill_rect(spx, spy + fh - 2u, fw > 2u ? fw - 2u : 1u, 2u, TV_SRCH_TXT);
        const char *sh = "  Enter=save  Esc=cancel";
        for (int k = 0; sh[k]; k++, spx += fw)
            console_render_glyph(spx, spy, (unsigned char)sh[k], 0x00304050u, TV_SRCH_BG);
    }

    /* Open-by-path bar (Ctrl+O) */
    if (w->text.open_bar_active) {
        uint64_t bar_y = iy + ih_text + tv_status_h;
        uint64_t row_h2 = fh + 8u;
        console_fill_rect(ix, bar_y, iw, row_h2, TV_SRCH_BG);
        console_fill_rect(ix, bar_y, iw, 1u, 0x0050a070u);
        uint64_t spx = ix + PAD;
        uint64_t spy = bar_y + 4u;
        const char *oap = "Open: ";
        for (int k = 0; oap[k]; k++, spx += fw)
            console_render_glyph(spx, spy, (unsigned char)oap[k], TV_SRCH_PROMPT, TV_SRCH_BG);
        for (int i = 0; i < w->text.open_bar_len; i++, spx += fw)
            console_render_glyph(spx, spy, (unsigned char)w->text.open_bar_buf[i], TV_SRCH_TXT, TV_SRCH_BG);
        if ((g_gui_tick / 25u) % 2u == 0u)
            console_fill_rect(spx, spy + fh - 2u, fw > 2u ? fw - 2u : 1u, 2u, TV_SRCH_TXT);
        const char *oh = "  Enter=open  Tab=complete  Esc=cancel";
        for (int k = 0; oh[k]; k++, spx += fw)
            console_render_glyph(spx, spy, (unsigned char)oh[k], 0x00304050u, TV_SRCH_BG);
    }
}
