#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "gui.h"
#include "console.h"
#include "mouse.h"
#include "heap.h"
#include "vfs.h"
#include "keyboard.h"
#include "pit.h"
#include "pmm.h"
/* ── Layout ──────────────────────────────────────────────────────────── */
#define STATUS_H        20u
#define TASKBAR_H       32u
#define TITLE_H         24u
#define BTN_W           TITLE_H
#define BORDER          1u
#define PAD             4u
#define LOGO_X          4u
#define LOGO_W          72u
#define TASKBTN_X       84u
#define TASKBTN_W       120u
#define TASKBTN_GAP     4u
#define RESIZE_MARGIN   6u
#define MIN_WIN_W       300u
#define MIN_WIN_H       180u
#define SNAP_DIST       14u
#define LAUNCHER_ITEM_H 26u
#define LAUNCHER_ITEMS  3u
#define LAUNCHER_W      100u
#define CTX_W           110u
#define CTX_ITEM_H      22u
#define CTX_ITEMS       3u

/* File browser layout */
#define FB_TOOLBAR_H    ((console_font_height() + 10u) * 2u)  /* 2 rows */
#define FB_ROW1_H       (console_font_height() + 10u)
#define FB_STATUSBAR_H  (console_font_height() + 8u)
#define FB_SIDEBAR_W    100u
#define FB_BTN_W        26u
#define FB_ROW_H        (console_font_height() + 6u)
#define FB_ICON_COLS    3u   /* chars per icon "[/]" */

/* ── Colours ─────────────────────────────────────────────────────────── */
#define COL_DESKTOP      0x001a1a2eu
#define COL_BORDER       0x003060c0u
#define COL_TITLE_FG     0x00e8eeffu
#define COL_WIN_BG       0x00101010u
#define COL_CLOSE        0x00993333u
#define COL_BTN_BG       0x00304860u
#define COL_BTN_FG       0x00a8c0e8u
#define COL_TASKBAR      0x00101018u
#define COL_TASKBAR_SEP  0x003060c0u
#define COL_TASKBTN      0x00202838u
#define COL_TASKBTN_A    0x003060c0u
#define COL_TASKBTN_FG   0x00c8d4f0u
#define COL_LOGO         0x002244aau
#define COL_LAUNCH_BG    0x00202838u
#define COL_LAUNCH_HL    0x003060c0u
#define COL_LAUNCH_FG    0x00c8d4f0u

/* File browser colours */
#define COL_FB_TOOLBAR   0x00101828u
#define COL_FB_BTN       0x00243448u
#define COL_FB_BTN_ACT   0x003060c0u
#define COL_FB_BTN_DIS   0x00181c24u
#define COL_FB_BTN_FG    0x00a8c0e8u
#define COL_FB_BTN_DIS_FG 0x00404858u
#define COL_FB_PATH_BG   0x000e1420u
#define COL_FB_PATH_FG   0x0090a8c8u
#define COL_FB_SIDEBAR   0x00131820u
#define COL_FB_SB_FG     0x0078909cu
#define COL_FB_SB_SEL    0x001c2c44u
#define COL_FB_SB_SEL_FG 0x0090c0f0u
#define COL_FB_LIST_BG   0x000c1018u
#define COL_FB_LIST_ALT  0x000f141cu
#define COL_FB_HOV       0x001a2840u
#define COL_FB_SEL       0x002050a0u
#define COL_FB_DIR       0x005898e8u
#define COL_FB_TXT       0x00c0d0e0u
#define COL_FB_CODE      0x0078d890u
#define COL_FB_SCRIPT    0x00e8c060u
#define COL_FB_BIN       0x00a090c8u
#define COL_FB_IMG       0x00e08060u
#define COL_FB_MUTED     0x00506070u
#define COL_FB_STATUSBAR  0x000a0e16u
#define COL_FB_STATUS_FG  0x00607080u
#define COL_FB_SEP        0x00202838u
#define COL_FB_SEARCH_BG  0x000e1420u
#define COL_FB_SEARCH_FG  0x00d0e0f0u
#define COL_FB_SEARCH_ACT 0x00001830u
#define COL_FB_SEARCH_CUR 0x004488ccu
#define COL_FB_MATCH_HL   0x00183060u

/* ── Types ───────────────────────────────────────────────────────────── */
typedef enum { WIN_HIDDEN, WIN_NORMAL, WIN_MAXIMIZED } win_state_t;
typedef enum { WIN_TERM, WIN_FILES, WIN_TEXT, WIN_SETTINGS } win_type_t;

typedef enum {
    RES_NONE,
    RES_N, RES_S, RES_E, RES_W,
    RES_NE, RES_NW, RES_SE, RES_SW
} resize_dir_t;

/* Text viewer */
#define TV_PATH_MAX  128

typedef struct {
    char       path[TV_PATH_MAX];
    char       title_buf[64];
    const void *data;
    uint64_t   size;
    int        scroll;
    int        total_lines;
    int32_t    scroll_vel;
    int32_t    scroll_acc;
} text_state_t;

#define FB_MAX_ENTRIES  96
#define FB_HIST_MAX      8
#define FB_SEARCH_MAX   64

typedef struct {
    char path[128];
    char entries[FB_MAX_ENTRIES][128];
    bool is_dir[FB_MAX_ENTRIES];
    int  entry_count;
    int  scroll;
    int  hover_row;   /* -1 = none */
    int  sel_row;     /* -1 = none */
    /* navigation history */
    char hist[FB_HIST_MAX][128];
    int  hist_depth;
    /* search */
    char search_query[FB_SEARCH_MAX];
    int  search_len;
    bool search_active;
    /* inertial scroll */
    int32_t scroll_vel; /* fp16 velocity (1/16 lines per tick) */
    int32_t scroll_acc; /* sub-line accumulator */
} fb_state_t;

typedef struct {
    bool        active;
    bool        half_snapped;
    win_state_t state;
    win_type_t  type;
    const char *title;
    uint64_t    x, y, w, h;
    uint64_t    saved_x, saved_y, saved_w, saved_h;
    uint64_t    btn_min_x, btn_max_x, btn_cls_x;
    fb_state_t   fb;
    text_state_t text;
} window_t;

#define MAX_WINS 4
static window_t g_wins[MAX_WINS];

/* ── Z-order ──────────────────────────────────────────────────────────── */
static int g_z[MAX_WINS];  /* g_z[0]=bottom, g_z[MAX_WINS-1]=top */

static void z_raise(int slot) {
    /* Find slot in g_z, remove it, shift others down, place at top */
    int pos = -1;
    for (int i = 0; i < MAX_WINS; i++) {
        if (g_z[i] == slot) { pos = i; break; }
    }
    if (pos < 0) return;
    for (int i = pos; i < MAX_WINS - 1; i++)
        g_z[i] = g_z[i + 1];
    g_z[MAX_WINS - 1] = slot;
}

/* ── Drag / resize state ─────────────────────────────────────────────── */
static bool      g_dragging    = false;
static int       g_drag_win    = -1;
static int32_t   g_drag_off_x  = 0;
static int32_t   g_drag_off_y  = 0;
static bool      g_prev_lbtn   = false;
static int       g_snap_preview = 0; /* 0=none, 1=left-half, 2=right-half */

/* Scrollbar drag state */
static bool     g_sb_drag      = false;
static int      g_sb_drag_win  = -1;
static int32_t  g_sb_drag_y0   = 0;
static int      g_sb_drag_s0   = 0;
static uint64_t g_sb_drag_lh   = 0;  /* track height in pixels */
static uint64_t g_sb_drag_ly   = 0;  /* track top y */
static int      g_sb_drag_max  = 0;  /* max scroll value */
static bool     g_sb_drag_text = false; /* true=text viewer, false=files */

static bool         g_resizing   = false;
static int          g_resize_win = -1;
static resize_dir_t g_resize_dir = RES_NONE;
static int32_t      g_resize_ox  = 0;
static int32_t      g_resize_oy  = 0;
static uint64_t     g_resize_wx0 = 0;
static uint64_t     g_resize_wy0 = 0;
static uint64_t     g_resize_ww0 = 0;
static uint64_t     g_resize_wh0 = 0;

static bool g_launcher_open = false;

/* ── Chrome hover state ──────────────────────────────────────────────── */
static int g_chrome_win = -1;
static int g_chrome_btn = 0;  /* 0=none, 1=close, 2=max, 3=min */

/* ── Double-click state ──────────────────────────────────────────────── */
static uint64_t g_last_click_tick = 0;
static int      g_last_click_win  = -1;

/* ── Context menu state ──────────────────────────────────────────────── */
static bool    g_ctx_open = false;
static int32_t g_ctx_x = 0;
static int32_t g_ctx_y = 0;

/* ── Launcher hover ──────────────────────────────────────────────────── */
static int g_launcher_hover = -1;

/* ── Taskbar button hover (-1=none, 0=Terminal, 1=Files) ─────────────── */
static int g_taskbtn_hover = -1;

/* ── Context menu hover (-1=none, 0..CTX_ITEMS-1) ───────────────────── */
static int g_ctx_hover = -1;

/* ── Resize edge hover ───────────────────────────────────────────────── */
static int          g_resize_hover_win = -1;
static resize_dir_t g_resize_hover_dir = RES_NONE;

/* ── GUI tick counter ────────────────────────────────────────────────── */
static uint64_t g_gui_tick = 0;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static uint64_t desk_top(void)   { return STATUS_H; }
static uint64_t desk_bot(void)   { return console_fb_height() - TASKBAR_H; }
static uint64_t desk_avail(void) { return desk_bot() - desk_top(); }

static size_t gui_strlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}

static bool gui_streq(const char *a, const char *b) {
    while (*a && *b) if (*a++ != *b++) return false;
    return *a == *b;
}

