#ifndef GUI_INTERNAL_H
#define GUI_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __linux__
#include <stdio.h>
#include <time.h>
#endif
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
#define STATUS_H        (console_font_height() + 6u)
#define TASKBAR_H       (console_font_height() + 10u)
#define TITLE_H         24u
#define BTN_W           TITLE_H
#define BORDER          1u
#define PAD             4u
#define LOGO_X          4u
#define LOGO_W          72u
#define TASKBTN_X       84u
#define TASKBTN_W       120u
#define TASKBTN_GAP     4u
#define RESIZE_MARGIN   8u
#define MIN_WIN_W       300u
#define MIN_WIN_H       180u
#define SNAP_DIST       14u
#define LAUNCHER_ITEM_H 26u
#define LAUNCHER_ITEMS  23u
#define LAUNCHER_W      110u
#define CTX_ITEM_H      26u
#define CTX_ITEMS       13u  /* 4 built-in + sep + 5 IPC apps + sep + 2 desktop actions */

/* File browser context menu */
#define FB_CTX_MAX_ITEMS  12
#define FB_CTX_ACT_OPEN       0
#define FB_CTX_ACT_EDIT       1
#define FB_CTX_ACT_RENAME     2
#define FB_CTX_ACT_DELETE     3
#define FB_CTX_ACT_NEW_FILE   4
#define FB_CTX_ACT_NEW_DIR    5
#define FB_CTX_ACT_REFRESH    6
#define FB_CTX_ACT_COPY       7
#define FB_CTX_ACT_CUT        8
#define FB_CTX_ACT_PASTE      9
#define FB_CTX_ACT_COPY_PATH 10
#define FB_CTX_ACT_ADD_DESK  11

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
#define WALLPAPER_GRADIENT  0
#define WALLPAPER_SOLID     1
#define WALLPAPER_STARS     2
#define WALLPAPER_GRID      3
#define WALLPAPER_WAVES     4
#define WALLPAPER_IMAGE     5
#define WALLPAPER_COUNT     6

/* Panel edge + item-run alignment for the configurable taskbar/panel. */
typedef enum { PANEL_BOTTOM = 0, PANEL_TOP = 1, PANEL_LEFT = 2, PANEL_RIGHT = 3 } panel_edge_t;
typedef enum { PALIGN_START = 0, PALIGN_CENTER = 1, PALIGN_END = 2 } panel_align_t;

typedef struct {
    uint32_t accent;        /* primary accent colour (borders, highlights) */
    int      wallpaper;     /* one of WALLPAPER_* */
    bool     clock_12h;     /* true = 12-hour AM/PM format */
    bool     animations;    /* true = window open/close animations */
    bool     statusbar;     /* true = show top status bar */
    bool     desktop_info;  /* true = show neofetch-style info on desktop */
    int8_t   utc_offset;    /* UTC hour offset applied to RTC time, -12..+14 */
    /* ── Panel customization (configurable taskbar) ── */
    uint8_t  panel_edge;    /* panel_edge_t: which screen edge the panel lives on */
    uint8_t  panel_align;   /* panel_align_t: how the item run is aligned */
    bool     panel_autohide;/* reveal-on-edge-hover instead of always visible */
    uint8_t  panel_size;    /* additive thickness over the base, 0..64 px */
    /* ── Visual effects ── */
    bool     fx_glass;      /* translucent (frosted) panels + menus */
    bool     fx_shadows;    /* window drop shadows */
    uint8_t  corner_radius; /* window corner rounding radius, 0..12 px */
} gui_theme_t;

#define DESK_ICON_MAX   12
#define DESK_ICON_W     72
#define DESK_ICON_H     72
#define DESK_ICON_PAD    8
typedef struct {
    char path[256];
    char label[48];
    bool active;
} desk_icon_t;

#define ACCENT_PRESET_COUNT 16
#define ANIM_TICKS 5

typedef enum { WIN_HIDDEN, WIN_NORMAL, WIN_MAXIMIZED } win_state_t;
typedef enum { WIN_TERM, WIN_FILES, WIN_TEXT, WIN_SETTINGS } win_type_t;
typedef enum { ANIM_NONE, ANIM_OPEN, ANIM_CLOSE } anim_phase_t;

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
    /* Undo ring buffer -- snapshots pushed before each edit */
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

#define MAX_WINS 4

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
    uint32_t     raise_z;     /* compared with g_term_raise_z: higher = on top of terminal */
} window_t;

