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
#include "net.h"
#include "rtc.h"
#include "hda.h"
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
#define LAUNCHER_ITEMS  4u
#define LAUNCHER_W      100u
#define CTX_W           110u
#define CTX_ITEM_H      22u
#define CTX_ITEMS       4u

/* File browser context menu */
#define FB_CTX_W         120u
#define FB_CTX_MAX_ITEMS  11
#define FB_CTX_ACT_OPEN    0
#define FB_CTX_ACT_EDIT    1
#define FB_CTX_ACT_RENAME  2
#define FB_CTX_ACT_DELETE  3
#define FB_CTX_ACT_NEW_FILE 4
#define FB_CTX_ACT_NEW_DIR  5
#define FB_CTX_ACT_REFRESH  6
#define FB_CTX_ACT_COPY    7
#define FB_CTX_ACT_CUT     8
#define FB_CTX_ACT_PASTE   9
#define FB_CTX_ACT_COPY_PATH 10

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
#define COL_FB_BTN_HOV   0x00304c68u
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

/* ── Theme ───────────────────────────────────────────────────────────── */
#define WALLPAPER_GRADIENT  0   /* default: dark blue gradient + dot grid */
#define WALLPAPER_SOLID     1   /* flat dark colour */
#define WALLPAPER_STARS     2   /* static starfield */
#define WALLPAPER_GRID      3   /* fine grid lines */
#define WALLPAPER_WAVES     4   /* diagonal wave stripes */
#define WALLPAPER_COUNT     5

typedef struct {
    uint32_t accent;        /* primary accent colour (borders, highlights) */
    int      wallpaper;     /* one of WALLPAPER_* */
    bool     clock_12h;     /* true = 12-hour AM/PM format */
    bool     animations;    /* true = window open/close animations */
    bool     statusbar;     /* true = show top status bar */
    bool     desktop_info;  /* true = show neofetch-style info on desktop */
    int8_t   utc_offset;    /* UTC hour offset applied to RTC time, -12..+14 */
} gui_theme_t;

/* Default theme: FiFi blue accent, gradient wallpaper */
static gui_theme_t g_theme = { 0x003060c0u, WALLPAPER_GRADIENT, false, true, true, true, 0 };

/* 16 accent colour presets */
#define ACCENT_PRESET_COUNT 16
static const uint32_t g_accent_presets[ACCENT_PRESET_COUNT] = {
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

/* ── Types ───────────────────────────────────────────────────────────── */
typedef enum { WIN_HIDDEN, WIN_NORMAL, WIN_MAXIMIZED } win_state_t;
typedef enum { WIN_TERM, WIN_FILES, WIN_TEXT, WIN_SETTINGS } win_type_t;
typedef enum { ANIM_NONE, ANIM_OPEN, ANIM_CLOSE } anim_phase_t;
#define ANIM_TICKS 5
/* Ease-out for open (36→64→84→96→100), ease-in for close (reversed) — scale 0..100 */
static const int g_anim_open_scale[ANIM_TICKS]  = { 36, 64, 84, 96, 100 };
static const int g_anim_close_scale[ANIM_TICKS] = { 64, 36, 16,  4,   0 };

typedef enum {
    RES_NONE,
    RES_N, RES_S, RES_E, RES_W,
    RES_NE, RES_NW, RES_SE, RES_SW
} resize_dir_t;

typedef enum { SYN_LANG_NONE, SYN_LANG_C, SYN_LANG_SH, SYN_LANG_PY, SYN_LANG_ASM, SYN_LANG_JSON, SYN_LANG_LUA, SYN_LANG_JS, SYN_LANG_MAKE, SYN_LANG_TOML, SYN_LANG_YAML, SYN_LANG_HTML, SYN_LANG_CSS, SYN_LANG_INI, SYN_LANG_MD, SYN_LANG_DIFF, SYN_LANG_SQL, SYN_LANG_RUST } syn_lang_t;

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
    bool       srch_active;
    bool       srch_is_goto;     /* true = "goto line" mode, false = search */
    bool       srch_is_repl;     /* true = find+replace mode (edit mode only) */
    bool       srch_case_fold;   /* true = case-insensitive search */
    bool       repl_focused;     /* true = cursor in replace field */
    char       srch_buf[64];
    int        srch_len;
    char       repl_buf[64];
    int        repl_len;
    int        srch_match_line;  /* -1 = no match */
    int        srch_match_col;
    int        srch_total_count; /* total occurrences in file (0 when no query) */
    int        srch_cur_idx;     /* 1-based index of active match (0 = none) */
    int        h_scroll;         /* horizontal scroll: chars from line start */
    int        max_line_len;     /* longest line in chars (for h_scroll clamping) */
    bool       word_wrap;        /* word-wrap toggle (W key) */
    syn_lang_t lang;             /* syntax highlighting language */
    /* Edit mode */
    bool       edit_mode;
    bool       edit_modified;
    uint8_t   *edit_buf;         /* mutable copy of file (kmalloc'd) */
    uint32_t   edit_size;        /* used bytes in edit_buf */
    uint32_t   edit_cap;         /* allocated capacity */
    uint32_t   edit_cur;         /* cursor: byte offset in edit_buf */
    uint32_t   edit_want_col;    /* desired col for up/down (sticky) */
    int        edit_cur_line;    /* cached: line number of cursor */
    int        edit_cur_col;     /* cached: column of cursor */
    int32_t    sel_anchor;       /* byte offset where selection started (-1 = no selection) */
    int32_t    sel_end;          /* byte offset of selection end (exclusive) */
    /* Undo ring buffer — snapshots pushed before each edit */
#define UNDO_DEPTH 16
    struct { uint8_t *data; uint32_t size; uint32_t cursor; } undo_ring[UNDO_DEPTH];
    int        undo_head;        /* next slot to write into (ring) */
    int        undo_count;       /* number of valid entries */
    bool       undo_in_group;    /* true = consecutive char inserts, skip re-push */
    /* Redo ring (populated by undo; cleared on any new edit) */
    struct { uint8_t *data; uint32_t size; uint32_t cursor; } redo_ring[UNDO_DEPTH];
    int        redo_head;
    int        redo_count;
    /* Save-as overlay (edit mode, no path set) */
    bool       save_as_active;
    char       save_as_buf[TV_PATH_MAX];
    int        save_as_len;
    /* Open-by-path bar (Ctrl+O) */
    bool       open_bar_active;
    char       open_bar_buf[TV_PATH_MAX];
    int        open_bar_len;
    /* Welcome screen recent-file hover row (-1 = none) */
    int        welcome_hover;
} text_state_t;

#define FB_MAX_ENTRIES  96
#define FB_HIST_MAX      8
#define FB_SEARCH_MAX   64
#define FB_VIEW_LIST    0   /* detail list view */
#define FB_VIEW_ICONS   1   /* icon grid view */