static void gui_draw_str(uint64_t px, uint64_t py, const char *s,
                         uint32_t fg, uint32_t bg) {
    uint64_t fw = console_font_width();
    for (size_t i = 0; s[i]; i++)
        console_render_glyph(px + (uint64_t)i * fw, py,
                             (unsigned char)s[i], fg, bg);
}

/* Draw str clipped to max_chars wide */
static void gui_draw_str_clip(uint64_t px, uint64_t py, const char *s,
                               uint32_t fg, uint32_t bg, uint64_t max_chars) {
    uint64_t fw = console_font_width();
    size_t len = gui_strlen(s);
    if (len <= max_chars) {
        gui_draw_str(px, py, s, fg, bg);
    } else if (max_chars >= 3) {
        /* truncate with "..." */
        for (size_t i = 0; i < max_chars - 3; i++)
            console_render_glyph(px + (uint64_t)i * fw, py, (unsigned char)s[i], fg, bg);
        for (size_t i = 0; i < 3; i++)
            console_render_glyph(px + (uint64_t)(max_chars-3+i) * fw, py, '.', COL_FB_MUTED, bg);
    }
}

/* Simple int-to-string, returns pointer into buf */
static char *gui_itoa(int n, char *buf, int bufsz) {
    if (bufsz < 2) { buf[0]='\0'; return buf; }
    if (n == 0) { buf[0]='0'; buf[1]='\0'; return buf; }
    int i = 0;
    if (n < 0) { buf[i++] = '-'; n = -n; }
    char tmp[16]; int j = 0;
    while (n > 0 && j < 15) { tmp[j++] = '0' + (n % 10); n /= 10; }
    while (j > 0 && i < bufsz-1) buf[i++] = tmp[--j];
    buf[i] = '\0';
    return buf;
}

/* Zero-pad 2-digit int into out[0..2] (null-terminated) */
static void gui_itoa_pad2(uint64_t n, char *out) {
    n %= 100;
    out[0] = '0' + (int)(n / 10);
    out[1] = '0' + (int)(n % 10);
    out[2] = '\0';
}

/* ── Taskbar ─────────────────────────────────────────────────────────── */

static void taskbar_draw_btn(int slot, const char *label) {
    uint64_t fb_h = console_fb_height();
    uint64_t fw   = console_font_width();
    uint64_t fh   = console_font_height();
    uint64_t ty   = fb_h - TASKBAR_H;
    uint64_t bx   = TASKBTN_X + (uint64_t)slot * (TASKBTN_W + TASKBTN_GAP);
    bool     vis  = (slot < MAX_WINS && g_wins[slot].active &&
                     g_wins[slot].state != WIN_HIDDEN);
    bool     hov  = (g_taskbtn_hover == slot);
    uint32_t bg   = vis  ? COL_TASKBTN_A :
                    hov  ? 0x00283848u : COL_TASKBTN;

    console_fill_rect(bx, ty + 3u, TASKBTN_W, TASKBAR_H - 6u, bg);
    /* Active indicator bar at bottom */
    if (vis)
        console_fill_rect(bx, ty + TASKBAR_H - 5u, TASKBTN_W, 3u, COL_BORDER);
    uint64_t llen = (uint64_t)gui_strlen(label);
    uint64_t lpx  = bx + (TASKBTN_W - llen * fw) / 2u;
    uint64_t lpy  = ty + (TASKBAR_H - fh) / 2u;
    gui_draw_str(lpx, lpy, label, COL_TASKBTN_FG, bg);
}

static void taskbar_draw(void) {
    uint64_t fb_w = console_fb_width();
    uint64_t fb_h = console_fb_height();
    uint64_t fw   = console_font_width();
    uint64_t fh   = console_font_height();
    uint64_t ty   = fb_h - TASKBAR_H;

    console_fill_rect(0, ty, fb_w, TASKBAR_H, COL_TASKBAR);
    console_fill_rect(0, ty, fb_w, 2u, COL_TASKBAR_SEP);

    uint32_t logo_bg = g_launcher_open ? COL_TASKBTN_A : COL_LOGO;
    console_fill_rect(LOGO_X, ty + 4u, LOGO_W, TASKBAR_H - 8u, logo_bg);
    const char *logo = "FiFi OS";
    uint64_t llen = (uint64_t)gui_strlen(logo);
    uint64_t lpx  = LOGO_X + (LOGO_W - llen * fw) / 2u;
    uint64_t lpy  = ty + (TASKBAR_H - fh) / 2u;
    gui_draw_str(lpx, lpy, logo, COL_TASKBTN_FG, logo_bg);

    taskbar_draw_btn(0, "Terminal");
    taskbar_draw_btn(1, "Files");
    taskbar_draw_btn(2, "Settings");
}

/* ── Launcher popup ──────────────────────────────────────────────────── */

static uint64_t launcher_lx(void) { return LOGO_X; }
static uint64_t launcher_ly(void) {
    return console_fb_height() - TASKBAR_H - LAUNCHER_ITEMS * LAUNCHER_ITEM_H - 2u;
}

static void launcher_draw(void) {
    uint64_t lx = launcher_lx();
    uint64_t ly = launcher_ly();
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();
    static const char *items[] = { "Terminal", "Files", "Settings" };

    int32_t mx, my;
    bool lbtn, rbtn;
    mouse_get_state(&mx, &my, &lbtn, &rbtn);

    for (int i = 0; i < (int)LAUNCHER_ITEMS; i++) {
        uint64_t ry = ly + (uint64_t)i * LAUNCHER_ITEM_H;
        bool hov = (g_launcher_hover == i);
        uint32_t bg = hov ? COL_LAUNCH_HL : COL_LAUNCH_BG;
        console_fill_rect(lx, ry, LAUNCHER_W, LAUNCHER_ITEM_H, bg);
        uint64_t slen = (uint64_t)gui_strlen(items[i]);
        uint64_t spx  = lx + (LAUNCHER_W - slen * fw) / 2u;
        uint64_t spy  = ry + (LAUNCHER_ITEM_H - fh) / 2u;
        gui_draw_str(spx, spy, items[i], COL_LAUNCH_FG, bg);
    }
    console_fill_rect(lx, ly, LAUNCHER_W, 1u, COL_LAUNCH_HL);
    console_fill_rect(lx, ly + LAUNCHER_ITEMS * LAUNCHER_ITEM_H, LAUNCHER_W, 1u, COL_LAUNCH_HL);
    console_fill_rect(lx, ly, 1u, LAUNCHER_ITEMS * LAUNCHER_ITEM_H + 1u, COL_LAUNCH_HL);
    console_fill_rect(lx + LAUNCHER_W - 1u, ly, 1u, LAUNCHER_ITEMS * LAUNCHER_ITEM_H + 1u, COL_LAUNCH_HL);
}

/* ── Context menu ────────────────────────────────────────────────────── */

static void ctx_draw(void) {
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();
    static const char *ctx_items[] = { "Terminal", "Files", "Settings" };
    int32_t cx = g_ctx_x;
    int32_t cy = g_ctx_y;

    console_fill_rect((uint64_t)cx, (uint64_t)cy, CTX_W, CTX_ITEMS * CTX_ITEM_H + 2u, COL_LAUNCH_BG);
    console_fill_rect((uint64_t)cx, (uint64_t)cy, CTX_W, 1u, COL_LAUNCH_HL);
    console_fill_rect((uint64_t)cx, (uint64_t)cy + CTX_ITEMS * CTX_ITEM_H + 1u, CTX_W, 1u, COL_LAUNCH_HL);
    console_fill_rect((uint64_t)cx, (uint64_t)cy, 1u, CTX_ITEMS * CTX_ITEM_H + 2u, COL_LAUNCH_HL);
    console_fill_rect((uint64_t)cx + CTX_W - 1u, (uint64_t)cy, 1u, CTX_ITEMS * CTX_ITEM_H + 2u, COL_LAUNCH_HL);

    for (int i = 0; i < (int)CTX_ITEMS; i++) {
        uint64_t ry  = (uint64_t)cy + 1u + (uint64_t)i * CTX_ITEM_H;
        bool hov     = (g_ctx_hover == i);
        uint32_t bg  = hov ? COL_LAUNCH_HL : COL_LAUNCH_BG;
        console_fill_rect((uint64_t)cx + 1u, ry, CTX_W - 2u, CTX_ITEM_H, bg);
        uint64_t slen = (uint64_t)gui_strlen(ctx_items[i]);
        uint64_t spx  = (uint64_t)cx + (CTX_W - slen * fw) / 2u;
        uint64_t spy  = ry + (CTX_ITEM_H - fh) / 2u;
        gui_draw_str(spx, spy, ctx_items[i], COL_LAUNCH_FG, bg);
    }
}

/* ── Window chrome ───────────────────────────────────────────────────── */