/* ── Shared globals ──────────────────────────────────────────────────── */

extern const uint32_t g_accent_presets[ACCENT_PRESET_COUNT];
extern const int g_anim_open_scale[ANIM_TICKS];
extern const int g_anim_close_scale[ANIM_TICKS];

extern gui_theme_t  g_theme;
extern uint32_t    *g_wall_img;
extern uint32_t     g_wall_img_w;
extern uint32_t     g_wall_img_h;

extern desk_icon_t  g_desk_icons[DESK_ICON_MAX];
extern int          g_desk_icon_hover;
extern int          g_desk_icon_sel;
extern int          g_desk_icon_dbl;
extern uint64_t     g_desk_icon_click_t;

extern window_t     g_wins[MAX_WINS];
extern int          g_z[MAX_WINS];

extern bool         g_dragging;
extern int          g_drag_win;
extern bool         g_text_drag_sel;
extern int          g_text_drag_win;
extern uint64_t     g_text_drag_scroll_tick;
extern int32_t      g_drag_off_x;
extern int32_t      g_drag_off_y;
extern uint32_t    *g_drag_shadow;
extern uint64_t     g_drag_shad_w;
extern uint64_t     g_drag_shad_h;
extern uint8_t     *g_clipboard;
extern uint32_t     g_clipboard_len;
extern bool         g_prev_lbtn;
extern int          g_snap_preview;

extern bool         g_sb_drag;
extern int          g_sb_drag_win;
extern int32_t      g_sb_drag_y0;
extern int32_t      g_sb_drag_x0;
extern int          g_sb_drag_s0;
extern uint64_t     g_sb_drag_range;
extern int          g_sb_drag_max;
extern bool         g_sb_drag_text;
extern bool         g_sb_drag_horiz;

extern bool         g_term_sb_drag;
extern int32_t      g_term_sb_drag_y0;
extern int          g_term_sb_drag_s0;
extern uint64_t     g_term_sb_drag_range;
extern int          g_term_sb_drag_max;

extern bool         g_resizing;
extern int          g_resize_win;
extern resize_dir_t g_resize_dir;
extern int32_t      g_resize_ox;
extern int32_t      g_resize_oy;
extern uint64_t     g_resize_wx0;
extern uint64_t     g_resize_wy0;
extern uint64_t     g_resize_ww0;
extern uint64_t     g_resize_wh0;

extern bool         g_launcher_open;
extern bool         g_help_open;     /* Super+/ shortcuts overlay */
void     help_draw(void);            /* gui_render.c */

extern int          g_settings_scroll;
extern uint32_t     g_gui_raise_z;

#define g_term_raise_z (g_wins[0].raise_z)

extern int          g_settings_total_h;
extern bool         g_sb_drag_settings;

extern int          g_term_scroll;

extern int          g_font_idx;
extern const char  *g_font_paths[];
extern const char  *g_font_labels[];
extern uint64_t     g_font_prev_bx, g_font_next_bx;
extern uint64_t     g_font_btn_by, g_font_btn_bw, g_font_btn_bh;
extern uint64_t     g_utc_minus_bx, g_utc_plus_bx;
extern uint64_t     g_utc_btn_by, g_utc_btn_bh;
extern uint64_t     g_vol_minus_bx, g_vol_plus_bx;
extern uint64_t     g_vol_btn_by, g_vol_btn_bh;
extern uint64_t     g_vol_chime_bx, g_vol_chime_by;
extern uint64_t     g_vol_chime_bw, g_vol_chime_bh;
extern uint64_t     g_gaming_btn_bx, g_gaming_btn_by;
extern uint64_t     g_gaming_btn_bw, g_gaming_btn_bh;
extern uint64_t     g_gaming_mode_bx, g_gaming_mode_by;
extern uint64_t     g_gaming_mode_bw, g_gaming_mode_bh;
extern uint64_t     g_fw_btn_bx, g_fw_btn_by;
extern uint64_t     g_fw_btn_bw, g_fw_btn_bh;
extern int          g_fw_state;
extern uint64_t     g_dns_btn_bx, g_dns_btn_by;
extern uint64_t     g_dns_btn_bw, g_dns_btn_bh;
extern int          g_dns_mode;
extern uint64_t     g_vpn_btn_bx, g_vpn_btn_by;
extern uint64_t     g_vpn_btn_bw, g_vpn_btn_bh;
extern uint64_t     g_vpn_auto_bx, g_vpn_auto_by;
extern uint64_t     g_vpn_auto_bw, g_vpn_auto_bh;
extern uint64_t     g_lto_btn_bx, g_lto_btn_by;
extern uint64_t     g_lto_btn_bw, g_lto_btn_bh;
extern int          g_lto_idx;