typedef struct {
    char path[128];
    char     entries[FB_MAX_ENTRIES][128];
    bool     is_dir[FB_MAX_ENTRIES];
    uint32_t file_sizes[FB_MAX_ENTRIES];   /* bytes; 0 for dirs */
    int      entry_count;
    int  scroll;
    int  hover_row;   /* -1 = none */
    int  sel_row;     /* -1 = none */
    /* navigation history (back stack + forward stack) */
    char hist[FB_HIST_MAX][128];
    int  hist_depth;
    char fwd_hist[FB_HIST_MAX][128];
    int  fwd_depth;
    /* search */
    char search_query[FB_SEARCH_MAX];
    int  search_len;
    bool search_active;
    /* inertial scroll */
    int32_t scroll_vel; /* fp16 velocity (1/16 lines per tick) */
    int32_t scroll_acc; /* sub-line accumulator */
    /* path bar hover: char position in path string (-1 = not hovering) */
    int  path_hov_char;
    /* new file/dir/rename input overlay */
    bool input_active;
    bool input_isdir;
    bool input_is_rename;
    char input_orig[128];   /* original name when renaming */
    char input_buf[128];
    int  input_len;
    int  input_cursor;  /* caret position within input_buf [0..input_len] */
    /* display options */
    bool show_hidden;  /* show files/dirs starting with '.' */
    /* sort: 0=name, 1=size */
    int  sort_by;
    bool sort_rev;
    int  header_hover; /* -1=none, 0=Name col, 1=Size col */
    int  toolbar_hover; /* -1=none, 0=back, 1=fwd, 2=up, 3=refresh, 4=pathbar */
    /* multi-selection */
    bool multi_sel[FB_MAX_ENTRIES];  /* additional selected entries beyond sel_row */
    int  sel_anchor;                 /* anchor row for shift-click range select */
    /* resizable size column */
    int  size_col_chars;   /* width of size column in char units (default 7, range 4..16) */
    bool col_drag_active;
    int  col_drag_start_x;
    int  col_drag_start_chars;
    /* view mode */
    int  view_mode;        /* FB_VIEW_LIST or FB_VIEW_ICONS */
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
    anim_phase_t anim_phase;
    int          anim_step;   /* 1..ANIM_TICKS; 0 unused (use ANIM_NONE) */
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
static bool      g_text_drag_sel = false;  /* dragging to extend text selection */
static int       g_text_drag_win = -1;
static uint64_t  g_text_drag_scroll_tick = 0;  /* rate-limit auto-scroll: max 12/sec */
static int32_t   g_drag_off_x  = 0;
static int32_t   g_drag_off_y  = 0;
/* Shadow buffer for smooth window drag */
static uint32_t *g_drag_shadow = NULL;
static uint64_t  g_drag_shad_w = 0;
static uint64_t  g_drag_shad_h = 0;
/* In-kernel clipboard for text editor copy/paste */
static uint8_t  *g_clipboard     = NULL;
static uint32_t  g_clipboard_len = 0;
static bool      g_prev_lbtn   = false;
static int       g_snap_preview = 0; /* 0=none, 1=left-half, 2=right-half */

/* Scrollbar drag state */
static bool     g_sb_drag      = false;
static int      g_sb_drag_win  = -1;
static int32_t  g_sb_drag_y0   = 0;
static int32_t  g_sb_drag_x0   = 0;
static int      g_sb_drag_s0   = 0;
static uint64_t g_sb_drag_range = 0; /* track_h/w - thumb_h/w: pixels for full scroll range */
static int      g_sb_drag_max  = 0;  /* max scroll value */
static bool     g_sb_drag_text = false; /* true=text viewer, false=files */
static bool     g_sb_drag_horiz = false; /* true=horizontal scrollbar */

/* Terminal scrollback scrollbar drag */
static bool     g_term_sb_drag   = false;
static int32_t  g_term_sb_drag_y0 = 0;
static int      g_term_sb_drag_s0 = 0;
static uint64_t g_term_sb_drag_range = 0;
static int      g_term_sb_drag_max   = 0;

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

/* ── Settings scroll ─────────────────────────────────────────────────── */
static int g_settings_scroll = 0;  /* pixel offset from top */

/* ── Terminal scrollback ─────────────────────────────────────────────── */
static int g_term_scroll = 0;  /* lines scrolled back (0 = live view) */

/* ── Font selector state ─────────────────────────────────────────────── */
static int g_font_idx = 0;
static const char *g_font_paths[] = {
    "/fonts/ter16b.psf", "/fonts/ter20b.psf", "/fonts/ter24b.psf",
    "/fonts/default.psf", NULL
};
static const char *g_font_labels[] = {
    "Terminus Bold 8x16", "Terminus Bold 10x20", "Terminus Bold 12x24",
    "Default 8x16", NULL
};
/* Absolute screen coords of font prev/next buttons (updated each settings render) */
static uint64_t g_font_prev_bx = 0, g_font_next_bx = 0;
static uint64_t g_font_btn_by  = 0, g_font_btn_bw  = 28u, g_font_btn_bh = 0u;
static uint64_t g_utc_minus_bx = 0, g_utc_plus_bx = 0;
static uint64_t g_utc_btn_by   = 0, g_utc_btn_bh  = 0u;
static uint64_t g_vol_minus_bx = 0, g_vol_plus_bx  = 0;
static uint64_t g_vol_btn_by   = 0, g_vol_btn_bh   = 0u;
static uint64_t g_vol_chime_bx = 0, g_vol_chime_by = 0;
static uint64_t g_vol_chime_bw = 0, g_vol_chime_bh = 0u;

/* ── Chrome hover state ──────────────────────────────────────────────── */
static int g_chrome_win = -1;
static int g_chrome_btn = 0;  /* 0=none, 1=close, 2=max, 3=min */

/* ── Double/triple-click state ───────────────────────────────────────── */
static uint64_t g_last_click_tick  = 0;
static int      g_last_click_win   = -1;
static int      g_last_click_count = 0;  /* consecutive click count (1/2/3+) */

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

/* ── Text editor context menu ───────────────────────────────────────── */
#define TXT_CTX_W        100u
#define TXT_CTX_ITEMS    5
static bool    g_txt_ctx_open  = false;
static int32_t g_txt_ctx_x    = 0;
static int32_t g_txt_ctx_y    = 0;
static int     g_txt_ctx_win  = -1;
static int     g_txt_ctx_hover = -1;

/* ── File browser context menu ───────────────────────────────────────── */
static bool g_fb_ctx_open  = false;
static int32_t g_fb_ctx_x  = 0;
static int32_t g_fb_ctx_y  = 0;
static int  g_fb_ctx_win   = -1;  /* window slot */
static int  g_fb_ctx_row   = -1;  /* file row (-1 = empty area) */
static bool g_fb_ctx_is_dir = false;
static int  g_fb_ctx_hover = -1;
static int  g_fb_ctx_n     = 0;
static int  g_fb_ctx_acts[FB_CTX_MAX_ITEMS];
static char g_fb_clip_path[256] = "";   /* file browser clipboard: source path */
static bool g_fb_clip_is_cut    = false; /* true=move, false=copy */

/* ── Recent files ────────────────────────────────────────────────────── */
#define RECENT_MAX 8
static char g_recent[RECENT_MAX][128];
static int  g_recent_count = 0;

/* ── Resize edge hover ───────────────────────────────────────────────── */
static int          g_resize_hover_win = -1;
static resize_dir_t g_resize_hover_dir = RES_NONE;

/* ── Theme settings UI hit boxes (updated each settings render) ──────── */
static uint64_t g_theme_accent_bx[ACCENT_PRESET_COUNT]; /* left x of each accent swatch */
static uint64_t g_theme_accent_by;      /* common y of swatches row */
static uint64_t g_theme_swatch_sz;      /* swatch size in pixels */
static uint64_t g_theme_accent_by2;     /* y of second accent row */
static uint64_t g_theme_wall_bx[WALLPAPER_COUNT]; /* wallpaper button x */
static uint64_t g_theme_wall_by;
static uint64_t g_theme_wall_bw;
static uint64_t g_theme_wall_bh;
/* toggle buttons: clock format, animations, status bar */
static uint64_t g_theme_toggle_x[4], g_theme_toggle_y[4];
static uint64_t g_theme_toggle_w, g_theme_toggle_h;

/* ── GUI tick counter ────────────────────────────────────────────────── */
static uint64_t g_gui_tick = 0;

/* ── Toast notification ──────────────────────────────────────────────── */
#define TOAST_TICKS  200u  /* ~2 s at 100 Hz */
static char     g_toast_msg[64] = {0};
static uint32_t g_toast_color   = 0x00c8d8ffu;
static int      g_toast_ticks   = 0;

static void gui_toast(const char *msg, uint32_t color) {
    int i = 0;
    while (msg[i] && i < 62) { g_toast_msg[i] = msg[i]; i++; }
    g_toast_msg[i] = '\0';
    g_toast_color  = color;
    g_toast_ticks  = (int)TOAST_TICKS;
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static uint64_t desk_top(void)   { return g_theme.statusbar ? STATUS_H : 0u; }
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

/* IPv4 uint32 (host byte order) to "x.x.x.x\0" in buf (need 16 bytes) */
static void gui_ip4_str(uint32_t ip, char *buf, int bufsz) {
    char tmp[16]; int ti = 0;
    for (int byte = 3; byte >= 0; byte--) {
        uint32_t octet = (ip >> (uint32_t)(byte * 8)) & 0xFFu;
        char nb[4]; int ni = 0;
        if (octet == 0) { nb[ni++] = '0'; }
        else { uint32_t v = octet; while (v > 0) { nb[ni++] = '0' + (int)(v % 10); v /= 10; } }
        for (int k = ni - 1; k >= 0 && ti < 14; k--) tmp[ti++] = nb[k];
        if (byte > 0 && ti < 14) tmp[ti++] = '.';
    }
    tmp[ti] = '\0';
    int i = 0; while (tmp[i] && i < bufsz - 1) { buf[i] = tmp[i]; i++; } buf[i] = '\0';
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
                     (g_wins[slot].state != WIN_HIDDEN ||
                      g_wins[slot].anim_phase == ANIM_CLOSE));
    bool     hov  = (g_taskbtn_hover == slot);
    /* Focused = topmost visible window */
    bool focused = (vis && g_z[MAX_WINS - 1] == slot);
    uint32_t bg   = focused ? 0x003878d8u :
                    vis     ? COL_TASKBTN_A :
                    hov     ? 0x00283848u : COL_TASKBTN;

    console_fill_rect(bx, ty + 3u, TASKBTN_W, TASKBAR_H - 6u, bg);
    /* Active indicator bar at bottom */
    if (vis)
        console_fill_rect(bx, ty + TASKBAR_H - 5u, TASKBTN_W, 3u,
                          focused ? 0x0060a0f0u : COL_BORDER);
    uint64_t llen     = (uint64_t)gui_strlen(label);
    uint64_t max_ch   = TASKBTN_W / fw;
    uint64_t disp_len = llen < max_ch ? llen : max_ch;
    uint64_t lpx      = bx + (TASKBTN_W > disp_len * fw ? (TASKBTN_W - disp_len * fw) / 2u : 0u);
    uint64_t lpy      = ty + (TASKBAR_H - fh) / 2u;
    gui_draw_str_clip(lpx, lpy, label, COL_TASKBTN_FG, bg, max_ch);
}

/* Draw a small system-tray area on the right side of the taskbar:
 *   [HH:MM] [mem bar] */
static void taskbar_draw_tray(void) {
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
    console_fill_rect(clk_x > 4u ? clk_x - 4u : 0u, ty + 2u, clk_w + 8u, TASKBAR_H - 4u, bg);
    gui_draw_str(clk_x, clk_y, clk, 0x00a0c8e8u, bg);

    /* ── Memory bar (16px wide, 8px tall) ── */
    uint64_t total_p = pmm_get_total_pages();
    uint64_t free_p  = pmm_get_free_pages();
    uint64_t bar_full_w = 32u;
    uint64_t fill = total_p > 0u ? (total_p - free_p) * bar_full_w / total_p : 0u;

    uint64_t bx = clk_x > bar_full_w + 12u ? clk_x - bar_full_w - 12u : 0u;
    uint64_t by = ty + (TASKBAR_H - 8u) / 2u;
    console_fill_rect(bx, by, bar_full_w, 8u, 0x00101828u);
    if (fill > 0u) console_fill_rect(bx, by, fill, 8u, g_theme.accent);
    console_fill_rect(bx, by, bar_full_w, 1u, 0x00202838u);
    console_fill_rect(bx, by + 7u, bar_full_w, 1u, 0x00202838u);
    console_fill_rect(bx, by, 1u, 8u, 0x00202838u);
    console_fill_rect(bx + bar_full_w - 1u, by, 1u, 8u, 0x00202838u);
}

static void taskbar_draw(void) {
    uint64_t fb_w = console_fb_width();
    uint64_t fb_h = console_fb_height();
    uint64_t fw   = console_font_width();
    uint64_t fh   = console_font_height();
    uint64_t ty   = fb_h - TASKBAR_H;

    console_fill_rect(0, ty, fb_w, TASKBAR_H, COL_TASKBAR);
    console_fill_rect(0, ty, fb_w, 2u, g_theme.accent);

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
    if (g_wins[3].active) {
        const char *tv_label = (g_wins[3].text.path[0]) ? g_wins[3].text.title_buf : "Viewer";
        taskbar_draw_btn(3, tv_label);
    }

    taskbar_draw_tray();
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
    static const char *items[] = { "Terminal", "Files", "Settings", "Viewer" };

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
    static const char *ctx_items[] = { "Terminal", "Files", "Settings", "Viewer" };
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

/* ── Text editor context menu draw ──────────────────────────────────── */
static void txt_ctx_draw(void) {
    if (!g_txt_ctx_open) return;
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();
    static const char *txt_ctx_items[] = { "Select All", "Copy", "Cut", "Paste", "Find..." };
    int32_t cx = g_txt_ctx_x;
    int32_t cy = g_txt_ctx_y;
    console_fill_rect((uint64_t)cx, (uint64_t)cy, TXT_CTX_W, TXT_CTX_ITEMS * CTX_ITEM_H + 2u, COL_LAUNCH_BG);
    console_fill_rect((uint64_t)cx, (uint64_t)cy, TXT_CTX_W, 1u, COL_LAUNCH_HL);
    console_fill_rect((uint64_t)cx, (uint64_t)cy + TXT_CTX_ITEMS * CTX_ITEM_H + 1u, TXT_CTX_W, 1u, COL_LAUNCH_HL);
    console_fill_rect((uint64_t)cx, (uint64_t)cy, 1u, TXT_CTX_ITEMS * CTX_ITEM_H + 2u, COL_LAUNCH_HL);
    console_fill_rect((uint64_t)cx + TXT_CTX_W - 1u, (uint64_t)cy, 1u, TXT_CTX_ITEMS * CTX_ITEM_H + 2u, COL_LAUNCH_HL);
    for (int i = 0; i < TXT_CTX_ITEMS; i++) {
        uint64_t ry = (uint64_t)cy + 1u + (uint64_t)i * CTX_ITEM_H;
        bool hov    = (g_txt_ctx_hover == i);
        uint32_t bg = hov ? COL_LAUNCH_HL : COL_LAUNCH_BG;
        console_fill_rect((uint64_t)cx + 1u, ry, TXT_CTX_W - 2u, CTX_ITEM_H, bg);
        uint64_t slen = (uint64_t)gui_strlen(txt_ctx_items[i]);
        uint64_t spx  = (uint64_t)cx + (TXT_CTX_W - slen * fw) / 2u;
        uint64_t spy  = ry + (CTX_ITEM_H - fh) / 2u;
        gui_draw_str(spx, spy, txt_ctx_items[i], COL_LAUNCH_FG, bg);
    }
}

/* ── File browser context menu draw ─────────────────────────────────── */

static const char *fb_ctx_labels[] = {
    "Open", "Edit", "Rename", "Delete", "New File", "New Folder", "Refresh",
    "Copy", "Cut", "Paste", "Copy Path"
};

static void fb_ctx_draw(void) {
    if (!g_fb_ctx_open || g_fb_ctx_n <= 0) return;
    uint64_t fh = console_font_height();
    uint64_t cx = (uint64_t)g_fb_ctx_x;
    uint64_t cy = (uint64_t)g_fb_ctx_y;
    int n = g_fb_ctx_n;

    console_fill_rect(cx, cy, FB_CTX_W, (uint64_t)n * CTX_ITEM_H + 2u, COL_LAUNCH_BG);
    console_fill_rect(cx, cy, FB_CTX_W, 1u, COL_LAUNCH_HL);
    console_fill_rect(cx, cy + (uint64_t)n * CTX_ITEM_H + 1u, FB_CTX_W, 1u, COL_LAUNCH_HL);
    console_fill_rect(cx, cy, 1u, (uint64_t)n * CTX_ITEM_H + 2u, COL_LAUNCH_HL);
    console_fill_rect(cx + FB_CTX_W - 1u, cy, 1u, (uint64_t)n * CTX_ITEM_H + 2u, COL_LAUNCH_HL);

    for (int i = 0; i < n; i++) {
        int act   = g_fb_ctx_acts[i];
        uint64_t ry  = cy + 1u + (uint64_t)i * CTX_ITEM_H;
        bool hov     = (g_fb_ctx_hover == i);
        uint32_t bg  = hov ? COL_LAUNCH_HL : COL_LAUNCH_BG;
        uint32_t fg  = (act == FB_CTX_ACT_DELETE)    ? 0x00e87060u :
                       (act == FB_CTX_ACT_CUT)       ? 0x00e8b060u :
                       (act == FB_CTX_ACT_PASTE)     ? 0x0080c8e8u :
                       (act == FB_CTX_ACT_EDIT)      ? 0x0080d8a0u :
                       (act == FB_CTX_ACT_COPY_PATH) ? 0x00a0b8d0u : COL_LAUNCH_FG;
        console_fill_rect(cx + 1u, ry, FB_CTX_W - 2u, CTX_ITEM_H, bg);
        const char *lbl  = fb_ctx_labels[act];
        uint64_t slen    = (uint64_t)gui_strlen(lbl);
        uint64_t spx     = cx + 10u;  /* left-align with small indent */
        uint64_t spy     = ry + (CTX_ITEM_H - fh) / 2u;
        (void)slen;
        gui_draw_str(spx, spy, lbl, fg, bg);
    }
}

/* Open the FB context menu at (x, y) for the given window/row */
static void fb_ctx_open_at(int win_slot, int row, bool is_dir, int32_t x, int32_t y) {
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
    if ((uint64_t)x + FB_CTX_W > fb_w2) x = (int32_t)(fb_w2 - FB_CTX_W);
    if ((uint64_t)y + menu_h > ty2)      y = (int32_t)(ty2 - menu_h);
    if (x < 0) x = 0;
    if (y < (int32_t)STATUS_H) y = (int32_t)STATUS_H;

    g_fb_ctx_x    = x;
    g_fb_ctx_y    = y;
    g_fb_ctx_open = true;
    fb_ctx_draw();
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

    /* Title bar with subtle gradient: lighter top strip → base color */
    console_fill_rect(w->x, w->y, w->w, TITLE_H, title_bg);
    /* Top highlight line */
    uint32_t title_top = active ? 0x00223a62u : 0x001e2e44u;
    console_fill_rect(w->x, w->y, w->w, 1u, title_top);
    /* Bottom separator line */
    console_fill_rect(w->x, w->y + TITLE_H - 1u, w->w, 1u, 0x0010192au);

    /* Compute button positions first so we know available title width */
    w->btn_cls_x = w->x + w->w - BTN_W;
    w->btn_max_x = w->btn_cls_x - BTN_W;
    w->btn_min_x = w->btn_max_x - BTN_W;

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
    uint64_t tpy      = w->y + (TITLE_H - fh) / 2u;
    uint64_t avail    = w->btn_min_x > w->x + 8u ? w->btn_min_x - w->x - 8u : 0u;
    uint64_t max_ch   = fw > 0u ? avail / fw : 0u;
    uint64_t tpx;
    if (tlen <= max_ch) {
        tpx = w->x + (w->w - tlen * fw) / 2u;
        if (w->w < tlen * fw) tpx = w->x + 4u;  /* safety against wrap */
        gui_draw_str_clip(tpx, tpy, disp_title, COL_TITLE_FG, title_bg, max_ch);
    } else {
        /* Title too long: show clipped text with trailing "…" */
        tpx = w->x + 4u;
        if (max_ch > 3u) {
            gui_draw_str_clip(tpx, tpy, disp_title, COL_TITLE_FG, title_bg, max_ch - 3u);
            gui_draw_str(tpx + (max_ch - 3u) * fw, tpy, "...", 0x00506878u, title_bg);
        } else {
            gui_draw_str_clip(tpx, tpy, disp_title, COL_TITLE_FG, title_bg, max_ch);
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
    if (g_term_scroll > 0)
        console_set_suppress_draw(true);
    else
        console_set_suppress_draw(false);
}

/* Render scrollback content into the terminal window when g_term_scroll > 0. */
static void term_render_scrollback(window_t *w) {
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
        uint64_t thumb_y = cy + (uint64_t)g_term_scroll * (sb_h - thumb_h) / (uint64_t)max_scroll;
        if (thumb_y + thumb_h > cy + sb_h) thumb_y = cy + sb_h - thumb_h;
        console_fill_rect(sb_x + 1u, thumb_y, 2u, thumb_h, 0x00304860u);
    }
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

static bool fb_is_viewable(const char *name) {
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

static void fb_load(fb_state_t *fb, const char *path) {
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

static void fb_navigate(fb_state_t *fb, const char *path) {
    /* Push current path to back stack, clear forward stack */
    if (fb->hist_depth < FB_HIST_MAX)
        fb_str_copy(fb->hist[fb->hist_depth++], fb->path, 128);
    fb->fwd_depth = 0;
    fb_load(fb, path);
}

static void fb_back(fb_state_t *fb) {
    if (fb->hist_depth == 0) return;
    /* Push current to forward stack */
    if (fb->fwd_depth < FB_HIST_MAX)
        fb_str_copy(fb->fwd_hist[fb->fwd_depth++], fb->path, 128);
    char prev[128];
    fb_str_copy(prev, fb->hist[--fb->hist_depth], 128);
    fb_load(fb, prev);
}

static void fb_forward(fb_state_t *fb) {
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

/* ── Text viewer ─────────────────────────────────────────────────────── */

static syn_lang_t detect_lang(const char *path);  /* forward declaration */

static void recent_add(const char *path) {
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

static void text_open(window_t *w, const char *path) {
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

static void edit_sel_clear(text_state_t *ts) { ts->sel_anchor = -1; ts->sel_end = -1; }

/* Returns canonical [lo, hi) byte range from sel_anchor/sel_end; lo==hi = no selection */
static void edit_sel_range(const text_state_t *ts, int32_t *lo, int32_t *hi) {
    if (ts->sel_anchor < 0) { *lo = *hi = 0; return; }
    if (ts->sel_anchor <= ts->sel_end) { *lo = ts->sel_anchor; *hi = ts->sel_end; }
    else                               { *lo = ts->sel_end;    *hi = ts->sel_anchor; }
}

static void edit_copy_to_clip(text_state_t *ts) {
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

static void edit_set_clipboard(const uint8_t *data, uint32_t len) {
    if (g_clipboard) { kfree(g_clipboard); g_clipboard = NULL; g_clipboard_len = 0; }
    if (!data || len == 0) return;
    g_clipboard = (uint8_t *)kmalloc(len + 1u);
    if (!g_clipboard) return;
    for (uint32_t k = 0; k < len; k++) g_clipboard[k] = data[k];
    g_clipboard[len] = '\0';
    g_clipboard_len  = len;
}

/* Update cached edit_cur_line/col by scanning edit_buf up to edit_cur. */
static void edit_sync_pos(text_state_t *ts) {
    ts->edit_cur_line = 0;
    ts->edit_cur_col  = 0;
    if (!ts->edit_buf) return;
    for (uint32_t i = 0; i < ts->edit_cur; i++) {
        if (ts->edit_buf[i] == '\n') { ts->edit_cur_line++; ts->edit_cur_col = 0; }
        else ts->edit_cur_col++;
    }
}

/* Recount total_lines and max_line_len from edit_buf or data. */
static void edit_recount(window_t *w) {
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
static uint32_t text_xy_to_offset(window_t *w, int32_t mx, int32_t my) {
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
static uint32_t text_xy_to_offset_ro(window_t *w, int32_t mx, int32_t my) {
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
static void edit_scroll_to_cursor(window_t *w) {
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
static void text_enter_edit(window_t *w) {
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
static void text_exit_edit(window_t *w) {
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
static void text_save(window_t *w) {
    text_state_t *ts = &w->text;
    if (!ts->edit_mode || !ts->edit_buf || !ts->path[0]) return;
    vfs_write(ts->path, ts->edit_buf, (uint64_t)ts->edit_size);
    ts->edit_modified = false;
    gui_toast("Saved", 0x0080e8b0u);
}

/* Insert one byte at cursor. */
static bool edit_insert(text_state_t *ts, uint8_t c) {
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
static bool edit_delete_selection(text_state_t *ts) {
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
static void edit_paste(window_t *w) {
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
static void edit_push_undo(text_state_t *ts) {
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
static void edit_pop_undo(window_t *w) {
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
static void edit_pop_redo(window_t *w) {
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
static void edit_del_before(text_state_t *ts) {
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
static void edit_del_at(text_state_t *ts) {
    if (ts->edit_cur >= ts->edit_size) return;
    for (uint32_t i = ts->edit_cur; i < ts->edit_size - 1u; i++)
        ts->edit_buf[i] = ts->edit_buf[i+1];
    ts->edit_size--;
    ts->edit_modified = true;
    /* Cursor position doesn't change */
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
}

/* Delete the word immediately before the cursor (Ctrl+Backspace). */
static void edit_del_word_before(text_state_t *ts) {
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
static void edit_del_word_at(text_state_t *ts) {
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
static void edit_move_left(text_state_t *ts) {
    if (ts->edit_cur == 0) return;
    ts->edit_cur--;
    if (ts->edit_buf[ts->edit_cur] == '\n') { ts->edit_cur_line--; edit_sync_pos(ts); }
    else ts->edit_cur_col--;
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
}

/* Move cursor right one byte. */
static void edit_move_right(text_state_t *ts) {
    if (ts->edit_cur >= ts->edit_size) return;
    uint8_t c = ts->edit_buf[ts->edit_cur];
    ts->edit_cur++;
    if (c == '\n') { ts->edit_cur_line++; ts->edit_cur_col = 0; }
    else           ts->edit_cur_col++;
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
}

/* Move cursor left by one word (Ctrl+Left). Stops at identifier boundaries. */
static void edit_move_word_left(text_state_t *ts) {
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
static void edit_move_word_right(text_state_t *ts) {
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
static void edit_indent_block(text_state_t *ts, bool indent) {
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
static void edit_toggle_comment(window_t *w) {
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

static void text_search_next(window_t *w, bool from_next); /* forward decl */

/* Replace current search match with repl_buf, then find next. */
static void text_replace_one(window_t *w) {
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
static int text_replace_all_impl(window_t *w) {
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
static void edit_dup_line(window_t *w) {
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
static void edit_kill_line(text_state_t *ts) {
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
static void edit_move_end(text_state_t *ts) {
    while (ts->edit_cur < ts->edit_size && ts->edit_buf[ts->edit_cur] != '\n')
        ts->edit_cur++;
    edit_sync_pos(ts);
    ts->edit_want_col = (uint32_t)ts->edit_cur_col;
}

/* Move cursor up one line. */
static void edit_move_up(text_state_t *ts) {
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
static void edit_move_down(text_state_t *ts) {
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
static void edit_move_line_up(window_t *w) {
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
static void edit_move_line_down(window_t *w) {
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
static syn_lang_t detect_lang(const char *path) {
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
static void text_search_next(window_t *w, bool from_next) {
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
static void text_search_prev(window_t *w) {
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

static void text_render(window_t *w) {
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

static void fb_on_motion(window_t *w, int32_t mx, int32_t my) {
    bool changed = false;

    int new_hover = fb_hit_row(w, mx, my);
    if (new_hover != w->fb.hover_row) { w->fb.hover_row = new_hover; changed = true; }

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
        if (new_phov != w->fb.path_hov_char) { w->fb.path_hov_char = new_phov; changed = true; }
    }

    /* Header hover */
    int new_hh = fb_hit_header(w, mx, my);
    if (new_hh != w->fb.header_hover) { w->fb.header_hover = new_hh; changed = true; }

    /* Toolbar button hover */
    {
        int _tbh = fb_hit_toolbar(w, mx, my);
        int new_tbh = (_tbh == 0) ? 0 : (_tbh == 5) ? 1 : (_tbh == 1) ? 2 : (_tbh == 2) ? 3 : (_tbh == 6) ? 4 : -1;
        if (new_tbh != w->fb.toolbar_hover) { w->fb.toolbar_hover = new_tbh; changed = true; }
    }

    /* Column separator drag */
    if (w->fb.col_drag_active) {
        uint64_t fw2 = console_font_width();
        if (fw2 < 1) fw2 = 1;
        int dx = mx - w->fb.col_drag_start_x;
        int new_chars = w->fb.col_drag_start_chars - (int)(dx / (int)fw2);
        if (new_chars < 4)  new_chars = 4;
        if (new_chars > 16) new_chars = 16;
        if (new_chars != w->fb.size_col_chars) { w->fb.size_col_chars = new_chars; changed = true; }
    }

    if (changed) fb_render(w);
}

static void win_show(window_t *w, int slot);  /* forward decl */

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

static void fb_on_click(window_t *w, int32_t mx, int32_t my) {
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
        /* Open files in the viewer */
        const char *name = w->fb.entries[idx];
        if (fb_is_viewable(name)) {
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
    /* Build dynamic memory string: "used / total MB" */
    char mem_str[32];
    {
        uint64_t total_p = pmm_get_total_pages();
        uint64_t free_p  = pmm_get_free_pages();
        uint64_t used_mb = ((total_p - free_p) * 4096u) >> 20u;
        uint64_t tot_mb  = (total_p * 4096u) >> 20u;
        char ub[8], tb[8];
        gui_itoa((int)used_mb, ub, 8); gui_itoa((int)tot_mb, tb, 8);
        int ri = 0;
        const char *p;
        for (p=ub; *p && ri<28; ) mem_str[ri++]=*p++;
        for (p=" / "; *p && ri<28; ) mem_str[ri++]=*p++;
        for (p=tb; *p && ri<28; ) mem_str[ri++]=*p++;
        for (p=" MB"; *p && ri<28; ) mem_str[ri++]=*p++;
        mem_str[ri] = '\0';
    }
    /* Build uptime string */
    char up_str[12];
    {
        uint64_t hz2 = pit_get_hz(); if (!hz2) hz2 = 100;
        uint64_t sc  = pit_ticks() / hz2;
        uint64_t mn  = sc / 60u; sc %= 60u;
        uint64_t hr  = mn / 60u; mn %= 60u;
        gui_itoa_pad2((int)hr, up_str + 0); up_str[2] = ':';
        gui_itoa_pad2((int)mn, up_str + 3); up_str[5] = ':';
        gui_itoa_pad2((int)sc, up_str + 6); up_str[8] = '\0';
    }
    struct { const char *key; const char *val; } sysinfo[] = {
        { "OS:",         "FiFi OS Alpha v5.0"     },
        { "Arch:",       "x86_64"                 },
        { "Kernel:",     "freestanding"           },
        { "Bootloader:", "Limine / UEFI"          },
        { "Memory:",     mem_str                  },
        { "Resolution:", res_str                  },
        { "Font:",       console_font_name()      },
        { "Uptime:",     up_str                   },
        { NULL, NULL }
    };
    for (int i = 0; sysinfo[i].key; i++) {
        uint32_t bg = (i & 1) ? 0x000f151fu : COL_SET_BG;
        console_fill_rect(ix, cy, iw, SET_ROW_H, bg);
        gui_draw_str(cx, cy + (SET_ROW_H - fh) / 2u, sysinfo[i].key, COL_SET_KEY_FG, bg);
        gui_draw_str(val_x, cy + (SET_ROW_H - fh) / 2u, sysinfo[i].val, COL_SET_VAL_FG, bg);
        cy += SET_ROW_H;
    }

    /* Memory usage bar */
    cy += 6u;
    {
        uint64_t bar_w  = iw > 2u * (uint64_t)SET_PAD ? iw - 2u * (uint64_t)SET_PAD : 1u;
        uint64_t used_p = pmm_get_total_pages() - pmm_get_free_pages();
        uint64_t tot_p  = pmm_get_total_pages();
        uint64_t fill   = tot_p > 0 ? used_p * bar_w / tot_p : 0u;
        console_fill_rect(cx, cy, bar_w, 8u, 0x0008101cu);
        if (fill > 0) console_fill_rect(cx, cy, fill, 8u, 0x00306898u);
        console_fill_rect(cx, cy, bar_w, 1u, 0x00203040u);
        console_fill_rect(cx, cy + 7u, bar_w, 1u, 0x00203040u);
    }
    cy += 12u;

    console_fill_rect(ix, cy, iw, 1u, COL_SET_SEP);
    cy += 5u;

    /* ── Section: Display ── */
    if (cy + SET_SEC_H + SET_ROW_H + 8u <= iy + ih) {
        console_fill_rect(ix, cy, iw, SET_SEC_H, COL_SET_SEC_BG);
        gui_draw_str(cx, cy + (SET_SEC_H - fh) / 2u, "Display",
                     COL_SET_SEC_FG, COL_SET_SEC_BG);
        cy += SET_SEC_H + 4u;

        /* Font row: [<] FontName [>] */
        uint64_t btn_h = fh + 6u;
        console_fill_rect(ix, cy, iw, btn_h, COL_SET_BG);
        gui_draw_str(cx, cy + (btn_h - fh) / 2u, "Font:", COL_SET_KEY_FG, COL_SET_BG);

        /* Prev button */
        uint64_t pb_x = val_x;
        console_fill_rect(pb_x, cy, g_font_btn_bw, btn_h, 0x00182838u);
        gui_draw_str(pb_x + (g_font_btn_bw - fw) / 2u, cy + (btn_h - fh) / 2u,
                     "<", 0x0060a0e0u, 0x00182838u);
        g_font_prev_bx = pb_x;

        /* Font label */
        uint64_t fl_x = pb_x + g_font_btn_bw + 4u;
        uint64_t nxt_x = ix + iw - SET_PAD - g_font_btn_bw;
        uint64_t fl_w  = nxt_x > fl_x + 2u ? nxt_x - fl_x - 2u : 1u;
        uint64_t fl_max = fl_w / fw;
        int nf = 0; while (g_font_paths[nf]) nf++;
        int idx = g_font_idx < nf ? g_font_idx : 0;
        gui_draw_str_clip(fl_x, cy + (btn_h - fh) / 2u,
                          g_font_labels[idx], COL_SET_VAL_FG, COL_SET_BG, fl_max);

        /* Next button */
        console_fill_rect(nxt_x, cy, g_font_btn_bw, btn_h, 0x00182838u);
        gui_draw_str(nxt_x + (g_font_btn_bw - fw) / 2u, cy + (btn_h - fh) / 2u,
                     ">", 0x0060a0e0u, 0x00182838u);
        g_font_next_bx = nxt_x;
        g_font_btn_by  = cy;
        g_font_btn_bh  = btn_h;
        cy += btn_h + 4u;

        console_fill_rect(ix, cy, iw, 1u, COL_SET_SEP);
        cy += 5u;
    }

    /* ── Section: Theme ── */
    if (cy + SET_SEC_H + SET_ROW_H + 40u <= iy + ih) {
        console_fill_rect(ix, cy, iw, SET_SEC_H, COL_SET_SEC_BG);
        gui_draw_str(cx, cy + (SET_SEC_H - fh) / 2u, "Theme",
                     COL_SET_SEC_FG, COL_SET_SEC_BG);
        cy += SET_SEC_H + 4u;

        /* Accent colour rows (16 swatches, 8 per row) */
        uint64_t sw_sz = (uint64_t)(fh + 4u);
        uint64_t sw_gap = 4u;
        /* Row 1 */
        console_fill_rect(ix, cy, iw, SET_ROW_H + 8u, COL_SET_BG);
        gui_draw_str(cx, cy + (SET_ROW_H - fh) / 2u + 2u, "Accent:", COL_SET_KEY_FG, COL_SET_BG);
        uint64_t sw_x = val_x;
        uint64_t sw_y = cy + (SET_ROW_H + 8u - sw_sz) / 2u;
        g_theme_swatch_sz = sw_sz;
        g_theme_accent_by = sw_y;
        for (int ai = 0; ai < ACCENT_PRESET_COUNT; ai++) {
            if (ai == 8) {
                /* Wrap to second row */
                cy += SET_ROW_H + 8u;
                console_fill_rect(ix, cy, iw, SET_ROW_H + 4u, COL_SET_BG);
                sw_x = val_x;
                sw_y = cy + (SET_ROW_H + 4u - sw_sz) / 2u;
                g_theme_accent_by2 = sw_y;
            }
            g_theme_accent_bx[ai] = sw_x;
            bool active = (g_accent_presets[ai] == g_theme.accent);
            console_fill_rect(sw_x, sw_y, sw_sz, sw_sz, g_accent_presets[ai]);
            if (active) {
                console_fill_rect(sw_x, sw_y, sw_sz, 2u, 0x00ffffffu);
                console_fill_rect(sw_x, sw_y + sw_sz - 2u, sw_sz, 2u, 0x00ffffffu);
                console_fill_rect(sw_x, sw_y, 2u, sw_sz, 0x00ffffffu);
                console_fill_rect(sw_x + sw_sz - 2u, sw_y, 2u, sw_sz, 0x00ffffffu);
            }
            sw_x += sw_sz + sw_gap;
        }
        cy += SET_ROW_H + 12u;

        /* Wallpaper selector row */
        console_fill_rect(ix, cy, iw, SET_ROW_H + 4u, COL_SET_BG);
        gui_draw_str(cx, cy + (SET_ROW_H - fh) / 2u + 2u, "Wallpaper:", COL_SET_KEY_FG, COL_SET_BG);

        static const char *wall_names[WALLPAPER_COUNT] = {
            "Gradient", "Solid", "Stars", "Grid", "Waves"
        };
        uint64_t wall_bh = (uint64_t)(fh + 6u);
        uint64_t wall_bw = 0u;
        {
            uint64_t max_namelen = 0;
            for (int wi = 0; wi < WALLPAPER_COUNT; wi++) {
                uint64_t nl = (uint64_t)gui_strlen(wall_names[wi]);
                if (nl > max_namelen) max_namelen = nl;
            }
            wall_bw = (max_namelen + 2u) * fw;
        }
        g_theme_wall_bh = wall_bh;
        g_theme_wall_bw = wall_bw;
        uint64_t wx  = val_x;
        uint64_t wy  = cy + (SET_ROW_H + 4u - wall_bh) / 2u;
        g_theme_wall_by = wy;
        for (int wi = 0; wi < WALLPAPER_COUNT; wi++) {
            g_theme_wall_bx[wi] = wx;
            bool active = (wi == g_theme.wallpaper);
            uint32_t bbg = active ? g_theme.accent : 0x00182838u;
            uint32_t bfg = active ? 0x00ffffffu : 0x0090b0d0u;
            console_fill_rect(wx, wy, wall_bw, wall_bh, bbg);
            uint64_t nl   = (uint64_t)gui_strlen(wall_names[wi]);
            uint64_t bpx  = wx + (wall_bw > nl * fw ? (wall_bw - nl * fw) / 2u : 0u);
            uint64_t bpy  = wy + (wall_bh - fh) / 2u;
            gui_draw_str(bpx, bpy, wall_names[wi], bfg, bbg);
            wx += wall_bw + 4u;
        }
        cy += SET_ROW_H + 8u;

        /* Toggle row: four ON/OFF buttons — Clock 12h, Animations, Status Bar, Desk Info */
        {
            console_fill_rect(ix, cy, iw, SET_ROW_H + 4u, COL_SET_BG);
            static const char *tog_labels[4] = { "12h Clock", "Animations", "Status Bar", "Desk Info" };
            bool tog_vals[4] = { g_theme.clock_12h, g_theme.animations, g_theme.statusbar, g_theme.desktop_info };

            uint64_t tbh = (uint64_t)(fh + 6u);
            uint64_t tbw = 11u * fw; /* fits all labels with padding */
            uint64_t tx  = val_x;
            uint64_t ty2 = cy + (SET_ROW_H + 4u - tbh) / 2u;
            g_theme_toggle_h = tbh;
            g_theme_toggle_w = tbw;
            for (int ti = 0; ti < 4; ti++) {
                g_theme_toggle_x[ti] = tx;
                g_theme_toggle_y[ti] = ty2;
                bool on = tog_vals[ti];
                uint32_t tbg = on ? g_theme.accent : 0x00182838u;
                uint32_t tfg = on ? 0x00ffffffu : 0x00607080u;
                console_fill_rect(tx, ty2, tbw, tbh, tbg);
                uint64_t nl  = (uint64_t)gui_strlen(tog_labels[ti]);
                uint64_t tpx = tx + (tbw > nl * fw ? (tbw - nl * fw) / 2u : 0u);
                uint64_t tpy = ty2 + (tbh - fh) / 2u;
                gui_draw_str(tpx, tpy, tog_labels[ti], tfg, tbg);
                tx += tbw + 6u;
            }
            cy += SET_ROW_H + 8u;
        }

        /* UTC offset row: [−]  UTC+N  [+] */
        {
            uint64_t btn_h2 = fh + 6u;
            console_fill_rect(ix, cy, iw, btn_h2, COL_SET_BG);
            gui_draw_str(cx, cy + (btn_h2 - fh) / 2u, "Clock UTC:", COL_SET_KEY_FG, COL_SET_BG);

            uint64_t pb2 = val_x;
            console_fill_rect(pb2, cy, g_font_btn_bw, btn_h2, 0x00182838u);
            gui_draw_str(pb2 + (g_font_btn_bw - fw) / 2u, cy + (btn_h2 - fh) / 2u,
                         "-", 0x0060a0e0u, 0x00182838u);
            g_utc_minus_bx = pb2;

            char utc_lbl[8];
            {
                int8_t off = g_theme.utc_offset;
                int abs_off = off < 0 ? (int)-off : (int)off;
                int ri = 0;
                utc_lbl[ri++] = 'U'; utc_lbl[ri++] = 'T'; utc_lbl[ri++] = 'C';
                utc_lbl[ri++] = (off < 0) ? '-' : '+';
                if (abs_off >= 10) utc_lbl[ri++] = (char)('0' + abs_off / 10);
                utc_lbl[ri++] = (char)('0' + abs_off % 10);
                utc_lbl[ri] = '\0';
            }
            uint64_t lbl_len = (uint64_t)gui_strlen(utc_lbl);
            uint64_t lbl_x   = pb2 + g_font_btn_bw + 4u;
            uint64_t plus2   = ix + iw - (uint64_t)SET_PAD - g_font_btn_bw;
            uint64_t lbl_w   = plus2 > lbl_x + 2u ? plus2 - lbl_x - 2u : 1u;
            uint64_t lbl_cx2 = lbl_x + (lbl_w > lbl_len * fw ? (lbl_w - lbl_len * fw) / 2u : 0u);
            console_fill_rect(lbl_x, cy, lbl_w, btn_h2, COL_SET_BG);
            gui_draw_str(lbl_cx2, cy + (btn_h2 - fh) / 2u, utc_lbl, COL_SET_VAL_FG, COL_SET_BG);

            console_fill_rect(plus2, cy, g_font_btn_bw, btn_h2, 0x00182838u);
            gui_draw_str(plus2 + (g_font_btn_bw - fw) / 2u, cy + (btn_h2 - fh) / 2u,
                         "+", 0x0060a0e0u, 0x00182838u);
            g_utc_plus_bx = plus2;
            g_utc_btn_by  = cy;
            g_utc_btn_bh  = btn_h2;
            cy += btn_h2 + 4u;
        }

        console_fill_rect(ix, cy, iw, 1u, COL_SET_SEP);
        cy += 5u;
    }

    /* ── Section: Audio ── */
    if (cy + SET_SEC_H + SET_ROW_H + 4u <= iy + ih) {
        console_fill_rect(ix, cy, iw, SET_SEC_H, COL_SET_SEC_BG);
        gui_draw_str(cx, cy + (SET_SEC_H - fh) / 2u, "Audio",
                     COL_SET_SEC_FG, COL_SET_SEC_BG);
        cy += SET_SEC_H + 4u;

        uint64_t btn_ha = fh + 6u;

        /* Volume row: [−]  NN%  [+] */
        console_fill_rect(ix, cy, iw, btn_ha, COL_SET_BG);
        if (hda_is_ready()) {
            gui_draw_str(cx, cy + (btn_ha - fh) / 2u, "Volume:", COL_SET_KEY_FG, COL_SET_BG);

            uint64_t vm_x = val_x;
            console_fill_rect(vm_x, cy, g_font_btn_bw, btn_ha, 0x00182838u);
            gui_draw_str(vm_x + (g_font_btn_bw - fw) / 2u, cy + (btn_ha - fh) / 2u,
                         "-", 0x0060a0e0u, 0x00182838u);
            g_vol_minus_bx = vm_x;

            char vol_lbl[6];
            {
                int vv = hda_get_volume();
                int ri = 0;
                if (vv >= 100) { vol_lbl[ri++]='1'; vol_lbl[ri++]='0'; vol_lbl[ri++]='0'; }
                else if (vv >= 10) { vol_lbl[ri++]=(char)('0'+vv/10); vol_lbl[ri++]=(char)('0'+vv%10); }
                else { vol_lbl[ri++]=(char)('0'+vv); }
                vol_lbl[ri++] = '%'; vol_lbl[ri] = '\0';
            }
            uint64_t vl_len = (uint64_t)gui_strlen(vol_lbl);
            uint64_t vl_x   = vm_x + g_font_btn_bw + 4u;
            uint64_t vp_x   = ix + iw - (uint64_t)SET_PAD - g_font_btn_bw;
            uint64_t vl_w   = vp_x > vl_x + 2u ? vp_x - vl_x - 2u : 1u;
            uint64_t vl_cx  = vl_x + (vl_w > vl_len * fw ? (vl_w - vl_len * fw) / 2u : 0u);
            console_fill_rect(vl_x, cy, vl_w, btn_ha, COL_SET_BG);
            gui_draw_str(vl_cx, cy + (btn_ha - fh) / 2u, vol_lbl, COL_SET_VAL_FG, COL_SET_BG);

            console_fill_rect(vp_x, cy, g_font_btn_bw, btn_ha, 0x00182838u);
            gui_draw_str(vp_x + (g_font_btn_bw - fw) / 2u, cy + (btn_ha - fh) / 2u,
                         "+", 0x0060a0e0u, 0x00182838u);
            g_vol_plus_bx = vp_x;
            g_vol_btn_by  = cy;
            g_vol_btn_bh  = btn_ha;
        } else {
            gui_draw_str(cx, cy + (btn_ha - fh) / 2u, "No audio device", COL_SET_HINT, COL_SET_BG);
            g_vol_btn_bh = 0u;
        }
        cy += btn_ha + 4u;

        /* Test Tone button */
        if (hda_is_ready() && cy + btn_ha <= iy + ih) {
            static const char *chime_lbl = "Chime";
            uint64_t cbl = (uint64_t)gui_strlen(chime_lbl);
            uint64_t cbw = (cbl + 2u) * fw;
            uint64_t cbx = val_x;
            uint64_t cby = cy;
            console_fill_rect(ix, cy, iw, btn_ha, COL_SET_BG);
            gui_draw_str(cx, cy + (btn_ha - fh) / 2u, "Test:", COL_SET_KEY_FG, COL_SET_BG);
            console_fill_rect(cbx, cby, cbw, btn_ha, 0x00182838u);
            uint64_t cpx = cbx + (cbw - cbl * fw) / 2u;
            gui_draw_str(cpx, cby + (btn_ha - fh) / 2u, chime_lbl, 0x0060a0e0u, 0x00182838u);
            g_vol_chime_bx = cbx; g_vol_chime_by = cby;
            g_vol_chime_bw = cbw; g_vol_chime_bh = btn_ha;
            cy += btn_ha + 4u;
        } else {
            g_vol_chime_bh = 0u;
        }

        console_fill_rect(ix, cy, iw, 1u, COL_SET_SEP);
        cy += 5u;
    }

    /* ── Section: Network ── */
    if (cy + SET_SEC_H + SET_ROW_H < iy + ih) {
        console_fill_rect(ix, cy, iw, SET_SEC_H, COL_SET_SEC_BG);
        gui_draw_str(cx, cy + (SET_SEC_H - fh) / 2u, "Network",
                     COL_SET_SEC_FG, COL_SET_SEC_BG);
        cy += SET_SEC_H + 4u;

        char ip_str[16], mask_str[16], gw_str[16], dns_str[16];
        gui_ip4_str(net_ip,      ip_str,   16);
        gui_ip4_str(net_mask,    mask_str, 16);
        gui_ip4_str(net_gateway, gw_str,   16);
        gui_ip4_str(net_dns,     dns_str,  16);

        /* MAC address string */
        char mac_str[20];
        {
            static const char hex[] = "0123456789abcdef";
            int mi = 0;
            for (int b = 0; b < 6; b++) {
                mac_str[mi++] = hex[(net_mac[b] >> 4) & 0xF];
                mac_str[mi++] = hex[ net_mac[b]       & 0xF];
                if (b < 5) mac_str[mi++] = ':';
            }
            mac_str[mi] = '\0';
        }

        struct { const char *key; const char *val; } netinfo[] = {
            { "Status:",  net_nic_present() ? (net_ip ? "Connected" : "No IP") : "No NIC" },
            { "IP:",      net_ip ? ip_str   : "0.0.0.0"  },
            { "Mask:",    net_ip ? mask_str : "0.0.0.0"  },
            { "Gateway:", net_ip ? gw_str   : "0.0.0.0"  },
            { "DNS:",     net_ip ? dns_str  : "0.0.0.0"  },
            { "MAC:",     mac_str                         },
            { NULL, NULL }
        };
        for (int i = 0; netinfo[i].key; i++) {
            if (cy + SET_ROW_H > iy + ih) break;
            uint32_t bg = (i & 1) ? 0x000f151fu : COL_SET_BG;
            console_fill_rect(ix, cy, iw, SET_ROW_H, bg);
            gui_draw_str(cx,    cy + (SET_ROW_H - fh) / 2u, netinfo[i].key, COL_SET_KEY_FG, bg);
            uint32_t vfg = (i == 0 && net_ip)    ? 0x0060d880u :
                           (i == 0 && !net_ip)   ? 0x00e88060u : COL_SET_VAL_FG;
            gui_draw_str(val_x, cy + (SET_ROW_H - fh) / 2u, netinfo[i].val, vfg, bg);
            cy += SET_ROW_H;
        }

        console_fill_rect(ix, cy, iw, 1u, COL_SET_SEP);
        cy += 5u;
    }

    /* ── Section: Keyboard Shortcuts ── */
    if (cy + SET_SEC_H + SET_ROW_H >= iy + ih) goto settings_done;
    console_fill_rect(ix, cy, iw, SET_SEC_H, COL_SET_SEC_BG);
    gui_draw_str(cx, cy + (SET_SEC_H - fh) / 2u, "Keyboard Shortcuts",
                 COL_SET_SEC_FG, COL_SET_SEC_BG);
    cy += SET_SEC_H + 4u;

    struct { const char *key; const char *desc; } shortcuts[] = {
        { "F1",             "Toggle Terminal"             },
        { "F2",             "Toggle Files"                },
        { "F3",             "Toggle Settings"             },
        { "F4",             "Toggle Text Viewer"          },
        { "Alt+Tab",        "Cycle open windows"         },
        { "Esc / Ctrl+W",   "Close focused window"       },
        { "Up / Down",      "Navigate file list"         },
        { "PgUp / PgDn",    "Scroll one page"            },
        { "Shift+PgUp/PgDn","Terminal: scroll back/fwd"  },
        { "Any key",        "Terminal: snap to live view"},
        { "Home / End",     "Jump to top / bottom"       },
        { "Enter",          "Open file or folder"        },
        { "Backspace",      "Go up one directory"        },
        { "A-Z / 0-9",      "Jump to first match"        },
        { "Y",              "Files: copy path to clipboard"},
        { "Alt+Left",       "Files: navigate back"       },
        { "Alt+Right",      "Files: navigate forward"    },
        { "/ or F",         "Files: open search"         },
        { "Tab / Esc",      "Files: close search"        },
        { "H",              "Files: show/hide dot files" },
        { "V",              "Files: toggle list/icon view"},
        { "Ctrl+N",         "Files: new file"            },
        { "Ctrl+D",         "Files: new directory"       },
        { "Ctrl+R",         "Files: rename selected"     },
        { "Delete",         "Files: delete selected file"},
        { "Ctrl+C",         "Files: copy selected file"  },
        { "Ctrl+X",         "Files: cut (mark for move)" },
        { "Ctrl+V",         "Files: paste file here"     },
        { "Ctrl+F",         "Text viewer: find"          },
        { "Ctrl+G",         "Text viewer: go to line"    },
        { "Click+drag",     "Text viewer: select text"   },
        { "Ctrl+C",         "Text viewer: copy sel/match"},
        { "j / k",          "Text viewer: scroll down/up"},
        { "Left / Right",   "Text viewer: scroll columns"},
        { "W",              "Text viewer: word wrap"     },
        { "R",              "Text viewer: reload file"   },
        { "Ctrl+E",         "Text viewer: enter edit mode"},
        { "Ctrl+O",         "Text viewer: open file by path"},
        { "Tab (open bar)", "Open bar: path completion"  },
        { "Ctrl+B",         "Text viewer: reveal in Files"},
        { "Ctrl+S",         "Edit mode: save file"       },
        { "Ctrl+S (no path)","Edit mode: save-as dialog" },
        { "Tab (save-as)",  "Edit mode: path completion" },
        { "ESC",            "Edit mode: save + exit"     },
        { "Ctrl+A",         "Edit mode: select all"      },
        { "Shift+arrows",   "Edit mode: extend selection"},
        { "Ctrl+C",         "Edit mode: copy sel/line"   },
        { "Ctrl+X",         "Edit mode: cut selection"   },
        { "Ctrl+V",         "Edit mode: paste"           },
        { "Ctrl+Z",         "Edit mode: undo"            },
        { "Ctrl+Y",         "Edit mode: redo"            },
        { "Ctrl+D",         "Edit mode: duplicate line"  },
        { "Ctrl+K",         "Edit mode: kill to EOL"     },
        { "Tab",            "Edit mode: indent block"    },
        { "Shift+Tab",      "Edit mode: unindent block"  },
        { "Ctrl+Backspace", "Edit mode: delete word L"   },
        { "Ctrl+Delete",    "Edit mode: delete word R"   },
        { "Ctrl+arrows",    "Edit mode: word navigate"   },
        { "Alt+Up/Down",    "Edit mode: move line up/down"},
        { "Ctrl+/",         "Edit mode: toggle comment"  },
        { "Ctrl+]",         "Edit mode: jump to bracket" },
        { "Ctrl+L",         "Edit mode: center on cursor"},
        { "Ctrl+R",         "Edit mode: find & replace"  },
        { "Ctrl+Home/End",  "Edit mode: file start/end"  },
        { "Ctrl+E",         "Files: edit selected file"  },
        { "Ctrl+A",         "Files: select all files"    },
        { "Ctrl+click",     "Files: toggle multi-select" },
        { "Shift+click",    "Files: range select"        },
        { "Shift+Up/Down",  "Files: extend selection"    },
        { "Delete (multi)", "Files: delete all selected" },
        { "Dbl-click",      "Edit mode: select word"     },
        { "Triple-click",   "Edit mode: select line"     },
        { "Left / Right",   "Input: cursor move"         },
        { "Ctrl+Left/Right","Input: word jump"           },
        { "Home / End",     "Input: cursor start/end"    },
        { "Delete",         "Input: delete right"        },
        { "Ctrl+V",         "Input: paste from clipboard"},
        { "Enter / Ctrl+N", "Find: next match"           },
        { "Shift+Enter / N","Find: previous match"       },
        { "Tab",            "Find: toggle case fold [Aa]"},
        { "Esc",            "Find: close search"         },
        { NULL, NULL }
    };
    /* Count total shortcuts for clamping scroll */
    int nsc = 0; while (shortcuts[nsc].key) nsc++;
    int vis_rows = (int)((iy + ih > cy) ? (iy + ih - cy) / SET_ROW_H : 0u);
    if (g_settings_scroll > nsc - vis_rows) g_settings_scroll = nsc - vis_rows;
    if (g_settings_scroll < 0) g_settings_scroll = 0;

    for (int i = g_settings_scroll; shortcuts[i].key; i++) {
        uint32_t bg = (i & 1) ? 0x000f151fu : COL_SET_BG;
        console_fill_rect(ix, cy, iw, SET_ROW_H, bg);
        gui_draw_str(cx, cy + (SET_ROW_H - fh) / 2u, shortcuts[i].key, COL_SET_KEY_FG, bg);
        gui_draw_str(val_x, cy + (SET_ROW_H - fh) / 2u, shortcuts[i].desc, COL_SET_VAL_FG, bg);
        cy += SET_ROW_H;
        if (cy + SET_ROW_H > iy + ih) break;
    }
    /* Scroll indicator */
    if (g_settings_scroll > 0 || vis_rows < nsc) {
        uint64_t sb_x = ix + iw - 6u;
        uint64_t sb_h = ih > SET_SEC_H + 4u ? ih - SET_SEC_H - 4u : 4u;
        uint64_t sb_y = iy + SET_SEC_H + 4u;
        console_fill_rect(sb_x, sb_y, 4u, sb_h, 0x00101820u);
        if (nsc > 0) {
            uint64_t th = sb_h * (uint64_t)vis_rows / (uint64_t)nsc;
            if (th < 8u) th = 8u;
            uint64_t ty2 = sb_y + sb_h * (uint64_t)g_settings_scroll / (uint64_t)nsc;
            if (ty2 + th > sb_y + sb_h) ty2 = sb_y + sb_h - th;
            console_fill_rect(sb_x, ty2, 4u, th, 0x00304878u);
        }
    }

    /* ── Hint at bottom ── */
settings_done:
    {
        uint64_t hint_y = iy + ih - fh - 8u;
        if (hint_y > cy + 4u) {
            console_fill_rect(ix, hint_y - 4u, iw, 1u, COL_SET_SEP);
            gui_draw_str(cx, hint_y,
                         "Press Esc or Ctrl+W to close", COL_SET_HINT, COL_SET_BG);
        }
    }
}

/* ── Window content render ───────────────────────────────────────────── */

static void win_render_content(window_t *w) {
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

/* ── Status bar (top strip, 0..STATUS_H-1) ───────────────────────────── */

static void draw_status_bar(void) {
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

/* ── Desktop info overlay (neofetch-style) ───────────────────────────── */

static void draw_desktop_info(void) {
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

    struct { const char *key; const char *val; } rows[] = {
        { "OS",       "FiFi OS Alpha v5.0" },
        { "Arch",     "x86_64"             },
        { "Kernel",   "freestanding"       },
        { "Memory",   mem_str              },
        { "Network",  ip_str               },
        { "Uptime",   up_str               },
        { NULL, NULL }
    };

    /* Measure widths */
    int nrows = 0; while (rows[nrows].key) nrows++;
    uint64_t key_w = 9u * fw;   /* longest key "Network " = 8 + colon */
    uint64_t val_w = 20u * fw;  /* max value */
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

static void draw_desktop_bg(void) {
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
    draw_desktop_info();  /* overlay on top of wallpaper, beneath windows */
}

/* Punch out 4px rounded corners on a window by overwriting with desktop bg. */
static void win_round_corners(const window_t *w) {
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
        if (w->anim_phase == ANIM_CLOSE && w->anim_step > ANIM_TICKS) continue;
        /* Compute animated dimensions */
        uint64_t aw = w->w, ah = w->h, ax = w->x, ay = w->y;
        if (w->anim_phase != ANIM_NONE) {
            int _sidx = (w->anim_step >= 1 && w->anim_step <= ANIM_TICKS) ? w->anim_step - 1 : ANIM_TICKS - 1;
            int _sc = (w->anim_phase == ANIM_OPEN) ? g_anim_open_scale[_sidx] : g_anim_close_scale[_sidx];
            aw = w->w * (uint64_t)_sc / 100u;
            ah = w->h * (uint64_t)_sc / 100u;
            if (aw < 4) aw = 4;
            if (ah < 4) ah = 4;
            ax = w->x + (w->w - aw) / 2u;
            ay = w->y + (w->h - ah) / 2u;
        }
        uint64_t sx3 = ax + 3u, sy3 = ay + 3u;
        if (sx3 + aw <= fb_w && sy3 + ah <= desk_bot())
            console_fill_rect(sx3, sy3, aw, ah, 0x00080c1au);
        uint64_t sx6 = ax + 6u, sy6 = ay + 6u;
        if (sx6 + aw <= fb_w && sy6 + ah <= desk_bot())
            console_fill_rect(sx6, sy6, aw, ah, 0x00020408u);
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
        /* For WIN_FILES, fb_render() calls win_draw_chrome() itself — skip the fill here */
        win_draw_chrome(w, w->type != WIN_FILES);
        win_render_content(w);
        if (w->type != WIN_TERM) suppress_term = true;
    }
    /* Corner-rounding pass: overwrite corner pixels with desktop background */
    for (int zi = 0; zi < MAX_WINS; zi++) {
        int i = g_z[zi];
        window_t *w = &g_wins[i];
        if (!w->active || w->state == WIN_HIDDEN) continue;
        if (w->anim_phase != ANIM_NONE) continue;  /* skip animating windows */
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
    if (g_ctx_open)
        ctx_draw();
    if (g_fb_ctx_open)
        fb_ctx_draw();
    if (g_txt_ctx_open)
        txt_ctx_draw();

    /* Toast notification overlay */
    if (g_toast_ticks > 0 && g_toast_msg[0]) {
        uint64_t fw    = console_font_width();
        uint64_t fh    = console_font_height();
        uint64_t tlen  = (uint64_t)gui_strlen(g_toast_msg);
        uint64_t tw    = tlen * fw + 24u;
        uint64_t th    = fh + 10u;
        uint64_t tx    = (fb_w > tw) ? (fb_w - tw) / 2u : 0u;
        uint64_t ty    = desk_bot() - th - 12u;
        uint32_t tbg   = 0x00101828u;
        console_fill_rect(tx,     ty,     tw,     th,     tbg);
        console_fill_rect(tx,     ty,     tw,     1u,     0x00303858u);
        console_fill_rect(tx,     ty+th-1,tw,     1u,     0x00303858u);
        console_fill_rect(tx,     ty,     1u,     th,     0x00303858u);
        console_fill_rect(tx+tw-1,ty,     1u,     th,     0x00303858u);
        gui_draw_str(tx + 12u, ty + (th - fh) / 2u, g_toast_msg, g_toast_color, tbg);
    }
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
            w->w = 540u;
            w->h = 700u;
            if (w->w > fb_w) w->w = fb_w;
            if (w->h > avail) w->h = avail;
            g_settings_scroll = 0;
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

    w->state      = WIN_NORMAL;
    if (g_theme.animations) {
        w->anim_phase = ANIM_OPEN;
        w->anim_step  = 1;
    } else {
        w->anim_phase = ANIM_NONE;
        w->anim_step  = 0;
    }
    full_redraw();
}

static void win_hide(window_t *w, int slot) {
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
    /* Load a cleaner font if available in the initrd */
    console_load_psf("/fonts/ter16b.psf");

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
    win_show(&g_wins[0], 0);
}

/* Execute a file browser context menu item (shared by mouse-click and keyboard handlers) */
static void fb_ctx_run(int item) {
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
    default: break;
    }
}

void gui_on_tick(void) {
    g_gui_tick++;

    /* ── Once-per-second: redraw status bar and live settings if open ── */
    {
        static uint64_t s_last_sec = (uint64_t)-1;
        uint64_t hz = pit_get_hz();
        if (!hz) hz = 100;
        uint64_t now_sec = pit_ticks() / hz;
        if (now_sec != s_last_sec) {
            s_last_sec = now_sec;
            /* full_redraw updates clock, taskbar tray, settings uptime/memory,
             * and respects z-order so settings never paints over topmost windows. */
            full_redraw();
        }
    }

    /* ── Toast countdown: decrement and trigger full redraw when it expires ── */
    if (g_toast_ticks > 0) {
        g_toast_ticks--;
        if (g_toast_ticks == 0) {
            g_toast_msg[0] = '\0';
            full_redraw();
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
        if (any_anim) full_redraw();
    }

    /* ── Cursor blink: trigger full redraw if any window has active search or edit mode ── */
    if ((g_gui_tick % 25u) == 0u) {
        for (int i = 0; i < MAX_WINS; i++) {
            window_t *bw = &g_wins[i];
            if (!bw->active || bw->state == WIN_HIDDEN || bw->anim_phase != ANIM_NONE) continue;
            if ((bw->type == WIN_FILES && bw->fb.search_active) ||
                (bw->type == WIN_TEXT  && (bw->text.srch_active || bw->text.edit_mode))) {
                full_redraw();
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
                if (w->type == WIN_FILES) fb_on_motion(w, mx, my);
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
                if (w->type == WIN_TEXT &&
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

        if (top_vis >= 0) {
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

        if (new_chrome_win != g_chrome_win || new_chrome_btn != g_chrome_btn) {
            g_chrome_win = new_chrome_win;
            g_chrome_btn = new_chrome_btn;
            full_redraw();
        }
    }

    /* ── Resize edge hover tracking — topmost focused window only ── */
    if (!g_dragging && !g_resizing && !g_launcher_open) {
        int          new_rw = -1;
        resize_dir_t new_rd = RES_NONE;
        if (top_vis >= 0) {
            resize_dir_t rd = hit_resize(&g_wins[top_vis], mx, my);
            if (rd != RES_NONE) { new_rw = top_vis; new_rd = rd; }
        }
        if (new_rw != g_resize_hover_win || new_rd != g_resize_hover_dir) {
            g_resize_hover_win = new_rw;
            g_resize_hover_dir = new_rd;
            full_redraw();
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
                    want = CURSOR_RESIZE_H; break;
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
            int n_btns = g_wins[3].active ? 4 : 3;
            for (int s = 0; s < n_btns; s++) {
                uint64_t bx = TASKBTN_X + (uint64_t)s * (TASKBTN_W + TASKBTN_GAP);
                if ((uint64_t)mx >= bx && (uint64_t)mx < bx + TASKBTN_W) {
                    new_tbhov = s; break;
                }
            }
        }
        if (new_tbhov != g_taskbtn_hover) {
            const char *tbnames[] = { "Terminal", "Files", "Settings",
                g_wins[3].text.path[0] ? g_wins[3].text.title_buf : "Viewer" };
            int old = g_taskbtn_hover;
            g_taskbtn_hover = new_tbhov;
            if (old >= 0 && old < 4) taskbar_draw_btn(old, tbnames[old]);
            if (new_tbhov >= 0 && new_tbhov < 4) taskbar_draw_btn(new_tbhov, tbnames[new_tbhov]);
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

    /* ── FB context menu hover tracking ── */
    if (g_fb_ctx_open) {
        uint64_t fb_cx = (uint64_t)g_fb_ctx_x;
        uint64_t fb_cy = (uint64_t)g_fb_ctx_y;
        int new_fhov = -1;
        if ((uint64_t)mx >= fb_cx && (uint64_t)mx < fb_cx + FB_CTX_W &&
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
        if ((uint64_t)mx >= tc_x && (uint64_t)mx < tc_x + TXT_CTX_W &&
            (uint64_t)my >= tc_y + 1u &&
            (uint64_t)my < tc_y + 1u + (uint64_t)(TXT_CTX_ITEMS * CTX_ITEM_H)) {
            new_thov = (int)((uint64_t)my - (tc_y + 1u)) / (int)CTX_ITEM_H;
        }
        if (new_thov != g_txt_ctx_hover) {
            g_txt_ctx_hover = new_thov;
            txt_ctx_draw();
        }
    }

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
                            z_raise(slot);
                            win_show(fw, slot);
                        } else if (g_z[MAX_WINS - 1] == slot) {
                            win_hide(fw, slot);
                        } else {
                            z_raise(slot);
                            full_redraw();
                        }
                    }
                    continue;
                }
                /* ── Launcher keyboard navigation ── */
                if (g_launcher_open) {
                    if ((uint8_t)ch == KEY_UP) {
                        if (--g_launcher_hover < 0) g_launcher_hover = (int)LAUNCHER_ITEMS - 1;
                        launcher_draw(); continue;
                    } else if ((uint8_t)ch == KEY_DOWN) {
                        if (++g_launcher_hover >= (int)LAUNCHER_ITEMS) g_launcher_hover = 0;
                        launcher_draw(); continue;
                    } else if ((ch == '\r' || ch == '\n' || ch == ' ') && g_launcher_hover >= 0) {
                        int _li = g_launcher_hover;
                        g_launcher_open = false; g_launcher_hover = -1;
                        if (_li < MAX_WINS) {
                            window_t *_lw = &g_wins[_li];
                            z_raise(_li);
                            if (_lw->state == WIN_HIDDEN) win_show(_lw, _li); else full_redraw();
                        }
                        continue;
                    } else { g_launcher_open = false; g_launcher_hover = -1; full_redraw(); continue; }
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
                            z_raise(_ci);
                            if (_cw->state == WIN_HIDDEN) win_show(_cw, _ci); else full_redraw();
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
                    if (vc >= 2) { z_raise(vis[vc - 2]); full_redraw(); }
                    continue;
                }
                if (!focused) continue;
                /* ── Terminal scrollback keyboard controls ── */
                if (focused->type == WIN_TERM) {
                    bool is_shift = kbd_shift_down();
                    int tot_tsb = console_tsb_count_lines();
                    if ((uint8_t)ch == KEY_PGUP && is_shift) {
                        /* Shift+PgUp: scroll back one page */
                        uint64_t fw2 = focused->w > 2u * PAD ? focused->w - 2u * PAD : 1u;
                        uint64_t fh2 = focused->h > TITLE_H + BORDER + 2u * PAD
                                       ? focused->h - TITLE_H - BORDER - 2u * PAD : 1u;
                        int page = (int)(fh2 / console_font_height());
                        if (page < 1) page = 1;
                        (void)fw2;
                        g_term_scroll += page;
                        if (g_term_scroll > tot_tsb) g_term_scroll = tot_tsb;
                        console_set_suppress_draw(g_term_scroll > 0);
                        full_redraw();
                        continue;
                    } else if ((uint8_t)ch == KEY_PGDN && is_shift) {
                        /* Shift+PgDn: scroll forward one page */
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
                            z_raise(1);
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
                            z_raise(1);
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
                        if (g_settings_scroll > 0) { g_settings_scroll--; changed = true; }
                    } else if (ch == KEY_DOWN || ch == 'j') {
                        g_settings_scroll++; changed = true;
                    } else if (ch == KEY_HOME) {
                        g_settings_scroll = 0; changed = true;
                    } else if (ch == KEY_END) {
                        g_settings_scroll = 9999; changed = true;
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

    /* ── Mouse scroll wheel ── */
    {
        int8_t scroll = mouse_consume_scroll();
        if (scroll) {
            /* Close fb context menu on scroll */
            if (g_fb_ctx_open) { g_fb_ctx_open = false; full_redraw(); }
            /* Find topmost visible window under cursor to scroll */
            for (int zi = MAX_WINS - 1; zi >= 0; zi--) {
                int si = g_z[zi];
                window_t *w = &g_wins[si];
                if (!w->active || w->state == WIN_HIDDEN) continue;
                if ((uint64_t)mx < w->x || (uint64_t)mx >= w->x + w->w ||
                    (uint64_t)my < w->y || (uint64_t)my >= w->y + w->h) continue;
                if (w->type == WIN_TERM) {
                    int tot_sb = console_tsb_count_lines();
                    g_term_scroll -= (int)scroll * 3;
                    if (g_term_scroll < 0) g_term_scroll = 0;
                    if (g_term_scroll > tot_sb) g_term_scroll = tot_sb;
                    console_set_suppress_draw(g_term_scroll > 0);
                    full_redraw();
                } else if (w->type == WIN_FILES) {
                    w->fb.scroll_vel += (int32_t)scroll * 24;
                    if (w->fb.scroll_vel >  2048) w->fb.scroll_vel =  2048;
                    if (w->fb.scroll_vel < -2048) w->fb.scroll_vel = -2048;
                } else if (w->type == WIN_TEXT) {
                    w->text.scroll_vel += (int32_t)scroll * 24;
                    if (w->text.scroll_vel >  2048) w->text.scroll_vel =  2048;
                    if (w->text.scroll_vel < -2048) w->text.scroll_vel = -2048;
                } else if (w->type == WIN_SETTINGS) {
                    g_settings_scroll += (int)scroll;
                    if (g_settings_scroll < 0) g_settings_scroll = 0;
                    settings_render(w);
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

        for (int s = 0; s < MAX_WINS; s++) {
            if (s == 3 && !g_wins[3].active) continue;
            uint64_t bx = TASKBTN_X + (uint64_t)s * (TASKBTN_W + TASKBTN_GAP);
            if (mx >= (int32_t)bx && mx < (int32_t)(bx + TASKBTN_W)) {
                window_t *w = &g_wins[s];
                if (w->state == WIN_HIDDEN) {
                    z_raise(s);
                    win_show(w, s);
                } else if (g_z[MAX_WINS - 1] == s) {
                    /* Already focused — minimize */
                    win_hide(w, s);
                } else {
                    /* Raise to front */
                    z_raise(s);
                    full_redraw();
                }
                break;
            }
        }
        mouse_consume_click(&cx, &cy);
        return;
    }

    /* ── Text context menu clicks ── */
    if (btn_pressed && g_txt_ctx_open) {
        int32_t cx2 = g_txt_ctx_x, cy2 = g_txt_ctx_y;
        bool hit_ctx = ((uint64_t)mx >= (uint64_t)cx2 &&
                        (uint64_t)mx <  (uint64_t)cx2 + TXT_CTX_W &&
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

    /* ── Right-click on Text editor window (edit mode): context menu ── */
    if (rbtn_pressed && (uint64_t)my < ty) {
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
                if ((uint64_t)ctx_x + TXT_CTX_W > fb_w2) ctx_x = (int32_t)(fb_w2 - TXT_CTX_W);
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
    if (rbtn_pressed && (uint64_t)my < ty) {
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
            /* Close launcher and fb ctx menu if open */
            if (g_launcher_open) {
                g_launcher_open = false;
                g_launcher_hover = -1;
            }
            if (g_fb_ctx_open) {
                g_fb_ctx_open = false;
                full_redraw();
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

    /* ── File browser context menu clicks ── */
    if (btn_pressed && g_fb_ctx_open) {
        int32_t cx2, cy2;
        uint64_t fcx = (uint64_t)g_fb_ctx_x;
        uint64_t fcy = (uint64_t)g_fb_ctx_y;
        bool inside = ((uint64_t)mx >= fcx && (uint64_t)mx < fcx + FB_CTX_W &&
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
            int ns = g_term_sb_drag_s0 + (int)(dy * (int64_t)g_term_sb_drag_max
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
                        win_draw_chrome(ow, ow->type != WIN_FILES);
                        win_render_content(ow);
                    }
                    /* Drop shadow */
                    uint64_t sx3 = w->x + 3u, sy3 = w->y + 3u;
                    if (sx3 + w->w <= fb_w2 && sy3 + w->h <= desk_bot())
                        console_fill_rect(sx3, sy3, w->w, w->h, 0x00080c1au);
                    uint64_t sx6 = w->x + 6u, sy6 = w->y + 6u;
                    if (sx6 + w->w <= fb_w2 && sy6 + w->h <= desk_bot())
                        console_fill_rect(sx6, sy6, w->w, w->h, 0x00020408u);
                    /* Blit shadow buffer */
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
                            uint64_t thumb_y_t = tcy + (uint64_t)g_term_scroll *
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
                                int ns = (int)(((uint64_t)my - tcy) * (uint64_t)total_sb_t / sb_h_t);
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
                            full_redraw();
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
                                full_redraw();
                                break;
                            }
                        }
                    }
                    /* Wallpaper buttons */
                    if (g_theme_wall_bh > 0 &&
                        (uint64_t)my >= g_theme_wall_by &&
                        (uint64_t)my <  g_theme_wall_by + g_theme_wall_bh) {
                        for (int wi = 0; wi < WALLPAPER_COUNT; wi++) {
                            if ((uint64_t)mx >= g_theme_wall_bx[wi] &&
                                (uint64_t)mx <  g_theme_wall_bx[wi] + g_theme_wall_bw) {
                                g_theme.wallpaper = wi;
                                full_redraw();
                                break;
                            }
                        }
                    }
                    /* Toggle buttons: 12h Clock, Animations, Status Bar, Desk Info */
                    if (g_theme_toggle_h > 0) {
                        for (int ti = 0; ti < 4; ti++) {
                            if ((uint64_t)my >= g_theme_toggle_y[ti] &&
                                (uint64_t)my <  g_theme_toggle_y[ti] + g_theme_toggle_h &&
                                (uint64_t)mx >= g_theme_toggle_x[ti] &&
                                (uint64_t)mx <  g_theme_toggle_x[ti] + g_theme_toggle_w) {
                                if (ti == 0) g_theme.clock_12h    = !g_theme.clock_12h;
                                if (ti == 1) g_theme.animations   = !g_theme.animations;
                                if (ti == 2) g_theme.statusbar    = !g_theme.statusbar;
                                if (ti == 3) g_theme.desktop_info = !g_theme.desktop_info;
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
                            full_redraw();
                        } else if ((uint64_t)mx >= g_utc_plus_bx &&
                                   (uint64_t)mx <  g_utc_plus_bx + g_font_btn_bw) {
                            if (g_theme.utc_offset < 14) g_theme.utc_offset++;
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
                }
            }
            break;
        }
        (void)hit_any;
        int32_t cx, cy;
        mouse_consume_click(&cx, &cy);
    }

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
    if (inertial_dirty) full_redraw();
}