static void win_draw_chrome(window_t *w, bool fill_content) {
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();

    int slot = (int)(w - g_wins);
    bool active = (g_z[MAX_WINS - 1] == slot);
    uint32_t title_bg = active ? COL_BORDER : 0x00182840u;

    /* Subtle 1-px focus ring just outside the active window */
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

    console_fill_rect(w->x, w->y, w->w, TITLE_H, title_bg);

    /* Compute button positions first so we know available title width */
    w->btn_cls_x = w->x + w->w - BTN_W;
    w->btn_max_x = w->btn_cls_x - BTN_W;
    w->btn_min_x = w->btn_max_x - BTN_W;

    /* Title: centered if it fits, left-aligned+clipped if too wide */
    uint64_t tlen     = (uint64_t)gui_strlen(w->title);
    uint64_t tpy      = w->y + (TITLE_H - fh) / 2u;
    uint64_t avail    = w->btn_min_x > w->x + 8u ? w->btn_min_x - w->x - 8u : 0u;
    uint64_t max_ch   = fw > 0u ? avail / fw : 0u;
    uint64_t tpx;
    if (tlen <= max_ch) {
        tpx = w->x + (w->w - tlen * fw) / 2u;
        if (w->w < tlen * fw) tpx = w->x + 4u;  /* safety against wrap */
    } else {
        tpx = w->x + 4u;
    }
    gui_draw_str_clip(tpx, tpy, w->title, COL_TITLE_FG, title_bg, max_ch);

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

    console_fill_rect(w->x, w->y + TITLE_H,
                      BORDER, w->h - TITLE_H, COL_BORDER);
    console_fill_rect(w->x + w->w - BORDER, w->y + TITLE_H,
                      BORDER, w->h - TITLE_H, COL_BORDER);
    console_fill_rect(w->x, w->y + w->h - BORDER,
                      w->w, BORDER, COL_BORDER);

    if (fill_content) {
        uint64_t ix = w->x + BORDER;
        uint64_t iy = w->y + TITLE_H;
        uint64_t iw = w->w - 2u * BORDER;
        uint64_t ih = w->h - TITLE_H - BORDER;
        console_fill_rect(ix, iy, iw, ih, COL_WIN_BG);
    }
}

/* ── Terminal viewport helpers ───────────────────────────────────────── */

static void term_set_viewport(window_t *w) {
    uint64_t ix = w->x + BORDER;
    uint64_t iy = w->y + TITLE_H;
    uint64_t iw = w->w - 2u * BORDER;
    uint64_t ih = w->h - TITLE_H - BORDER;
    console_set_viewport(ix + PAD, iy + PAD, iw - 2u * PAD, ih - 2u * PAD);
}

/* ── File browser ────────────────────────────────────────────────────── */