extern bool         g_vol_popup_open;
extern uint64_t     g_vol_tray_x;
extern uint64_t     g_vol_tray_w;

extern bool         g_cal_popup_open;   /* clock/calendar popup */
extern uint64_t     g_clk_x;            /* clock hit region on the taskbar */
extern uint64_t     g_clk_w;

/* ── System tray indicators + hover tooltips ─────────────────────────── */
#define TRAY_NONE (-1)
#define TRAY_BATT  0
#define TRAY_CPU   1
#define TRAY_NET   2
#define TRAY_MEM   3
#define TRAY_VOL   4
#define TRAY_CLK   5
extern bool     g_batt_present;
extern uint64_t g_batt_x, g_batt_w;
extern uint64_t g_cpu_tray_x, g_cpu_tray_w;
extern uint64_t g_net_tray_x, g_net_tray_w;
extern uint64_t g_mem_tray_x, g_mem_tray_w;
extern int      g_tray_hover;           /* TRAY_* id under cursor, or TRAY_NONE */
int      tray_item_at(int32_t mx, int32_t my);
void     tray_tip_draw(void);
/* platform battery/cpu (weak — Linux-only) */
__attribute__((weak)) bool battery_present(void);
__attribute__((weak)) int  battery_percent(void);
__attribute__((weak)) bool battery_charging(void);
__attribute__((weak)) int  battery_minutes(void);
__attribute__((weak)) int  cpu_usage_percent(void);
extern uint64_t     g_cal_pop_x, g_cal_pop_y, g_cal_pop_w, g_cal_pop_h;
extern int          g_cal_view_mon, g_cal_view_year;   /* month being displayed */
extern bool         g_cal_pick_open;    /* month/year picker overlay */
/* header nav + picker hit regions (set by cal_popup_draw) */
extern uint64_t     g_cal_prev_bx, g_cal_next_bx, g_cal_arrow_by, g_cal_arrow_bw, g_cal_arrow_bh;
extern uint64_t     g_cal_hdr_bx, g_cal_hdr_bw, g_cal_hdr_by, g_cal_hdr_bh;
extern uint64_t     g_cal_yr_prev_bx, g_cal_yr_next_bx, g_cal_yr_by, g_cal_yr_bw, g_cal_yr_bh;
extern uint64_t     g_cal_mgrid_x, g_cal_mgrid_y, g_cal_mcell_w, g_cal_mcell_h;
void     cal_popup_draw(void);
extern uint64_t     g_vol_pop_x;
extern uint64_t     g_vol_pop_y;
extern uint64_t     g_vol_pop_h;
extern uint64_t     g_vol_pop_minus_x;
extern uint64_t     g_vol_pop_plus_x;
extern uint64_t     g_vol_pop_btn_y;
extern uint64_t     g_vol_pop_btn_w;
extern uint64_t     g_vol_pop_btn_h;
extern uint64_t     g_vol_pop_slid_x;
extern uint64_t     g_vol_pop_slid_w;

extern int          g_chrome_win;
extern int          g_chrome_btn;

extern uint64_t     g_last_click_tick;
extern int          g_last_click_win;
extern int          g_last_click_count;

extern bool         g_ctx_open;
extern int32_t      g_ctx_x;
extern int32_t      g_ctx_y;

extern int          g_launcher_hover;
extern int          g_taskbtn_hover;
extern int          g_ctx_hover;

#define TXT_CTX_ITEMS    5
extern bool         g_txt_ctx_open;
extern int32_t      g_txt_ctx_x;
extern int32_t      g_txt_ctx_y;
extern int          g_txt_ctx_win;
extern int          g_txt_ctx_hover;

extern bool         g_fb_ctx_open;
extern int32_t      g_fb_ctx_x;
extern int32_t      g_fb_ctx_y;
extern int          g_fb_ctx_win;
extern int          g_fb_ctx_row;
extern bool         g_fb_ctx_is_dir;
extern int          g_fb_ctx_hover;
extern int          g_fb_ctx_n;
extern int          g_fb_ctx_acts[FB_CTX_MAX_ITEMS];
extern char         g_fb_clip_path[256];
extern bool         g_fb_clip_is_cut;