static void fb_str_copy(char *dst, const char *src, int maxlen) {
    int i;
    for (i = 0; i < maxlen - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void fb_path_join(char *out, const char *parent, const char *child) {
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

static void fb_path_parent(char *out, const char *path) {
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

/* Returns icon string and sets *col */
static const char *fb_file_icon(const char *name, uint32_t *col) {
    if (fb_has_ext(name, ".txt") || fb_has_ext(name, ".md") ||
        fb_has_ext(name, ".log")) {
        *col = COL_FB_TXT; return "[T]";
    }
    if (fb_has_ext(name, ".c") || fb_has_ext(name, ".h") ||
        fb_has_ext(name, ".cpp") || fb_has_ext(name, ".s") ||
        fb_has_ext(name, ".asm")) {
        *col = COL_FB_CODE; return "[C]";
    }
    if (fb_has_ext(name, ".sh") || fb_has_ext(name, ".py") ||
        fb_has_ext(name, ".lua")) {
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

static void fb_load(fb_state_t *fb, const char *path) {
    fb_str_copy(fb->path, path, 128);
    fb->entry_count  = 0;
    fb->scroll       = 0;
    fb->hover_row    = -1;
    fb->sel_row      = -1;
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
            fb_str_copy(fb->entries[fb->entry_count], p, len + 1);
            fb->entries[fb->entry_count][len] = '\0';
            char full[256];
            fb_path_join(full, path, fb->entries[fb->entry_count]);
            fb->is_dir[fb->entry_count] = (vfs_isdir(full) == 1);
            fb->entry_count++;
        }
        p = nl + 1;
    }
}

static void fb_navigate(fb_state_t *fb, const char *path) {
    /* Push current path to history */
    if (fb->hist_depth < FB_HIST_MAX)
        fb_str_copy(fb->hist[fb->hist_depth++], fb->path, 128);
    fb_load(fb, path);
}

static void fb_back(fb_state_t *fb) {
    if (fb->hist_depth == 0) return;
    char prev[128];
    fb_str_copy(prev, fb->hist[--fb->hist_depth], 128);
    fb_load(fb, prev);
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
                                 const char *label, bool enabled) {
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();
    uint32_t bg  = enabled ? COL_FB_BTN     : COL_FB_BTN_DIS;
    uint32_t fg  = enabled ? COL_FB_BTN_FG  : COL_FB_BTN_DIS_FG;
    console_fill_rect(bx, by, bw, bh, bg);
    /* 1-px border */
    console_fill_rect(bx, by, bw, 1, enabled ? 0x00304858u : 0x00161a22u);
    console_fill_rect(bx, by+bh-1, bw, 1, 0x00090c12u);
    uint64_t llen = (uint64_t)gui_strlen(label);
    uint64_t lpx  = bx + (bw - llen*fw)/2u;
    uint64_t lpy  = by + (bh - fh)/2u;
    gui_draw_str(lpx, lpy, label, fg, bg);
}

/* Full file browser render */
static void fb_render(window_t *w) {
    win_draw_chrome(w, false);
    uint64_t fw  = console_font_width();
    uint64_t fh  = console_font_height();
    uint64_t ix   = w->x + BORDER;
    uint64_t iy   = w->y + TITLE_H;
    uint64_t iw   = w->w - 2u * BORDER;
    uint64_t ih   = w->h - TITLE_H - BORDER;
    uint64_t tb_h = FB_TOOLBAR_H;

    /* ── Toolbar row 1: nav buttons + path bar ── */
    uint64_t r1_h   = FB_ROW1_H;
    uint64_t btn_h  = fh + 6u;
    uint64_t btn_y  = iy + (r1_h - btn_h) / 2u;
    bool can_back   = (w->fb.hist_depth > 0);
    bool can_up     = !(w->fb.path[0] == '/' && w->fb.path[1] == '\0');

    console_fill_rect(ix, iy, iw, r1_h, COL_FB_TOOLBAR);
    console_fill_rect(ix, iy + r1_h - 1, iw, 1, COL_FB_SEP);

    uint64_t bb_x = ix + 4u;
    fb_draw_toolbar_btn(bb_x, btn_y, FB_BTN_W, btn_h, "<", can_back);

    uint64_t ub_x = bb_x + FB_BTN_W + 4u;
    fb_draw_toolbar_btn(ub_x, btn_y, FB_BTN_W, btn_h, "^", can_up);

    uint64_t rb_x = ub_x + FB_BTN_W + 4u;
    fb_draw_toolbar_btn(rb_x, btn_y, FB_BTN_W, btn_h, "R", true);

    /* Path bar */
    uint64_t pb_x = rb_x + FB_BTN_W + 6u;
    uint64_t pb_w = iw - (pb_x - ix) - 4u;
    console_fill_rect(pb_x, btn_y, pb_w, btn_h, COL_FB_PATH_BG);
    console_fill_rect(pb_x, btn_y, pb_w, 1, 0x00253545u);
    console_fill_rect(pb_x, btn_y+btn_h-1, pb_w, 1, 0x00050810u);
    uint64_t pb_max = (pb_w > 2*fw) ? (pb_w - 2*fw) / fw : 0;
    gui_draw_str_clip(pb_x + fw, btn_y + (btn_h-fh)/2u, w->fb.path,
                      COL_FB_PATH_FG, COL_FB_PATH_BG, pb_max);

    /* ── Toolbar row 2: search bar ── */
    uint64_t r2_y = iy + r1_h;
    uint64_t r2_h = r1_h;
    console_fill_rect(ix, r2_y, iw, r2_h, COL_FB_TOOLBAR);
    console_fill_rect(ix, r2_y + r2_h - 1, iw, 1, COL_FB_SEP);

    /* Search icon label */
    uint64_t si_x = ix + 4u;
    gui_draw_str(si_x, r2_y + (r2_h - fh)/2u, "Search:", COL_FB_SB_FG, COL_FB_TOOLBAR);
    uint64_t sb_x   = si_x + 8u * fw;
    uint64_t srch_w = iw - (sb_x - ix) - 4u;
    uint32_t sb_bg  = w->fb.search_active ? COL_FB_SEARCH_ACT : COL_FB_SEARCH_BG;
    console_fill_rect(sb_x, r2_y + (r2_h - btn_h)/2u, srch_w, btn_h, sb_bg);
    console_fill_rect(sb_x, r2_y + (r2_h - btn_h)/2u, srch_w, 1,
                      w->fb.search_active ? COL_FB_SEARCH_CUR : 0x00253545u);
    /* Draw search text */
    uint64_t sq_max = (srch_w > 2*fw) ? (srch_w - 2*fw) / fw - 1u : 0;
    if (w->fb.search_len > 0) {
        gui_draw_str_clip(sb_x + fw, r2_y + (r2_h - btn_h)/2u + (btn_h - fh)/2u,
                          w->fb.search_query, COL_FB_SEARCH_FG, sb_bg, sq_max);
    } else {
        gui_draw_str_clip(sb_x + fw, r2_y + (r2_h - btn_h)/2u + (btn_h - fh)/2u,
                          "Type to filter...", COL_FB_MUTED, sb_bg, sq_max);
    }
    /* Cursor when active */
    if (w->fb.search_active) {
        uint64_t cur_x = sb_x + fw + (uint64_t)w->fb.search_len * fw;
        if (cur_x < sb_x + srch_w - fw)
            console_fill_rect(cur_x, r2_y + (r2_h - btn_h)/2u + 2u,
                              2u, btn_h - 4u, COL_FB_SEARCH_CUR);
    }

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

    static const char *sb_labels[] = { "Root /", "/bin", "/etc", "/dev", "/usr", NULL };
    static const char *sb_paths[]  = { "/",     "/bin", "/etc", "/dev", "/usr", NULL };
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

    /* Column header */
    uint64_t hdr_h = fh + 4u;
    console_fill_rect(lx, ly, lw, hdr_h, COL_FB_TOOLBAR);
    console_fill_rect(lx, ly + hdr_h - 1, lw, 1, COL_FB_SEP);
    uint64_t icon_col_w = (FB_ICON_COLS + 1u) * fw;
    uint64_t name_col_x = lx + icon_col_w + 4u;
    gui_draw_str(lx + 2u, ly + 2u, "   Name", 0x00506878u, COL_FB_TOOLBAR);
    ly += hdr_h;
    lh -= hdr_h;

    /* File rows */
    uint64_t row_h   = FB_ROW_H;
    uint64_t max_rows = lh / row_h;
    console_fill_rect(lx, ly, lw, lh, COL_FB_LIST_BG);

    int row_idx = 0;
    int skipped = 0;
    for (int i = 0; i < w->fb.entry_count && row_idx < (int)max_rows; i++) {
        /* Apply search filter */
        if (!fb_name_matches(w->fb.entries[i], w->fb.search_query)) continue;
        /* Apply scroll offset */
        if (skipped < w->fb.scroll) { skipped++; continue; }
        uint64_t ry = ly + (uint64_t)row_idx * row_h;
        bool hov = (i == w->fb.hover_row);
        bool sel = (i == w->fb.sel_row);
        bool matched = (w->fb.search_len > 0);  /* highlight all when searching */
        uint32_t row_bg = sel ? COL_FB_SEL :
                          hov ? COL_FB_HOV :
                          matched ? COL_FB_MATCH_HL :
                          (row_idx & 1) ? COL_FB_LIST_ALT : COL_FB_LIST_BG;

        console_fill_rect(lx, ry, lw, row_h, row_bg);

        /* Icon */
        const char *icon;
        uint32_t icon_fg;
        if (w->fb.is_dir[i]) {
            icon = "[/]";
            icon_fg = COL_FB_DIR;
        } else {
            icon = fb_file_icon(w->fb.entries[i], &icon_fg);
        }
        gui_draw_str(lx + 2u, ry + (row_h - fh) / 2u, icon, icon_fg, row_bg);

        /* Name */
        uint64_t name_max = (lw > (name_col_x - lx) + fw) ?
                            (lw - (name_col_x - lx)) / fw - 1u : 1u;
        const char *name = w->fb.entries[i];
        uint32_t name_fg = w->fb.is_dir[i] ? COL_FB_DIR : icon_fg;
        if (w->fb.is_dir[i]) {
            /* Show dir name with trailing slash */
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
        row_idx++;
    }

    /* Empty-directory message */
    if (w->fb.entry_count == 0) {
        const char *msg = "(empty)";
        uint64_t mx2 = lx + (lw - (uint64_t)gui_strlen(msg)*fw)/2u;
        uint64_t my2 = ly + lh/2u - fh/2u;
        gui_draw_str(mx2, my2, msg, COL_FB_MUTED, COL_FB_LIST_BG);
    }

    /* Scrollbar */
    if (w->fb.entry_count > (int)max_rows) {
        uint64_t sb_x  = lx + lw - 6u;
        uint64_t sb_y  = ly;
        uint64_t sb_th = lh;
        console_fill_rect(sb_x, sb_y, 6u, sb_th, 0x000a0e16u);
        /* thumb */
        uint64_t thumb_h = (max_rows * sb_th) / (uint64_t)w->fb.entry_count;
        if (thumb_h < 8) thumb_h = 8;
        uint64_t thumb_y = sb_y + ((uint64_t)w->fb.scroll * (sb_th - thumb_h))
                           / (uint64_t)(w->fb.entry_count - (int)max_rows + 1);
        console_fill_rect(sb_x + 1u, thumb_y, 4u, thumb_h, 0x00304858u);
    }

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
}

/* ── Text viewer ─────────────────────────────────────────────────────── */

static void text_open(window_t *w, const char *path) {
    fb_str_copy(w->text.path, path, TV_PATH_MAX);
    w->text.scroll = 0;
    w->text.data   = NULL;
    w->text.size   = 0;
    w->text.total_lines = 0;

    vfs_read(path, &w->text.data, &w->text.size);

    if (w->text.data && w->text.size > 0) {
        const char *d = (const char *)w->text.data;
        for (uint64_t i = 0; i < w->text.size; i++)
            if (d[i] == '\n') w->text.total_lines++;
        if (d[w->text.size - 1] != '\n') w->text.total_lines++;
    }

    /* Build title from basename */
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
    int ti = 0;
    while (base[ti] && ti < 62) { w->text.title_buf[ti] = base[ti]; ti++; }
    w->text.title_buf[ti] = '\0';
    w->title = w->text.title_buf;
}

static void text_render(window_t *w) {
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();
    uint64_t ix = w->x + BORDER;
    uint64_t iy = w->y + TITLE_H;
    uint64_t iw = w->w - 2u * BORDER;
    uint64_t ih = w->h - TITLE_H - BORDER;

    console_fill_rect(ix, iy, iw, ih, COL_FB_LIST_BG);

    if (!w->text.data || w->text.size == 0) {
        const char *msg = "(empty file)";
        gui_draw_str(ix + PAD, iy + PAD, msg, COL_FB_MUTED, COL_FB_LIST_BG);
        return;
    }

    /* Line number gutter */
    uint64_t gutter_chars = 1;
    int tot = w->text.total_lines > 0 ? w->text.total_lines : 1;
    while (tot >= 10) { tot /= 10; gutter_chars++; }
    uint64_t gutter_w  = (gutter_chars + 2u) * fw;
    uint64_t gutter_bg = 0x00090d14u;
    console_fill_rect(ix, iy, gutter_w, ih, gutter_bg);
    console_fill_rect(ix + gutter_w, iy, 1u, ih, 0x00202830u);

    uint64_t tx        = ix + gutter_w + 1u;  /* text content start x */
    uint64_t avail_w   = iw > gutter_w + 13u ? iw - gutter_w - 13u : 1u;
    uint64_t max_cols  = avail_w > 2u * PAD ? (avail_w - 2u * PAD) / fw : 1u;
    uint64_t max_rows  = ih > 2u * PAD ? (ih - 2u * PAD) / fh : 1u;
    if (max_cols < 1) max_cols = 1;
    if (max_rows < 1) max_rows = 1;

    /* Clamp scroll */
    int max_scroll = w->text.total_lines - (int)max_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (w->text.scroll > max_scroll) w->text.scroll = max_scroll;
    if (w->text.scroll < 0)         w->text.scroll = 0;

    const char *d   = (const char *)w->text.data;
    uint64_t    pos = 0;
    int line = 0;

    /* Skip to scroll offset */
    while (pos < w->text.size && line < w->text.scroll) {
        while (pos < w->text.size && d[pos] != '\n') pos++;
        if (pos < w->text.size) pos++;
        line++;
    }

    /* Render lines */
    int row = 0;
    while (pos <= w->text.size && (uint64_t)row < max_rows) {
        uint64_t py = iy + PAD + (uint64_t)row * fh;

        /* Line number: right-align in gutter */
        int linenum = w->text.scroll + row + 1;
        char lnbuf[8]; gui_itoa(linenum, lnbuf, 8);
        uint64_t ln_len = (uint64_t)gui_strlen(lnbuf);
        uint64_t ln_x   = ix + gutter_w - (ln_len + 1u) * fw;
        gui_draw_str(ln_x, py, lnbuf, 0x00405060u, gutter_bg);

        /* Text content */
        uint64_t col = 0;
        while (pos < w->text.size && d[pos] != '\n' && col < max_cols) {
            unsigned char c = (unsigned char)d[pos];
            if (c >= 32 && c < 127) {
                console_render_glyph(tx + PAD + col * fw, py, c,
                                     COL_FB_TXT, COL_FB_LIST_BG);
                col++;
            } else if (c == '\t') {
                uint64_t next = (col + 4u) & ~3u;
                while (col < next && col < max_cols) {
                    console_render_glyph(tx + PAD + col * fw, py, ' ',
                                         COL_FB_TXT, COL_FB_LIST_BG);
                    col++;
                }
            }
            pos++;
        }
        /* advance past remainder of this line */
        while (pos < w->text.size && d[pos] != '\n') pos++;
        if (pos < w->text.size) pos++;
        else if (pos == w->text.size) pos++;
        row++;
    }

    /* Scrollbar */
    if (w->text.total_lines > (int)max_rows) {
        uint64_t sb_x = ix + iw - 8u;
        console_fill_rect(sb_x, iy, 8u, ih, 0x000a0e16u);
        uint64_t thumb_h = (max_rows * ih) / (uint64_t)w->text.total_lines;
        if (thumb_h < 8) thumb_h = 8;
        uint64_t thumb_y = iy + ((uint64_t)w->text.scroll * (ih - thumb_h))
                           / (uint64_t)(max_scroll > 0 ? max_scroll : 1);
        console_fill_rect(sb_x + 2u, thumb_y, 4u, thumb_h, 0x00304858u);
    }
}

/* ── File browser region helpers ─────────────────────────────────────── */

/* Returns the inner content area */
static void fb_inner(window_t *w, uint64_t *ix, uint64_t *iy,
                     uint64_t *iw, uint64_t *ih) {
    *ix = w->x + BORDER;
    *iy = w->y + TITLE_H;
    *iw = w->w - 2u * BORDER;
    *ih = w->h - TITLE_H - BORDER;
}

/* Returns the list area origin and dimensions */
static void fb_list_region(window_t *w,
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
static int fb_hit_row(window_t *w, int32_t mx, int32_t my) {
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
    static const char *sb_paths[] = {"/", "/bin", "/etc", "/dev", "/usr", NULL};
    int count = 0;
    while (sb_paths[count]) count++;
    if (item < 0 || item >= count) return -1;
    return item;
}

/* Check toolbar hit: 0=back,1=up,2=refresh,3=search_bar,-1=miss */
static int fb_hit_toolbar(window_t *w, int32_t mx, int32_t my) {
    uint64_t ix, iy, iw, ih;
    fb_inner(w, &ix, &iy, &iw, &ih);
    (void)iw; (void)ih;
    uint64_t fh    = console_font_height();
    uint64_t r1_h  = FB_ROW1_H;
    uint64_t btn_h = fh + 6u;
    uint64_t btn_y = iy + (r1_h - btn_h) / 2u;

    /* Row 1 buttons */
    if ((uint64_t)my >= btn_y && (uint64_t)my < btn_y + btn_h) {
        uint64_t bb_x = ix + 4u;
        uint64_t ub_x = bb_x + FB_BTN_W + 4u;
        uint64_t rb_x = ub_x + FB_BTN_W + 4u;
        if ((uint64_t)mx >= bb_x && (uint64_t)mx < bb_x + FB_BTN_W) return 0;
        if ((uint64_t)mx >= ub_x && (uint64_t)mx < ub_x + FB_BTN_W) return 1;
        if ((uint64_t)mx >= rb_x && (uint64_t)mx < rb_x + FB_BTN_W) return 2;
    }

    /* Row 2: search bar */
    uint64_t r2_y  = iy + r1_h;
    uint64_t r2_h  = r1_h;
    (void)btn_h;
    if ((uint64_t)my >= r2_y && (uint64_t)my < r2_y + r2_h)
        return 3;

    return -1;
}

static void fb_on_motion(window_t *w, int32_t mx, int32_t my) {
    int new_hover = fb_hit_row(w, mx, my);
    if (new_hover == w->fb.hover_row) return;
    w->fb.hover_row = new_hover;
    fb_render(w);
}

static void win_show(window_t *w, int slot);  /* forward decl */

static void fb_on_click(window_t *w, int32_t mx, int32_t my) {
    static const char *sb_paths[] = {"/", "/bin", "/etc", "/dev", "/usr", NULL};

    /* Toolbar buttons */
    int tb = fb_hit_toolbar(w, mx, my);
    if (tb == 0) { fb_back(&w->fb); fb_render(w); return; }
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
                    g_sb_drag      = true;
                    g_sb_drag_win  = (int)(w - g_wins);
                    g_sb_drag_y0   = my;
                    g_sb_drag_s0   = w->fb.scroll;
                    g_sb_drag_lh   = tlh;
                    g_sb_drag_ly   = tly;
                    g_sb_drag_max  = max_sc;
                    g_sb_drag_text = false;
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

    /* File list */
    int idx = fb_hit_row(w, mx, my);
    if (idx < 0) return;
    w->fb.sel_row = idx;
    if (w->fb.is_dir[idx]) {
        char newpath[256];
        fb_path_join(newpath, w->fb.path, w->fb.entries[idx]);
        fb_navigate(&w->fb, newpath);
    } else {
        /* Open text files in the viewer (slot 2) */
        const char *name = w->fb.entries[idx];
        if (fb_has_ext(name, ".txt") || fb_has_ext(name, ".log") ||
            fb_has_ext(name, ".md")  || fb_has_ext(name, ".ini") ||
            fb_has_ext(name, ".cfg") || fb_has_ext(name, ".sh")) {
            char full[256];
            fb_path_join(full, w->fb.path, name);
            text_open(&g_wins[3], full);
            win_show(&g_wins[3], 3);
            return;
        }
    }
    fb_render(w);
}

/* ── Settings window ─────────────────────────────────────────────────── */

#define SET_PAD     12u
#define SET_ROW_H   20u
#define SET_SEC_H   22u
#define COL_SET_BG      0x000c1018u
#define COL_SET_SEC_BG  0x00141e28u
#define COL_SET_SEC_FG  0x005898e8u
#define COL_SET_KEY_FG  0x0080a0c8u
#define COL_SET_VAL_FG  0x00d0dce8u
#define COL_SET_SEP     0x00182838u
#define COL_SET_HINT    0x00405060u

static void settings_render(window_t *w) {
    uint64_t ix = w->x + BORDER;
    uint64_t iy = w->y + TITLE_H;
    uint64_t iw = w->w - 2u * BORDER;
    uint64_t ih = w->h - TITLE_H - BORDER;
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();
    uint64_t cx = ix + SET_PAD;
    uint64_t cy = iy + SET_PAD;
    uint64_t val_x = ix + SET_PAD + 18u * fw;  /* value column */

    console_fill_rect(ix, iy, iw, ih, COL_SET_BG);

    /* ── Section: System ── */
    console_fill_rect(ix, cy, iw, SET_SEC_H, COL_SET_SEC_BG);
    gui_draw_str(cx, cy + (SET_SEC_H - fh) / 2u, "System Information",
                 COL_SET_SEC_FG, COL_SET_SEC_BG);
    cy += SET_SEC_H + 4u;

    /* Build dynamic resolution string */
    char res_str[24];
    {
        char ws[8], hs[8];
        gui_itoa((int)console_fb_width(),  ws, 8);
        gui_itoa((int)console_fb_height(), hs, 8);
        int ri = 0;
        for (int k = 0; ws[k] && ri < 20; ) res_str[ri++] = ws[k++];
        res_str[ri++] = ' '; res_str[ri++] = 'x'; res_str[ri++] = ' ';
        for (int k = 0; hs[k] && ri < 23; ) res_str[ri++] = hs[k++];
        res_str[ri] = '\0';
    }
    /* Build dynamic memory string */
    char mem_str[24];
    {
        uint64_t total_p = pmm_get_total_pages();
        char tb[8];
        gui_itoa((int)((total_p * 4096u) >> 20u), tb, 8);
        int ri = 0;
        for (int k = 0; tb[k] && ri < 20; ) mem_str[ri++] = tb[k++];
        mem_str[ri++] = ' '; mem_str[ri++] = 'M'; mem_str[ri++] = 'B';
        mem_str[ri] = '\0';
    }
    struct { const char *key; const char *val; } sysinfo[] = {
        { "OS:",         "FiFi OS Alpha v5.0" },
        { "Arch:",       "x86_64"             },
        { "Kernel:",     "freestanding"       },
        { "Bootloader:", "Limine / UEFI"      },
        { "Memory:",     mem_str              },
        { "Resolution:", res_str              },
        { NULL, NULL }
    };
    for (int i = 0; sysinfo[i].key; i++) {
        uint32_t bg = (i & 1) ? 0x000f151fu : COL_SET_BG;
        console_fill_rect(ix, cy, iw, SET_ROW_H, bg);
        gui_draw_str(cx, cy + (SET_ROW_H - fh) / 2u, sysinfo[i].key, COL_SET_KEY_FG, bg);
        gui_draw_str(val_x, cy + (SET_ROW_H - fh) / 2u, sysinfo[i].val, COL_SET_VAL_FG, bg);
        cy += SET_ROW_H;
    }

    cy += 8u;
    console_fill_rect(ix, cy, iw, 1u, COL_SET_SEP);
    cy += 5u;

    /* ── Section: Keyboard Shortcuts ── */
    console_fill_rect(ix, cy, iw, SET_SEC_H, COL_SET_SEC_BG);
    gui_draw_str(cx, cy + (SET_SEC_H - fh) / 2u, "Keyboard Shortcuts",
                 COL_SET_SEC_FG, COL_SET_SEC_BG);
    cy += SET_SEC_H + 4u;

    struct { const char *key; const char *desc; } shortcuts[] = {
        { "Alt+Tab",        "Cycle open windows"     },
        { "Esc / Ctrl+W",   "Close focused window"   },
        { "Up / Down",      "Navigate file list"     },
        { "Page Up/Down",   "Scroll one page"        },
        { "Home / End",     "Jump to top / bottom"   },
        { "Enter",          "Open file or folder"    },
        { "/ or F",         "Open search bar"        },
        { "Backspace",      "Delete search char"     },
        { "Tab",            "Close search bar"       },
        { NULL, NULL }
    };
    for (int i = 0; shortcuts[i].key; i++) {
        uint32_t bg = (i & 1) ? 0x000f151fu : COL_SET_BG;
        console_fill_rect(ix, cy, iw, SET_ROW_H, bg);
        gui_draw_str(cx, cy + (SET_ROW_H - fh) / 2u, shortcuts[i].key, COL_SET_KEY_FG, bg);
        gui_draw_str(val_x, cy + (SET_ROW_H - fh) / 2u, shortcuts[i].desc, COL_SET_VAL_FG, bg);
        cy += SET_ROW_H;
        if (cy + SET_ROW_H > iy + ih) break;
    }

    /* ── Hint at bottom ── */
    uint64_t hint_y = iy + ih - fh - 8u;
    if (hint_y > cy + 4u) {
        console_fill_rect(ix, hint_y - 4u, iw, 1u, COL_SET_SEP);
        gui_draw_str(cx, hint_y,
                     "Press Esc or Ctrl+W to close", COL_SET_HINT, COL_SET_BG);
    }
}

/* ── Window content render ───────────────────────────────────────────── */

static void win_render_content(window_t *w) {
    if (w->type == WIN_TERM)
        term_set_viewport(w);
    else if (w->type == WIN_FILES)
        fb_render(w);
    else if (w->type == WIN_TEXT)
        text_render(w);
    else if (w->type == WIN_SETTINGS)
        settings_render(w);
}

/* ── Status bar (top strip, 0..STATUS_H-1) ───────────────────────────── */

static void draw_status_bar(void) {
    uint64_t fb_w = console_fb_width();
    uint64_t fw   = console_font_width();
    uint64_t fh   = console_font_height();
    uint32_t bar_bg = 0x0008101cu;
    uint64_t sy   = (STATUS_H > fh) ? (STATUS_H - fh) / 2u : 0u;

    console_fill_rect(0, 0, fb_w, STATUS_H, bar_bg);
    console_fill_rect(0, STATUS_H - 1, fb_w, 1, COL_TASKBAR_SEP);

    /* Left: branding */
    gui_draw_str(6u, sy, "FiFi OS", COL_BORDER, bar_bg);

    /* Uptime: HH:MM:SS */
    uint64_t hz   = pit_get_hz();
    if (!hz) hz   = 100;
    uint64_t secs = pit_ticks() / hz;
    uint64_t mins = secs / 60u;  secs %= 60u;
    uint64_t hrs  = mins / 60u;  mins %= 60u;

    char up[12];
    gui_itoa_pad2(hrs,  up + 0);
    up[2] = ':';
    gui_itoa_pad2(mins, up + 3);
    up[5] = ':';
    gui_itoa_pad2(secs, up + 6);
    up[8] = '\0';

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
        for (p = " / "; *p && i < 28; ) membuf[i++] = *p++;
        for (p = tb;    *p && i < 28; ) membuf[i++] = *p++;
        for (p = " MB"; *p && i < 28; ) membuf[i++] = *p++;
        membuf[i] = '\0';
    }

    /* Right-align: memory  |  uptime */
    uint64_t up_len  = 8u;
    uint64_t mem_len = (uint64_t)gui_strlen(membuf);
    uint64_t right_w = (mem_len + 3u + up_len) * fw + 12u;
    uint64_t rx = fb_w > right_w ? fb_w - right_w : 0u;
    uint32_t info_col = 0x00506878u;

    gui_draw_str(rx,                         sy, membuf, info_col, bar_bg);
    gui_draw_str(rx + (mem_len + 1u) * fw,   sy, "|",    0x00303848u, bar_bg);
    gui_draw_str(rx + (mem_len + 3u) * fw,   sy, up,     info_col, bar_bg);
}

/* ── Resize edge highlight ───────────────────────────────────────────── */

static void draw_resize_hint(int slot, resize_dir_t dir) {
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

/* ── Desktop background with subtle dot grid ─────────────────────────── */

static void draw_desktop_bg(void) {
    uint64_t fb_w = console_fb_width();
    console_fill_rect(0, desk_top(), fb_w, desk_avail(), COL_DESKTOP);
    uint32_t dot = 0x00252838u;
    for (uint64_t y = desk_top() + 16; y + 1 < desk_bot(); y += 24)
        for (uint64_t x = 16; x + 1 < fb_w; x += 24)
            console_fill_rect(x, y, 1, 1, dot);
}

/* ── Full compositing redraw ─────────────────────────────────────────── */

static void full_redraw(void) {
    uint64_t fb_w = console_fb_width();
    draw_desktop_bg();
    draw_status_bar();
    bool suppress_term = false;
    /* Shadow pass: two-layer soft drop shadow */
    for (int zi = 0; zi < MAX_WINS; zi++) {
        int i = g_z[zi];
        window_t *w = &g_wins[i];
        if (!w->active || w->state == WIN_HIDDEN) continue;
        /* Outer (lighter) layer */
        uint64_t sx3 = w->x + 3u, sy3 = w->y + 3u;
        if (sx3 + w->w <= fb_w && sy3 + w->h <= desk_bot())
            console_fill_rect(sx3, sy3, w->w, w->h, 0x00080c1au);
        /* Inner (darker) core */
        uint64_t sx6 = w->x + 6u, sy6 = w->y + 6u;
        if (sx6 + w->w <= fb_w && sy6 + w->h <= desk_bot())
            console_fill_rect(sx6, sy6, w->w, w->h, 0x00020408u);
    }
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
        /* For WIN_FILES, fb_render() calls win_draw_chrome() itself — skip the fill here */
        win_draw_chrome(w, w->type != WIN_FILES);
        win_render_content(w);
        if (w->type != WIN_TERM) suppress_term = true;
    }
    /* Resize edge hint overlay */
    if (g_resize_hover_win >= 0 && g_resize_hover_dir != RES_NONE)
        draw_resize_hint(g_resize_hover_win, g_resize_hover_dir);
    /* Non-terminal window visible: suppress terminal rendering to avoid overwrites */
    if (suppress_term)
        console_set_viewport(0, 0, 0, 0);
    taskbar_draw();
    if (g_launcher_open)
        launcher_draw();
    if (g_ctx_open)
        ctx_draw();
}

/* ── Window open / hide / maximize ──────────────────────────────────── */

static void win_show(window_t *w, int slot) {
    uint64_t fb_w  = console_fb_width();
    uint64_t avail = desk_avail();

    if (w->w == 0 && w->h == 0) {
        if (w->type == WIN_TERM) {
            w->w = fb_w * 88u / 100u;
            w->h = avail * 90u / 100u;
        } else if (w->type == WIN_SETTINGS) {
            w->w = 520u;
            w->h = 480u;
            if (w->w > fb_w) w->w = fb_w;
            if (w->h > avail) w->h = avail;
        } else {
            w->w = fb_w * 60u / 100u;
            w->h = avail * 85u / 100u;
        }
        uint64_t ox = (uint64_t)slot * 32u;
        uint64_t oy = (uint64_t)slot * 32u;
        w->x = (fb_w - w->w) / 2u + ox;
        w->y = desk_top() + (avail - w->h) / 2u + oy;
        if (w->x + w->w > fb_w) w->x = fb_w > w->w ? fb_w - w->w : 0;
        if (w->y + w->h > desk_bot()) w->y = desk_bot() > w->h ? desk_bot() - w->h : desk_top();
    }

    w->state = WIN_NORMAL;
    full_redraw();
}

static void win_hide(window_t *w, int slot) {
    (void)slot;
    if (w->type == WIN_TERM)
        console_set_viewport(0, 0, 0, 0);
    g_dragging    = false;
    g_resizing    = false;
    g_snap_preview = 0;
    w->state      = WIN_HIDDEN;
    full_redraw();
}

static void win_maximize_toggle(window_t *w) {
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

static resize_dir_t hit_resize(window_t *w, int32_t mx, int32_t my) {
    if (w->state != WIN_NORMAL) return RES_NONE;

    int32_t wx = (int32_t)w->x;
    int32_t wy = (int32_t)w->y;
    int32_t we = wx + (int32_t)w->w;
    int32_t wb = wy + (int32_t)w->h;
    int32_t m  = (int32_t)RESIZE_MARGIN;
    int32_t cm = m * 2;

    if (mx < wx - m || mx > we + m || my < wy - m || my > wb + m)
        return RES_NONE;

    if (mx <= wx + cm && my <= wy + cm) return RES_NW;
    if (mx >= we - cm && my <= wy + cm) return RES_NE;
    if (mx <= wx + cm && my >= wb - cm) return RES_SW;
    if (mx >= we - cm && my >= wb - cm) return RES_SE;

    if (mx > wx + m && mx < we - m && my > wy + m && my < wb - m)
        return RES_NONE;

    if (my < wy + m) return RES_N;
    if (my > wb - m) return RES_S;
    if (mx < wx + m) return RES_W;
    if (mx > we - m) return RES_E;

    return RES_NONE;
}

static void win_do_resize(window_t *w, int32_t mx, int32_t my) {
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
    int64_t  mw   = (int64_t)MIN_WIN_W;
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
    win_draw_chrome(w, w->type != WIN_FILES);
    win_render_content(w);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void gui_init(void) {
    uint64_t fb_w = console_fb_width();

    /* Initialize z-order: 0=bottom ... MAX_WINS-1=top */
    for (int i = 0; i < MAX_WINS; i++) g_z[i] = i;

    g_wins[0].active = true;
    g_wins[0].type   = WIN_TERM;
    g_wins[0].title  = "Terminal";
    g_wins[0].state  = WIN_HIDDEN;

    g_wins[1].active = true;
    g_wins[1].type   = WIN_FILES;
    /* Title points into fb.path — auto-updates as user navigates */
    g_wins[1].title  = g_wins[1].fb.path;
    g_wins[1].state  = WIN_HIDDEN;

    g_wins[2].active = true;
    g_wins[2].type   = WIN_SETTINGS;
    g_wins[2].title  = "Settings";
    g_wins[2].state  = WIN_HIDDEN;

    g_wins[3].active = true;
    g_wins[3].type   = WIN_TEXT;
    g_wins[3].title  = "Viewer";
    g_wins[3].state  = WIN_HIDDEN;

    console_fill_rect(0, desk_top(), fb_w, desk_avail(), COL_DESKTOP);
    taskbar_draw();
    fb_load(&g_wins[1].fb, "/");
    win_show(&g_wins[0], 0);
}

void gui_on_tick(void) {
    g_gui_tick++;

    /* ── Uptime: redraw status bar once per second ── */
    {
        static uint64_t s_last_sec = (uint64_t)-1;
        uint64_t hz = pit_get_hz();
        if (!hz) hz = 100;
        uint64_t now_sec = pit_ticks() / hz;
        if (now_sec != s_last_sec) {
            s_last_sec = now_sec;
            draw_status_bar();
        }
    }

    int32_t mx, my;
    bool lbtn, rbtn;
    mouse_get_state(&mx, &my, &lbtn, &rbtn);

    bool btn_pressed  = lbtn && !g_prev_lbtn;
    bool btn_released = !lbtn && g_prev_lbtn;
    g_prev_lbtn = lbtn;

    /* Track right-button edge */
    static bool g_prev_rbtn = false;
    bool rbtn_pressed = rbtn && !g_prev_rbtn;
    g_prev_rbtn = rbtn;

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
    uint64_t ty   = fb_h - TASKBAR_H;

    /* ── Hover tracking for file browser (z-order top-to-bottom) ── */
    if (!g_dragging && !g_resizing && !g_launcher_open) {
        for (int zi = MAX_WINS - 1; zi >= 0; zi--) {
            int si = g_z[zi];
            window_t *w = &g_wins[si];
            if (!w->active || w->state == WIN_HIDDEN || w->type != WIN_FILES) continue;
            fb_on_motion(w, mx, my);
            break;
        }
    }

    /* ── Chrome hover tracking ── */
    if (!g_dragging && !g_resizing) {
        int new_chrome_win = -1;
        int new_chrome_btn = 0;

        for (int zi = MAX_WINS - 1; zi >= 0; zi--) {
            int si = g_z[zi];
            window_t *w = &g_wins[si];
            if (!w->active || w->state == WIN_HIDDEN) continue;

            int32_t wy = (int32_t)w->y;
            int32_t wx = (int32_t)w->x;
            int32_t we = wx + (int32_t)w->w;

            if (my >= wy && my < wy + (int32_t)TITLE_H &&
                mx >= wx && mx < we) {
                int32_t clx = (int32_t)w->btn_cls_x;
                int32_t mxx = (int32_t)w->btn_max_x;
                int32_t mnx = (int32_t)w->btn_min_x;
                new_chrome_win = si;
                if (mx >= clx && mx < clx + (int32_t)BTN_W)
                    new_chrome_btn = 1;
                else if (mx >= mxx && mx < mxx + (int32_t)BTN_W)
                    new_chrome_btn = 2;
                else if (mx >= mnx && mx < mnx + (int32_t)BTN_W)
                    new_chrome_btn = 3;
                else
                    new_chrome_btn = 0;
                break;
            }
            /* Only check topmost window that contains this x,y in its bounds */
            if (my >= wy && my < wy + (int32_t)w->h &&
                mx >= wx && mx < we) {
                break;
            }
        }

        if (new_chrome_win != g_chrome_win || new_chrome_btn != g_chrome_btn) {
            int old_win = g_chrome_win;
            g_chrome_win = new_chrome_win;
            g_chrome_btn = new_chrome_btn;
            /* Redraw old hovered window's chrome */
            if (old_win >= 0 && old_win < MAX_WINS &&
                g_wins[old_win].active && g_wins[old_win].state != WIN_HIDDEN)
                win_draw_chrome(&g_wins[old_win], false);
            /* Redraw new hovered window's chrome */
            if (g_chrome_win >= 0 && g_chrome_win < MAX_WINS &&
                g_wins[g_chrome_win].active && g_wins[g_chrome_win].state != WIN_HIDDEN)
                win_draw_chrome(&g_wins[g_chrome_win], false);
        }
    }

    /* ── Resize edge hover tracking ── */
    if (!g_dragging && !g_resizing && !g_launcher_open) {
        int          new_rw = -1;
        resize_dir_t new_rd = RES_NONE;
        for (int zi = MAX_WINS - 1; zi >= 0; zi--) {
            int si = g_z[zi];
            window_t *w = &g_wins[si];
            if (!w->active || w->state == WIN_HIDDEN) continue;
            resize_dir_t rd = hit_resize(w, mx, my);
            if (rd != RES_NONE) { new_rw = si; new_rd = rd; break; }
            if ((uint64_t)mx >= w->x && (uint64_t)mx < w->x + w->w &&
                (uint64_t)my >= w->y && (uint64_t)my < w->y + w->h) break;
        }
        if (new_rw != g_resize_hover_win || new_rd != g_resize_hover_dir) {
            g_resize_hover_win = new_rw;
            g_resize_hover_dir = new_rd;
            full_redraw();
        }
    }

    /* ── Launcher hover tracking ── */
    if (g_launcher_open) {
        uint64_t lx = launcher_lx();
        uint64_t ly = launcher_ly();
        int new_hover = -1;
        if ((uint64_t)mx >= lx && (uint64_t)mx < lx + LAUNCHER_W &&
            (uint64_t)my >= ly &&
            (uint64_t)my < ly + LAUNCHER_ITEMS * LAUNCHER_ITEM_H) {
            new_hover = (int)((uint64_t)my - ly) / (int)LAUNCHER_ITEM_H;
        }
        if (new_hover != g_launcher_hover) {
            g_launcher_hover = new_hover;
            launcher_draw();
        }
    }

    /* ── Taskbar button hover tracking ── */
    if (!g_launcher_open) {
        int new_tbhov = -1;
        if ((uint64_t)my >= ty) {
            for (int s = 0; s < 3; s++) {
                uint64_t bx = TASKBTN_X + (uint64_t)s * (TASKBTN_W + TASKBTN_GAP);
                if ((uint64_t)mx >= bx && (uint64_t)mx < bx + TASKBTN_W) {
                    new_tbhov = s; break;
                }
            }
        }
        if (new_tbhov != g_taskbtn_hover) {
            static const char *tbnames[] = { "Terminal", "Files", "Settings" };
            int old = g_taskbtn_hover;
            g_taskbtn_hover = new_tbhov;
            if (old >= 0 && old < 3) taskbar_draw_btn(old, tbnames[old]);
            if (new_tbhov >= 0 && new_tbhov < 3) taskbar_draw_btn(new_tbhov, tbnames[new_tbhov]);
        }
    }

    /* ── Context menu hover tracking ── */
    if (g_ctx_open) {
        uint64_t ctx_x = (uint64_t)g_ctx_x;
        uint64_t ctx_y = (uint64_t)g_ctx_y;
        int new_chov = -1;
        if ((uint64_t)mx >= ctx_x && (uint64_t)mx < ctx_x + CTX_W &&
            (uint64_t)my >= ctx_y + 1u &&
            (uint64_t)my < ctx_y + 1u + CTX_ITEMS * CTX_ITEM_H) {
            new_chov = (int)((uint64_t)my - (ctx_y + 1u)) / (int)CTX_ITEM_H;
        }
        if (new_chov != g_ctx_hover) {
            g_ctx_hover = new_chov;
            ctx_draw();
        }
    }

    /* ── Keyboard capture + input for focused non-terminal window ── */
    {
        /* Find frontmost non-terminal visible window using z-order */
        window_t *focused = NULL;
        for (int zi = MAX_WINS - 1; zi >= 0; zi--) {
            int ki = g_z[zi];
            window_t *kw = &g_wins[ki];
            if (kw->active && kw->state != WIN_HIDDEN && kw->type != WIN_TERM) {
                focused = kw; break;
            }
        }

        /* Manage GUI keyboard capture based on window visibility */
        static bool s_gui_cap = false;
        bool want_cap = (focused != NULL);
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
                /* ── Global: Alt+Tab cycles visible windows ── */
                if ((uint8_t)ch == KEY_ALTTAB) {
                    int vis[MAX_WINS], vc = 0;
                    for (int zi = 0; zi < MAX_WINS; zi++) {
                        int si = g_z[zi];
                        if (g_wins[si].active && g_wins[si].state != WIN_HIDDEN)
                            vis[vc++] = si;
                    }
                    if (vc >= 2) { z_raise(vis[vc - 2]); full_redraw(); }
                    continue;
                }
                if (!focused) continue;
                if (focused->type == WIN_FILES) {
                    if (focused->fb.search_active) {
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
                            focused->fb.sel_row = sel;
                            if (sel < focused->fb.scroll)
                                focused->fb.scroll = sel;
                            changed = true;
                        } else if (ch == KEY_DOWN) {
                            if (sel < 0) sel = focused->fb.scroll;
                            else if (sel < n - 1) sel++;
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
                                    if (fb_has_ext(name, ".txt") || fb_has_ext(name, ".log") ||
                                        fb_has_ext(name, ".md")  || fb_has_ext(name, ".ini") ||
                                        fb_has_ext(name, ".cfg") || fb_has_ext(name, ".sh")) {
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
                        } else if (ch == '\b' || ch == 127) {
                            /* Backspace: go up one directory */
                            bool can_up2 = !(focused->fb.path[0]=='/'&&focused->fb.path[1]=='\0');
                            if (can_up2) {
                                char parent[128];
                                fb_path_parent(parent, focused->fb.path);
                                fb_navigate(&focused->fb, parent);
                                changed = true;
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
                    if (ch == KEY_UP) {
                        if (focused->text.scroll > 0) { focused->text.scroll--; changed = true; }
                    } else if (ch == KEY_DOWN) {
                        focused->text.scroll++; changed = true;
                    } else if (ch == KEY_PGUP) {
                        focused->text.scroll -= 10;
                        if (focused->text.scroll < 0) focused->text.scroll = 0;
                        changed = true;
                    } else if (ch == KEY_PGDN) {
                        focused->text.scroll += 10; changed = true;
                    } else if (ch == KEY_HOME) {
                        if (focused->text.scroll) { focused->text.scroll = 0; changed = true; }
                    } else if (ch == KEY_END) {
                        focused->text.scroll = focused->text.total_lines; changed = true;
                    } else if (ch == 27 || ch == 'q' || ch == 23) { /* ESC, q, Ctrl+W */
                        int slot = (int)(focused - g_wins);
                        win_hide(focused, slot);
                        focused = NULL; closed = true; break;
                    }
                } else if (focused->type == WIN_SETTINGS) {
                    if (ch == 27 || ch == 23) { /* ESC or Ctrl+W */
                        int slot = (int)(focused - g_wins);
                        win_hide(focused, slot);
                        focused = NULL; closed = true; break;
                    }
                }
            }
            if (changed && !closed) {
                if (focused && focused->type == WIN_FILES) {
                    fb_render(focused);
                } else if (focused && focused->type == WIN_TEXT) {
                    text_render(focused);
                }
            }
        }
    }

    /* ── Mouse scroll wheel ── */
    {
        int8_t scroll = mouse_consume_scroll();
        if (scroll) {
            /* Find topmost visible window under cursor to scroll */
            for (int zi = MAX_WINS - 1; zi >= 0; zi--) {
                int si = g_z[zi];
                window_t *w = &g_wins[si];
                if (!w->active || w->state == WIN_HIDDEN) continue;
                if ((uint64_t)mx < w->x || (uint64_t)mx >= w->x + w->w ||
                    (uint64_t)my < w->y || (uint64_t)my >= w->y + w->h) continue;
                if (w->type == WIN_FILES) {
                    w->fb.scroll_vel += (int32_t)scroll * 24;
                    if (w->fb.scroll_vel >  2048) w->fb.scroll_vel =  2048;
                    if (w->fb.scroll_vel < -2048) w->fb.scroll_vel = -2048;
                } else if (w->type == WIN_TEXT) {
                    w->text.scroll_vel += (int32_t)scroll * 24;
                    if (w->text.scroll_vel >  2048) w->text.scroll_vel =  2048;
                    if (w->text.scroll_vel < -2048) w->text.scroll_vel = -2048;
                }
                break;
            }
        }
    }

    /* ── Taskbar clicks ── */
    if (btn_pressed && (uint64_t)my >= ty) {
        int32_t cx, cy;

        if (mx >= (int32_t)LOGO_X && mx < (int32_t)(LOGO_X + LOGO_W)) {
            g_launcher_open = !g_launcher_open;
            g_launcher_hover = -1;
            if (g_launcher_open) {
                taskbar_draw();
                launcher_draw();
            } else {
                full_redraw();
            }
            mouse_consume_click(&cx, &cy);
            return;
        }

        for (int s = 0; s < 3; s++) {
            uint64_t bx = TASKBTN_X + (uint64_t)s * (TASKBTN_W + TASKBTN_GAP);
            if (mx >= (int32_t)bx && mx < (int32_t)(bx + TASKBTN_W)) {
                window_t *w = &g_wins[s];
                if (w->state == WIN_HIDDEN) {
                    z_raise(s);
                    win_show(w, s);
                } else {
                    win_hide(w, s);
                }
                break;
            }
        }
        mouse_consume_click(&cx, &cy);
        return;
    }

    /* ── Right-click: context menu on desktop ── */
    if (rbtn_pressed && (uint64_t)my < ty) {
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
            /* Close launcher if open */
            if (g_launcher_open) {
                g_launcher_open = false;
                g_launcher_hover = -1;
            }
            /* Clamp context menu to screen */
            int32_t ctx_x = mx;
            int32_t ctx_y = my;
            if ((uint64_t)ctx_x + CTX_W > fb_w)
                ctx_x = (int32_t)(fb_w - CTX_W);
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

    /* ── Launcher popup clicks ── */
    if (btn_pressed && g_launcher_open) {
        int32_t cx, cy;
        uint64_t lx = launcher_lx();
        uint64_t ly = launcher_ly();
        bool inside = ((uint64_t)mx >= lx && (uint64_t)mx < lx + LAUNCHER_W &&
                       (uint64_t)my >= ly &&
                       (uint64_t)my < ly + LAUNCHER_ITEMS * LAUNCHER_ITEM_H);
        g_launcher_open = false;
        g_launcher_hover = -1;
        if (inside) {
            int item = (int)((uint64_t)my - ly) / (int)LAUNCHER_ITEM_H;
            if (item >= 0 && item < (int)LAUNCHER_ITEMS) {
                window_t *w = &g_wins[item];
                z_raise(item);
                if (w->state == WIN_HIDDEN)
                    win_show(w, item);
                else
                    full_redraw();
            } else {
                full_redraw();
            }
            mouse_consume_click(&cx, &cy);
            return;
        }
        full_redraw();
        /* fall through to window hit test */
    }

    /* ── Context menu clicks ── */
    if (btn_pressed && g_ctx_open) {
        int32_t cx, cy;
        uint64_t ctx_x = (uint64_t)g_ctx_x;
        uint64_t ctx_y = (uint64_t)g_ctx_y;
        bool inside = ((uint64_t)mx >= ctx_x && (uint64_t)mx < ctx_x + CTX_W &&
                       (uint64_t)my >= ctx_y + 1u &&
                       (uint64_t)my < ctx_y + 1u + CTX_ITEMS * CTX_ITEM_H);
        g_ctx_open = false;
        if (inside) {
            int item = (int)((uint64_t)my - (ctx_y + 1u)) / (int)CTX_ITEM_H;
            if (item >= 0 && item < (int)CTX_ITEMS) {
                window_t *w = &g_wins[item];
                z_raise(item);
                if (w->state == WIN_HIDDEN)
                    win_show(w, item);
                else
                    full_redraw();
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
    if (g_sb_drag && g_sb_drag_win >= 0) {
        window_t *w = &g_wins[g_sb_drag_win];
        if (btn_released) {
            g_sb_drag = false;
            g_sb_drag_win = -1;
        } else if (g_sb_drag_lh > 0) {
            int64_t dy = (int64_t)my - (int64_t)g_sb_drag_y0;
            int ns = g_sb_drag_s0 + (int)(dy * (int64_t)g_sb_drag_max
                                          / (int64_t)g_sb_drag_lh);
            if (ns < 0) ns = 0;
            if (ns > g_sb_drag_max) ns = g_sb_drag_max;
            if (g_sb_drag_text) {
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
            /* Snap to screen edges */
            if ((uint64_t)new_x < SNAP_DIST) new_x = 0;
            if ((uint64_t)(new_x + (int32_t)w->w) + SNAP_DIST > fb_w2)
                new_x = (int32_t)(fb_w2 - w->w);
            if ((uint64_t)new_y < desk_top() + SNAP_DIST) new_y = (int32_t)desk_top();
            if ((uint64_t)(new_y + (int32_t)w->h) + SNAP_DIST > desk_bot())
                new_y = (int32_t)(desk_bot() - w->h);
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
            if (w->x != old_wx || w->y != old_wy || g_snap_preview != old_snap)
                full_redraw();
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

    /* ── Per-window hit tests (z-order top-to-bottom) ── */
    if (btn_pressed) {
        bool hit_any = false;
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

            hit_any = true;

            /* Raise window to top on any click */
            bool was_top = (g_z[MAX_WINS - 1] == si);
            z_raise(si);
            if (!was_top) {
                /* Redraw chrome of previously-top window (now dimmed) and this one */
                int prev_top = g_z[MAX_WINS - 2];
                if (g_wins[prev_top].active && g_wins[prev_top].state != WIN_HIDDEN)
                    win_draw_chrome(&g_wins[prev_top], false);
                win_draw_chrome(w, false);
            }

            if (in_tb && mx >= clx && mx < clx + (int32_t)BTN_W) {
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
                        g_last_click_tick = g_gui_tick;
                        g_last_click_win  = si;
                        if (dbl) {
                            win_maximize_toggle(w);
                        } else {
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
                        }
                    } else if (w->state == WIN_MAXIMIZED) {
                        win_maximize_toggle(w);
                    }
                } else if (w->type == WIN_FILES && in_win && !in_tb) {
                    fb_on_click(w, mx, my);
                } else if (w->type == WIN_TEXT && in_win && !in_tb) {
                    /* Click on text viewer scrollbar: thumb drag or track jump */
                    uint64_t fix = w->x + BORDER;
                    uint64_t fiy = w->y + TITLE_H;
                    uint64_t fiw = w->w - 2u * BORDER;
                    uint64_t fih = w->h - TITLE_H - BORDER;
                    uint64_t sbx = fix + fiw - 8u;
                    if (w->text.total_lines > 0 &&
                        (uint64_t)mx >= sbx && (uint64_t)mx < sbx + 8u &&
                        (uint64_t)my >= fiy && (uint64_t)my < fiy + fih) {
                        uint64_t max_r = fih > 2u * PAD ? (fih - 2u * PAD) / console_font_height() : 1u;
                        int max_sc = w->text.total_lines - (int)max_r;
                        if (max_sc < 0) max_sc = 0;
                        uint64_t th = (max_r * fih) / (uint64_t)w->text.total_lines;
                        if (th < 8u) th = 8u;
                        uint64_t ty = fiy + ((uint64_t)w->text.scroll * (fih - th))
                                          / (uint64_t)(max_sc > 0 ? max_sc : 1);
                        if ((uint64_t)my >= ty && (uint64_t)my < ty + th) {
                            /* Thumb drag — cancel inertia */
                            w->text.scroll_vel = 0; w->text.scroll_acc = 0;
                            g_sb_drag      = true;
                            g_sb_drag_win  = si;
                            g_sb_drag_y0   = my;
                            g_sb_drag_s0   = w->text.scroll;
                            g_sb_drag_lh   = fih;
                            g_sb_drag_ly   = fiy;
                            g_sb_drag_max  = max_sc;
                            g_sb_drag_text = true;
                        } else {
                            /* Track click: jump */
                            int ns = (int)(((uint64_t)my - fiy) *
                                           (uint64_t)w->text.total_lines / fih);
                            if (ns < 0) ns = 0;
                            if (ns > max_sc) ns = max_sc;
                            w->text.scroll = ns;
                            text_render(w);
                        }
                    }
                }
            }
            break;
        }
        (void)hit_any;
        int32_t cx, cy;
        mouse_consume_click(&cx, &cy);
    }

    /* ── Inertial scroll tick ── */
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
                fb_render(w);
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
                text_render(w);
            }
            w->text.scroll_vel = w->text.scroll_vel * 7 / 8;
            if (w->text.scroll_vel > -16 && w->text.scroll_vel < 16) {
                w->text.scroll_vel = 0;
                w->text.scroll_acc = 0;
            }
        }
    }
}