extern int          g_resize_hover_win;
extern resize_dir_t g_resize_hover_dir;
extern int          g_resize_pending_win;
extern resize_dir_t g_resize_pending_dir;
extern int          g_resize_pending_ticks;

extern uint64_t     g_theme_accent_bx[ACCENT_PRESET_COUNT];
extern uint64_t     g_theme_accent_by;
extern uint64_t     g_theme_swatch_sz;
extern uint64_t     g_theme_accent_by2;
extern uint64_t     g_theme_wall_bx[WALLPAPER_COUNT];
extern uint64_t     g_theme_wall_by_arr[WALLPAPER_COUNT];
extern uint64_t     g_theme_wall_by;
extern uint64_t     g_theme_wall_bw;
extern uint64_t     g_theme_wall_bh;
#define THEME_TOGGLE_COUNT 7   /* 12h, Animations, Status Bar, Desk Info, Glass, Shadows, Auto-hide */
extern uint64_t     g_theme_toggle_x[THEME_TOGGLE_COUNT], g_theme_toggle_y[THEME_TOGGLE_COUNT];
extern uint64_t     g_theme_toggle_w, g_theme_toggle_h;
#define PANEL_POS_COUNT 4      /* Bottom, Top, Left, Right */
extern uint64_t     g_theme_panel_bx[PANEL_POS_COUNT], g_theme_panel_by[PANEL_POS_COUNT];
extern uint64_t     g_theme_panel_bw, g_theme_panel_bh;
#define PANEL_ALIGN_COUNT 3    /* Left(Start), Center, Right(End) */
extern uint64_t     g_theme_align_bx[PANEL_ALIGN_COUNT], g_theme_align_by[PANEL_ALIGN_COUNT];
extern uint64_t     g_theme_align_bw, g_theme_align_bh;
extern bool         g_panel_revealed;  /* auto-hide: panel currently shown */

extern uint64_t     g_gui_tick;

#define TOAST_TICKS  200u
extern char         g_toast_msg[64];
extern uint32_t     g_toast_color;
extern int          g_toast_ticks;

#define RECENT_MAX 8
extern char         g_recent[RECENT_MAX][128];
extern int          g_recent_count;

extern int          g_redraw_src;
extern const char * const g_launcher_items[LAUNCHER_ITEMS];

/* ── Kickoff launcher (searchable) ───────────────────────────────────── */
#define LAUNCH_MAX 128
typedef struct {
    char    label[40];
    char    exec[192];   /* spawn path; empty for built-in windows / power */
    int8_t  builtin;     /* built-in window slot 0..3, else -1 */
    uint8_t power;       /* 0 none | 1 sleep | 2 restart | 3 shutdown */
} launch_entry_t;
extern launch_entry_t g_launch[LAUNCH_MAX];
extern int  g_launch_n;
extern int  g_launch_filt[LAUNCH_MAX];   /* indices into g_launch matching query */
extern int  g_launch_filt_n;
extern char g_launch_q[40];
extern int  g_launch_qlen;
extern int  g_launcher_scroll;           /* first visible filtered row */

void     launcher_open_reset(void);      /* rebuild list + clear query/sel/scroll */
void     launcher_filter(void);          /* rebuild filtered view from query */
void     launcher_do_launch(int filt_row);
void     launcher_add_desktop(int filt_row);
void     launcher_pin_taskbar(int filt_row);
/* launcher item context menu (right-click) */
extern int g_launchctx_row;
extern int g_launchctx_hover;
void     launchctx_open(int filt_row, int32_t mx, int32_t my);
int      launchctx_hit(int32_t mx, int32_t my);   /* -1 / 0=pin / 1=desktop */
void     launchctx_draw(void);

/* ── Taskbar favorites (pinned apps) ─────────────────────────────────── */
#define FAV_MAX 10
#define FAVBAR_BUILTINS 4   /* Terminal/Files/Settings/Viewer occupy the first favbar slots */
typedef struct { char path[192]; char label[40]; bool active; } fav_t;
extern fav_t g_favs[FAV_MAX];
extern int   g_fav_count;
extern int   g_fav_hover;
void     gui_fav_save(void);
void     gui_fav_load(void);
bool     gui_fav_add(const char *path, const char *label);  /* false if dup/full */
void     gui_fav_remove_at(int idx);
uint64_t fav_btn_w(void);
uint64_t favbar_start_x(void);
uint64_t favbar_w(void);        /* total strip width (0 when no favorites) */
void     favbar_draw(void);
int      favbar_hit(int32_t mx, int32_t my);  /* unified favbar index or -1 */
int      favbar_count(void);                  /* FAVBAR_BUILTINS + user favorites */
int      favbar_builtin_slot(int idx);        /* window slot for a built-in entry, else -1 */
uint32_t *app_load_icon_png(const char *exec, uint32_t *w, uint32_t *h);  /* gui_widgets.c */
int      launcher_hit_row(int32_t mx, int32_t my);  /* filtered idx or -1 (body only) */
bool     launcher_in_search(int32_t mx, int32_t my);
uint64_t launcher_row_h(void);
uint64_t launcher_rows_visible(void);
uint64_t launcher_body_y(void);
uint64_t launcher_panel_w(void);
uint64_t launcher_panel_h(void);

/* ── Platform weak declaration ───────────────────────────────────────── */
__attribute__((weak)) bool platform_load_image(const char *path __attribute__((unused)),
    uint32_t **px __attribute__((unused)),
    uint32_t *w   __attribute__((unused)),
    uint32_t *h   __attribute__((unused)));

/* ── Forward declarations: gui_text_util.c ───────────────────────────── */
size_t   gui_strlen(const char *s);
bool     gui_streq(const char *a, const char *b);
uint64_t desk_top(void);
uint64_t desk_bot(void);
uint64_t desk_avail(void);
uint64_t panel_y(void);                /* panel (taskbar) top Y — edge-aware */
bool     panel_is_vertical(void);      /* true for LEFT/RIGHT panels */
uint64_t panel_x(void);                /* vertical panel strip left X */
uint64_t desk_left(void);
uint64_t desk_right(void);
uint64_t desk_availw(void);            /* usable desktop width */
bool     statusbar_bottom(void);       /* status bar relocated to bottom (top panel) */
uint64_t statusbar_y(void);            /* status bar top Y */
uint64_t vpanel_logo_y(void);          /* vertical dock: logo square Y */
uint64_t vpanel_fav_y0(void);          /* vertical dock: first favorite Y */
void     taskbar_draw_vertical(void);
uint32_t desktop_bg_at(uint64_t y);   /* wallpaper colour at row y (corner rounding) */
void     gui_toast(const char *msg, uint32_t color);
void     gui_draw_str(uint64_t px, uint64_t py, const char *s, uint32_t fg, uint32_t bg);
void     gui_draw_str_fg(uint64_t px, uint64_t py, const char *s, uint32_t fg);
void     gui_draw_str_clip_fg(uint64_t px, uint64_t py, const char *s, uint32_t fg, uint64_t max_chars);
void     gui_draw_str_scaled(uint64_t px, uint64_t py, const char *s, uint64_t scale, uint32_t fg, uint32_t bg);
void     gui_draw_str_clip(uint64_t px, uint64_t py, const char *s, uint32_t fg, uint32_t bg, uint64_t max_chars);
char    *gui_itoa(int n, char *buf, int bufsz);
void     gui_ip4_str(uint32_t ip, char *buf, int bufsz);
void     gui_itoa_pad2(uint64_t n, char *out);
void     gui_draw_str_glyph_fg(uint64_t px, uint64_t py, uint8_t glyph_idx, uint32_t fg);

/* ── Forward declarations: gui_window.c ─────────────────────────────── */
void         z_raise(int slot);
void         raise_win(int slot);
bool         gui_is_topmost(int slot);
void         win_show(window_t *w, int slot);
void         win_hide(window_t *w, int slot);
void         win_maximize_toggle(window_t *w);
resize_dir_t hit_resize(window_t *w, int32_t mx, int32_t my);
void         win_do_resize(window_t *w, int32_t mx, int32_t my);
void         win_draw_chrome(window_t *w, bool fill_content);
void         win_render_content(window_t *w);
void         win_round_corners(const window_t *w);
void         term_set_viewport(window_t *w);
void         term_render_scrollback(window_t *w);

/* ── Forward declarations: gui_taskbar.c ────────────────────────────── */
void     taskbar_draw(void);
void     taskbar_draw_btn(int slot, const char *label);
void     taskbar_pill(uint64_t bx, uint64_t ty, uint64_t tbw, const char *label,
                      bool vis, bool focused, bool hov);
void     taskbar_draw_tray(void);
uint64_t logo_eff_w(void);
uint64_t taskbtn_start_x(void);
uint64_t taskbtn_w(void);

/* ── Forward declarations: gui_widgets.c ────────────────────────────── */
void     ctx_draw(void);
void     txt_ctx_draw(void);
void     fb_ctx_draw(void);
void     launcher_draw(void);
uint64_t ctx_w(void);
uint64_t fb_ctx_w(void);
uint64_t txt_ctx_w(void);
uint64_t launcher_lx(void);
uint64_t launcher_ly(void);
uint64_t launcher_eff_w(void);
uint64_t launcher_item_h(void);
void vol_popup_draw(void);
void fb_ctx_open_at(int win_slot, int row, bool is_dir, int32_t x, int32_t y);
void fb_ctx_run(int item);

/* ── Forward declarations: gui_render.c ─────────────────────────────── */
void full_redraw(void);
void tick_redraw(void);
void draw_status_bar(void);
void draw_desktop_bg(void);
void draw_desktop_icons(void);
uint64_t desk_icon_col_x(void);
void draw_desktop_info(void);
void draw_resize_hint(int slot, resize_dir_t dir);
int  desk_icon_at(int mx, int my);
void gui_draw_popups(void);

/* ── Forward declarations: gui_settings.c ───────────────────────────── */
void settings_render(window_t *w);

/* ── Forward declarations: gui_files.c ──────────────────────────────── */
void fb_str_copy(char *dst, const char *src, int maxlen);
void fb_render(window_t *w);
void fb_on_click(window_t *w, int32_t mx, int32_t my);
void fb_on_motion(window_t *w, int32_t mx, int32_t my);
void recent_add(const char *path);
void fb_navigate(fb_state_t *fb, const char *path);
void fb_path_join(char *out, const char *parent, const char *child);
void fb_path_parent(char *out, const char *path);
void fb_load(fb_state_t *fb, const char *path);
void fb_back(fb_state_t *fb);
void fb_forward(fb_state_t *fb);
bool fb_is_viewable(const char *name);
int  fb_hit_row(window_t *w, int32_t mx, int32_t my);
void fb_list_region(window_t *w, uint64_t *lx, uint64_t *ly, uint64_t *lw, uint64_t *lh);

/* ── Forward declarations: gui_text.c ───────────────────────────────── */
void text_render(window_t *w);
void text_open(window_t *w, const char *path);
void text_save(window_t *w);
void text_enter_edit(window_t *w);
void text_exit_edit(window_t *w);
void edit_scroll_to_cursor(window_t *w);
void edit_sync_pos(text_state_t *ts);
void edit_set_clipboard(const uint8_t *data, uint32_t len);
syn_lang_t detect_lang(const char *path);
/* edit operations called from gui_on_tick */
void edit_sel_clear(text_state_t *ts);
void    edit_sel_range(const text_state_t *ts, int32_t *lo, int32_t *hi);
void    edit_copy_to_clip(text_state_t *ts);
bool    edit_delete_selection(text_state_t *ts);
void    edit_paste(window_t *w);
void edit_push_undo(text_state_t *ts);
void edit_pop_undo(window_t *w);
void edit_pop_redo(window_t *w);
bool edit_insert(text_state_t *ts, uint8_t c);
void edit_del_before(text_state_t *ts);
void edit_del_at(text_state_t *ts);
void edit_del_word_before(text_state_t *ts);
void edit_del_word_at(text_state_t *ts);
void edit_move_left(text_state_t *ts);
void edit_move_right(text_state_t *ts);
void edit_move_word_left(text_state_t *ts);
void edit_move_word_right(text_state_t *ts);
void edit_move_end(text_state_t *ts);
void edit_move_up(text_state_t *ts);
void edit_move_down(text_state_t *ts);
void edit_move_line_up(window_t *w);
void edit_move_line_down(window_t *w);
void edit_indent_block(text_state_t *ts, bool indent);
void edit_toggle_comment(window_t *w);
void edit_dup_line(window_t *w);
void edit_kill_line(text_state_t *ts);
void edit_recount(window_t *w);
void text_replace_one(window_t *w);
int text_replace_all_impl(window_t *w);
void text_search_next(window_t *w, bool from_next);
void text_search_prev(window_t *w);
uint32_t text_xy_to_offset(window_t *w, int32_t mx, int32_t my);
uint32_t text_xy_to_offset_ro(window_t *w, int32_t mx, int32_t my);

/* ── Forward declarations: gui.c public API ─────────────────────────── */
void gui_add_desktop_icon(const char *path, const char *label);

#endif /* GUI_INTERNAL_H */
