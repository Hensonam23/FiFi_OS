#include "gui_internal.h"

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

void settings_render(window_t *w) {
    uint64_t ix = w->x + BORDER;
    uint64_t iy = w->y + TITLE_H;
    uint64_t iw = w->w - 2u * BORDER;
    uint64_t ih = w->h - TITLE_H - BORDER;
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();
    uint64_t cx = ix + SET_PAD;
    /* Value column -- clamp so it never overflows the window's right edge */
    uint64_t val_x = ix + SET_PAD + 18u * fw;
    if (iw > 4u && val_x >= ix + iw - 4u) val_x = ix + iw > 4u ? ix + iw - 4u : ix;
    /* One-shot debug: print dimensions on first render so logs reveal any layout issue */
    static bool s_dbg_logged = false;
    if (!s_dbg_logged) {
        s_dbg_logged = true;
        uint64_t cy0 = (uint64_t)((int64_t)(iy + SET_PAD) - (int64_t)g_settings_scroll);
        fprintf(stderr, "[settings] fb=%ux%u win=%ux%u ix=%u iy=%u iw=%u ih=%u fw=%u fh=%u scroll=%d cy0=%u SET_ROW_H=%u SET_SEC_H=%u val_x=%u\n",
                (unsigned)console_fb_width(), (unsigned)console_fb_height(),
                (unsigned)w->w, (unsigned)w->h,
                (unsigned)ix, (unsigned)iy, (unsigned)iw, (unsigned)ih,
                (unsigned)fw, (unsigned)fh, g_settings_scroll,
                (unsigned)cy0, (unsigned)SET_ROW_H, (unsigned)SET_SEC_H,
                (unsigned)(ix + SET_PAD + 18u * fw));
    }

    /* Compute total content height for scroll clamping */
    uint64_t h_sys   = (uint64_t)(SET_SEC_H + 4u) + 7u * SET_ROW_H + 6u + 12u + 5u;
    uint64_t h_disp  = (uint64_t)(SET_SEC_H + 4u) + (fh + 6u) + 4u + 5u;

    /* Dynamic button-row wrapping: how many wallpaper/toggle buttons fit per row */
    uint64_t btn_avail_w = iw > (2u * (uint64_t)SET_PAD + 18u * fw)
                         ? iw - 2u * (uint64_t)SET_PAD - 18u * fw : 10u * fw;
    uint64_t wall_bw_c   = (8u + 2u) * fw;   /* "Gradient" longest name + 2-char pad */
    int wall_per_row = (int)((btn_avail_w + 4u) / (wall_bw_c + 4u));
    if (wall_per_row < 1) wall_per_row = 1;
    if (wall_per_row > WALLPAPER_COUNT) wall_per_row = WALLPAPER_COUNT;
    int wall_rows = (WALLPAPER_COUNT + wall_per_row - 1) / wall_per_row;

    uint64_t tog_bw_c = 11u * fw;   /* toggle button width */
    int tog_per_row = (int)((btn_avail_w + 6u) / (tog_bw_c + 6u));
    if (tog_per_row < 1) tog_per_row = 1;
    if (tog_per_row > 4) tog_per_row = 4;
    int tog_rows = (4 + tog_per_row - 1) / tog_per_row;

    uint64_t h_theme = (uint64_t)(SET_SEC_H + 4u) + 2u * SET_ROW_H + 20u
                       + (uint64_t)wall_rows * (SET_ROW_H + 8u)
                       + (uint64_t)tog_rows  * (SET_ROW_H + 8u)
                       + (fh + 6u) + 4u + 5u;
    uint64_t h_audio = (uint64_t)(SET_SEC_H + 4u) + (fh + 6u) + 4u + (fh + 6u) + 4u + 5u;
    uint64_t h_net   = (uint64_t)(SET_SEC_H + 4u) + 6u * SET_ROW_H + 2u * SET_ROW_H + 5u;
    /* shortcuts table — defined here so we can count it for total_h */
    struct { const char *key; const char *desc; } shortcuts[] = {
        { "F1",             "Toggle Terminal"             },
        { "F2",             "Toggle Files"                },
        { "F3",             "Toggle Settings"             },
        { "F4",             "Toggle Text Viewer"          },
        { "F5",             "Launch Sys Monitor"          },
        { "F6",             "Launch Net Monitor"          },
        { "F7",             "Launch Calculator"           },
        { "F11",            "Volume down"                 },
        { "F12",            "Volume up"                   },
        { "Super+Left",     "Snap window left half"       },
        { "Super+Right",    "Snap window right half"      },
        { "Super+Up",       "Maximize window"             },
        { "Super+Down",     "Restore window"              },
        { "Super+L",        "Lock screen"                 },
        { "Super+D",        "Show/hide desktop"           },
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
    int nsc = 0; while (shortcuts[nsc].key) nsc++;
    uint64_t h_sc      = (uint64_t)(SET_SEC_H + 4u) + (uint64_t)nsc * SET_ROW_H;
    uint64_t h_privacy = (uint64_t)(SET_SEC_H + 4u) + SET_ROW_H + 5u;
    /* VPN section: header + status + connect-button + auto-connect + hint/separator */
    uint64_t h_vpn     = (uint64_t)(SET_SEC_H + 4u) + 4u * SET_ROW_H + 5u;
    /* WiFi section: header + status + optional hint row + separator */
    uint64_t h_wifi    = (uint64_t)(SET_SEC_H + 4u) + 2u * SET_ROW_H + 5u;
    uint64_t total_h = h_sys + h_disp + h_theme + h_audio + h_net + h_vpn + h_wifi + h_privacy + h_sc + (uint64_t)SET_PAD;
    g_settings_total_h = (int)total_h;
    /* Clamp scroll */
    if ((int64_t)total_h > (int64_t)ih) {
        int max_scroll = (int)(total_h - ih);
        if (g_settings_scroll > max_scroll) g_settings_scroll = max_scroll;
    } else {
        g_settings_scroll = 0;
    }
    if (g_settings_scroll < 0) g_settings_scroll = 0;

    /* cy is int64 so we can handle scroll offset: negative cy → above viewport → no-op draws */
    int64_t cy = (int64_t)(iy + SET_PAD) - (int64_t)g_settings_scroll;

    /* Reset all hitboxes — only re-set when section is in the visible area */
    g_font_prev_bx = 0; g_font_next_bx = 0; g_font_btn_by = 0; g_font_btn_bh = 0;
    g_theme_accent_by = 0; g_theme_accent_by2 = 0;
    g_theme_wall_by = 0; g_theme_wall_bh = 0; g_theme_wall_bw = 0;
    g_theme_toggle_h = 0; g_theme_toggle_w = 0;
    g_utc_btn_by = 0; g_utc_btn_bh = 0;
    g_vol_btn_by = 0; g_vol_btn_bh = 0;
    g_vol_pop_btn_y = 0; g_vol_chime_bw = 0; g_vol_chime_bh = 0;
    g_gaming_btn_bh = 0; g_gaming_mode_bh = 0;
    g_fw_btn_bh = 0; g_dns_btn_bh = 0; g_vpn_btn_bh = 0; g_vpn_auto_bh = 0; g_lto_btn_bh = 0;

    console_fill_rect(ix, iy, iw, ih, COL_SET_BG);

    /* ── Section: System ── */
    if (SVIS) {
        console_fill_rect(ix, SCY, iw, SET_SEC_H, COL_SET_SEC_BG);
        gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_SEC_H - fh) / 2u)), "System Information",
                     COL_SET_SEC_FG, COL_SET_SEC_BG);
    }
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
    /* Build CPU frequency string */
    char cpu_str[16];
    {
        /* sys_cpu_freq_mhz() is provided by Linux platform.c; returns 0 if unavailable */
        __attribute__((weak)) uint32_t sys_cpu_freq_mhz(void);
        uint32_t mhz = sys_cpu_freq_mhz ? sys_cpu_freq_mhz() : 0u;
        if (mhz == 0) {
            cpu_str[0] = '?'; cpu_str[1] = '\0';
        } else if (mhz >= 1000) {
            char gb[6];
            gui_itoa((int)(mhz / 1000), gb, 6);
            int ri = 0;
            for (int k = 0; gb[k] && ri < 12; ) cpu_str[ri++] = gb[k++];
            cpu_str[ri++] = '.';
            cpu_str[ri++] = (char)('0' + (mhz % 1000) / 100);
            cpu_str[ri++] = ' '; cpu_str[ri++] = 'G'; cpu_str[ri++] = 'H'; cpu_str[ri++] = 'z';
            cpu_str[ri] = '\0';
        } else {
            char mb[6];
            gui_itoa((int)mhz, mb, 6);
            int ri = 0;
            for (int k = 0; mb[k] && ri < 9; ) cpu_str[ri++] = mb[k++];
            cpu_str[ri++] = ' '; cpu_str[ri++] = 'M'; cpu_str[ri++] = 'H'; cpu_str[ri++] = 'z';
            cpu_str[ri] = '\0';
        }
    }
    /* Use human-readable font label from picker instead of raw filename */
    int nfonts = 0; while (g_font_paths[nfonts]) nfonts++;
    const char *font_label = g_font_labels[g_font_idx < nfonts ? g_font_idx : 0];

    __attribute__((weak)) const char *platform_kernel_str(void);
    const char *kern_ver = (platform_kernel_str && platform_kernel_str())
                         ? platform_kernel_str() : "freestanding";

    struct { const char *key; const char *val; } sysinfo[] = {
        { "OS:",         "FiFi OS linux-desktop"  },
        { "Kernel:",     kern_ver                 },
        { "Memory:",     mem_str                  },
        { "Resolution:", res_str                  },
        { "CPU:",        cpu_str                  },
        { "Font:",       font_label               },
        { "Uptime:",     up_str                   },
        { NULL, NULL }
    };
    for (int i = 0; sysinfo[i].key; i++) {
        SADVBOT;
        if (SVIS) {
            uint32_t bg = (i & 1) ? 0x000f151fu : COL_SET_BG;
            uint64_t row_y = (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u));
            uint64_t val_max = (ix + iw > val_x + (uint64_t)SET_PAD)
                             ? (ix + iw - val_x - (uint64_t)SET_PAD) / fw : 1u;
            console_fill_rect(ix, SCY, iw, SET_ROW_H, bg);
            gui_draw_str(cx, row_y, sysinfo[i].key, COL_SET_KEY_FG, bg);
            gui_draw_str_clip(val_x, row_y, sysinfo[i].val, COL_SET_VAL_FG, bg, val_max);
        }
        cy += SET_ROW_H;
    }

    /* Memory usage bar */
    cy += 6u;
    if (SVIS) {
        uint64_t bar_w  = iw > 2u * (uint64_t)SET_PAD ? iw - 2u * (uint64_t)SET_PAD : 1u;
        uint64_t used_p = pmm_get_total_pages() - pmm_get_free_pages();
        uint64_t tot_p  = pmm_get_total_pages();
        uint64_t fill   = tot_p > 0 ? used_p * bar_w / tot_p : 0u;
        console_fill_rect(cx, SCY, bar_w, 8u, 0x0008101cu);
        if (fill > 0) console_fill_rect(cx, SCY, fill, 8u, 0x00306898u);
        console_fill_rect(cx, SCY, bar_w, 1u, 0x00203040u);
        console_fill_rect(cx, (uint64_t)(cy + 7), bar_w, 1u, 0x00203040u);
    }
    cy += 12u;

    if (SVIS) console_fill_rect(ix, SCY, iw, 1u, COL_SET_SEP);
    cy += 5u;
    SADVBOT;

    /* ── Section: Display ── */
    {
        if (SVIS) {
            console_fill_rect(ix, SCY, iw, SET_SEC_H, COL_SET_SEC_BG);
            gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_SEC_H - fh) / 2u)), "Display",
                         COL_SET_SEC_FG, COL_SET_SEC_BG);
        }
        cy += SET_SEC_H + 4u;
        SADVBOT;

        /* Font row: [<] FontName [>] */
        uint64_t btn_h = fh + 6u;
        if (SVIS) {
            console_fill_rect(ix, SCY, iw, btn_h, COL_SET_BG);
            gui_draw_str(cx, (uint64_t)(cy + (int64_t)((btn_h - fh) / 2u)), "Font:", COL_SET_KEY_FG, COL_SET_BG);
            /* Prev button */
            uint64_t pb_x = val_x;
            console_fill_rect(pb_x, SCY, g_font_btn_bw, btn_h, 0x00182838u);
            gui_draw_str(pb_x + (g_font_btn_bw - fw) / 2u, (uint64_t)(cy + (int64_t)((btn_h - fh) / 2u)),
                         "<", 0x0060a0e0u, 0x00182838u);
            g_font_prev_bx = pb_x;
            /* Font label */
            uint64_t fl_x = pb_x + g_font_btn_bw + 4u;
            uint64_t nxt_x = ix + iw - SET_PAD - g_font_btn_bw;
            uint64_t fl_w  = nxt_x > fl_x + 2u ? nxt_x - fl_x - 2u : 1u;
            uint64_t fl_max = fl_w / fw;
            int nf = 0; while (g_font_paths[nf]) nf++;
            int idx = g_font_idx < nf ? g_font_idx : 0;
            gui_draw_str_clip(fl_x, (uint64_t)(cy + (int64_t)((btn_h - fh) / 2u)),
                              g_font_labels[idx], COL_SET_VAL_FG, COL_SET_BG, fl_max);
            /* Next button */
            console_fill_rect(nxt_x, SCY, g_font_btn_bw, btn_h, 0x00182838u);
            gui_draw_str(nxt_x + (g_font_btn_bw - fw) / 2u, (uint64_t)(cy + (int64_t)((btn_h - fh) / 2u)),
                         ">", 0x0060a0e0u, 0x00182838u);
            g_font_next_bx = nxt_x;
            g_font_btn_by  = SCY;
            g_font_btn_bh  = btn_h;
        }
        cy += btn_h + 4u;
        if (SVIS) console_fill_rect(ix, SCY, iw, 1u, COL_SET_SEP);
        cy += 5u;
    }
    SADVBOT;

    /* ── Section: Theme ── */
    {
        if (SVIS) {
            console_fill_rect(ix, SCY, iw, SET_SEC_H, COL_SET_SEC_BG);
            gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_SEC_H - fh) / 2u)), "Theme",
                         COL_SET_SEC_FG, COL_SET_SEC_BG);
        }
        cy += SET_SEC_H + 4u;

        /* Accent colour rows (16 swatches, 8 per row) */
        uint64_t sw_sz = (uint64_t)(fh + 4u);
        uint64_t sw_gap = 4u;
        g_theme_swatch_sz = sw_sz;

        /* Row 1 */
        SADVBOT;
        if (SVIS) {
            console_fill_rect(ix, SCY, iw, SET_ROW_H + 8u, COL_SET_BG);
            gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u + 2u)),
                         "Accent:", COL_SET_KEY_FG, COL_SET_BG);
            g_theme_accent_by = (uint64_t)(cy + (int64_t)((SET_ROW_H + 8u - sw_sz) / 2u));
        }
        /* Recompute sw_sz to fit all 8 swatches within available width */
        {
            uint64_t avail = (val_x < ix + iw) ? (ix + iw - val_x) : 0u;
            uint64_t per = avail / 8u;
            if (per > 4u && per - 4u < sw_sz) sw_sz = per - 4u;
            if (sw_sz < 4u) sw_sz = 4u;
            g_theme_swatch_sz = sw_sz;
        }
        uint64_t sw_x = val_x;
        for (int ai = 0; ai < ACCENT_PRESET_COUNT; ai++) {
            if (ai == 8) {
                cy += SET_ROW_H + 8u;
                SADVBOT;
                if (SVIS) {
                    console_fill_rect(ix, SCY, iw, SET_ROW_H + 4u, COL_SET_BG);
                    g_theme_accent_by2 = (uint64_t)(cy + (int64_t)((SET_ROW_H + 4u - sw_sz) / 2u));
                }
                sw_x = val_x;
            }
            g_theme_accent_bx[ai] = sw_x;
            /* Skip drawing if swatch would overflow window */
            if (sw_x + sw_sz > ix + iw) { sw_x += sw_sz + sw_gap; continue; }
            uint64_t swy = (ai < 8) ? g_theme_accent_by : g_theme_accent_by2;
            if (swy > 0u) {
                bool active = (g_accent_presets[ai] == g_theme.accent);
                console_fill_rect(sw_x, swy, sw_sz, sw_sz, g_accent_presets[ai]);
                if (active) {
                    console_fill_rect(sw_x, swy, sw_sz, 2u, 0x00ffffffu);
                    console_fill_rect(sw_x, swy + sw_sz - 2u, sw_sz, 2u, 0x00ffffffu);
                    console_fill_rect(sw_x, swy, 2u, sw_sz, 0x00ffffffu);
                    console_fill_rect(sw_x + sw_sz - 2u, swy, 2u, sw_sz, 0x00ffffffu);
                }
            }
            sw_x += sw_sz + sw_gap;
        }
        cy += SET_ROW_H + 12u;

        /* Wallpaper selector row(s) — wraps dynamically based on available width */
        {
            static const char *wall_names[WALLPAPER_COUNT] = {
                "Gradient", "Solid", "Stars", "Grid", "Waves", "Image"
            };
            uint64_t wall_bh = (uint64_t)(fh + 6u);
            uint64_t wall_bw = wall_bw_c;
            g_theme_wall_bh = wall_bh;
            g_theme_wall_bw = wall_bw;
            for (int _wi = 0; _wi < WALLPAPER_COUNT; _wi++) {
                g_theme_wall_bx[_wi] = 0; g_theme_wall_by_arr[_wi] = 0;
            }
            g_theme_wall_by = 0;
            for (int wrow = 0; wrow < wall_rows; wrow++) {
                SADVBOT;
                if (SVIS) {
                    console_fill_rect(ix, SCY, iw, SET_ROW_H + 4u, COL_SET_BG);
                    if (wrow == 0)
                        gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u + 2u)),
                                     "Wallpaper:", COL_SET_KEY_FG, COL_SET_BG);
                }
                uint64_t wx  = val_x;
                uint64_t wy  = (uint64_t)(cy + (int64_t)((SET_ROW_H + 4u - wall_bh) / 2u));
                int i_start  = wrow * wall_per_row;
                int i_end    = i_start + wall_per_row;
                if (i_end > WALLPAPER_COUNT) i_end = WALLPAPER_COUNT;
                for (int wi = i_start; wi < i_end; wi++) {
                    g_theme_wall_bx[wi]     = wx;
                    g_theme_wall_by_arr[wi] = wy;
                    if (wi == 0) g_theme_wall_by = wy;
                    if (SVIS) {
                        bool active = (wi == g_theme.wallpaper);
                        uint32_t bbg = active ? g_theme.accent : 0x00182838u;
                        uint32_t bfg = active ? 0x00ffffffu   : 0x0090b0d0u;
                        console_fill_rect(wx, wy, wall_bw, wall_bh, bbg);
                        uint64_t nl  = (uint64_t)gui_strlen(wall_names[wi]);
                        uint64_t bpx = wx + (wall_bw > nl * fw ? (wall_bw - nl * fw) / 2u : 0u);
                        uint64_t bpy = wy + (wall_bh - fh) / 2u;
                        gui_draw_str(bpx, bpy, wall_names[wi], bfg, bbg);
                    }
                    wx += wall_bw + 4u;
                }
                cy += SET_ROW_H + 8u;
            }
        }

        /* Toggle row(s): Clock 12h, Animations, Status Bar, Desk Info — wraps dynamically */
        {
            static const char *tog_labels[4] = { "12h Clock", "Animations", "Status Bar", "Desk Info" };
            bool tog_vals[4] = { g_theme.clock_12h, g_theme.animations, g_theme.statusbar, g_theme.desktop_info };
            uint64_t tbh = (uint64_t)(fh + 6u);
            uint64_t tbw = tog_bw_c;
            g_theme_toggle_h = tbh;
            g_theme_toggle_w = tbw;
            for (int ti = 0; ti < 4; ti++) { g_theme_toggle_x[ti] = 0; g_theme_toggle_y[ti] = 0; }
            for (int trow = 0; trow < tog_rows; trow++) {
                SADVBOT;
                if (SVIS)
                    console_fill_rect(ix, SCY, iw, SET_ROW_H + 4u, COL_SET_BG);
                uint64_t tx  = val_x;
                uint64_t ty2 = (uint64_t)(cy + (int64_t)((SET_ROW_H + 4u - tbh) / 2u));
                int i_start  = trow * tog_per_row;
                int i_end    = i_start + tog_per_row;
                if (i_end > 4) i_end = 4;
                for (int ti = i_start; ti < i_end; ti++) {
                    g_theme_toggle_x[ti] = tx;
                    g_theme_toggle_y[ti] = ty2;
                    if (SVIS) {
                        bool on = tog_vals[ti];
                        uint32_t tbg = on ? g_theme.accent : 0x00182838u;
                        uint32_t tfg = on ? 0x00ffffffu    : 0x00607080u;
                        console_fill_rect(tx, ty2, tbw, tbh, tbg);
                        uint64_t nl  = (uint64_t)gui_strlen(tog_labels[ti]);
                        uint64_t tpx = tx + (tbw > nl * fw ? (tbw - nl * fw) / 2u : 0u);
                        uint64_t tpy = ty2 + (tbh - fh) / 2u;
                        gui_draw_str(tpx, tpy, tog_labels[ti], tfg, tbg);
                    }
                    tx += tbw + 6u;
                }
                cy += SET_ROW_H + 8u;
            }
        }

        /* UTC offset row: [−]  UTC+N  [+] */
        SADVBOT;
        {
            uint64_t btn_h2 = fh + 6u;
            if (SVIS) {
                console_fill_rect(ix, SCY, iw, btn_h2, COL_SET_BG);
                gui_draw_str(cx, (uint64_t)(cy + (int64_t)((btn_h2 - fh) / 2u)),
                             "Clock UTC:", COL_SET_KEY_FG, COL_SET_BG);
                uint64_t pb2 = val_x;
                console_fill_rect(pb2, SCY, g_font_btn_bw, btn_h2, 0x00182838u);
                gui_draw_str(pb2 + (g_font_btn_bw - fw) / 2u,
                             (uint64_t)(cy + (int64_t)((btn_h2 - fh) / 2u)),
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
                console_fill_rect(lbl_x, SCY, lbl_w, btn_h2, COL_SET_BG);
                gui_draw_str(lbl_cx2, (uint64_t)(cy + (int64_t)((btn_h2 - fh) / 2u)),
                             utc_lbl, COL_SET_VAL_FG, COL_SET_BG);
                console_fill_rect(plus2, SCY, g_font_btn_bw, btn_h2, 0x00182838u);
                gui_draw_str(plus2 + (g_font_btn_bw - fw) / 2u,
                             (uint64_t)(cy + (int64_t)((btn_h2 - fh) / 2u)),
                             "+", 0x0060a0e0u, 0x00182838u);
                g_utc_plus_bx = plus2;
                g_utc_btn_by  = SCY;
                g_utc_btn_bh  = btn_h2;
            }
            cy += btn_h2 + 4u;
        }

        if (SVIS) console_fill_rect(ix, SCY, iw, 1u, COL_SET_SEP);
        cy += 5u;
    }
    SADVBOT;

    /* ── Section: Audio ── */
    {
        if (SVIS) {
            console_fill_rect(ix, SCY, iw, SET_SEC_H, COL_SET_SEC_BG);
            gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_SEC_H - fh) / 2u)), "Audio",
                         COL_SET_SEC_FG, COL_SET_SEC_BG);
        }
        cy += SET_SEC_H + 4u;
        SADVBOT;

        uint64_t btn_ha = fh + 6u;

        /* Volume row: [−]  NN%  [+] */
        if (SVIS) {
            console_fill_rect(ix, SCY, iw, btn_ha, COL_SET_BG);
            if (hda_is_ready()) {
                gui_draw_str(cx, (uint64_t)(cy + (int64_t)((btn_ha - fh) / 2u)),
                             "Volume:", COL_SET_KEY_FG, COL_SET_BG);
                uint64_t vm_x = val_x;
                console_fill_rect(vm_x, SCY, g_font_btn_bw, btn_ha, 0x00182838u);
                gui_draw_str(vm_x + (g_font_btn_bw - fw) / 2u,
                             (uint64_t)(cy + (int64_t)((btn_ha - fh) / 2u)),
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
                console_fill_rect(vl_x, SCY, vl_w, btn_ha, COL_SET_BG);
                gui_draw_str(vl_cx, (uint64_t)(cy + (int64_t)((btn_ha - fh) / 2u)),
                             vol_lbl, COL_SET_VAL_FG, COL_SET_BG);
                console_fill_rect(vp_x, SCY, g_font_btn_bw, btn_ha, 0x00182838u);
                gui_draw_str(vp_x + (g_font_btn_bw - fw) / 2u,
                             (uint64_t)(cy + (int64_t)((btn_ha - fh) / 2u)),
                             "+", 0x0060a0e0u, 0x00182838u);
                g_vol_plus_bx = vp_x;
                g_vol_btn_by  = SCY;
                g_vol_btn_bh  = btn_ha;
            } else {
                gui_draw_str(cx, (uint64_t)(cy + (int64_t)((btn_ha - fh) / 2u)),
                             "No audio device", COL_SET_HINT, COL_SET_BG);
                g_vol_btn_bh = 0u;
            }
        }
        cy += btn_ha + 4u;

        /* Test Tone button */
        SADVBOT;
        if (hda_is_ready()) {
            if (SVIS) {
                static const char *chime_lbl = "Chime";
                uint64_t cbl = (uint64_t)gui_strlen(chime_lbl);
                uint64_t cbw = (cbl + 2u) * fw;
                uint64_t cbx = val_x;
                console_fill_rect(ix, SCY, iw, btn_ha, COL_SET_BG);
                gui_draw_str(cx, (uint64_t)(cy + (int64_t)((btn_ha - fh) / 2u)),
                             "Test:", COL_SET_KEY_FG, COL_SET_BG);
                console_fill_rect(cbx, SCY, cbw, btn_ha, 0x00182838u);
                uint64_t cpx = cbx + (cbw - cbl * fw) / 2u;
                gui_draw_str(cpx, (uint64_t)(cy + (int64_t)((btn_ha - fh) / 2u)),
                             chime_lbl, 0x0060a0e0u, 0x00182838u);
                g_vol_chime_bx = cbx; g_vol_chime_by = SCY;
                g_vol_chime_bw = cbw; g_vol_chime_bh = btn_ha;
            }
            cy += btn_ha + 4u;
        } else {
            g_vol_chime_bh = 0u;
        }

        if (SVIS) console_fill_rect(ix, SCY, iw, 1u, COL_SET_SEP);
        cy += 5u;
    }
    SADVBOT;

    /* ── Section: Gaming (only shown when gamepad connected) ── */
    {
        extern bool input_gamepad_connected(void);
        if (input_gamepad_connected()) {
            uint64_t gm_btn_h = fh + 6u;
            if (SVIS) {
                console_fill_rect(ix, SCY, iw, SET_SEC_H, COL_SET_SEC_BG);
                gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_SEC_H - fh) / 2u)), "Gaming",
                             COL_SET_SEC_FG, COL_SET_SEC_BG);
            }
            cy += SET_SEC_H + 4u;
            SADVBOT;

            /* Gamepad status row */
            if (SVIS) {
                bool gp = input_gamepad_connected();
                uint32_t bg = COL_SET_BG;
                console_fill_rect(ix, SCY, iw, SET_ROW_H, bg);
                gui_draw_str(cx,    (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                             "Gamepad:", COL_SET_KEY_FG, bg);
                gui_draw_str(val_x, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                             gp ? "Connected" : "None", gp ? 0x0060d880u : COL_SET_HINT, bg);
            }
            cy += SET_ROW_H;
            SADVBOT;

            /* Launch Gamepad Visualizer button */
            if (SVIS) {
                static const char *gvlbl = "Launch";
                uint64_t gvl = (uint64_t)gui_strlen(gvlbl);
                uint64_t gvw = (gvl + 2u) * fw;
                uint64_t gvx = val_x;
                console_fill_rect(ix, SCY, iw, gm_btn_h, COL_SET_BG);
                gui_draw_str(cx, (uint64_t)(cy + (int64_t)((gm_btn_h - fh) / 2u)),
                             "Gamepad App:", COL_SET_KEY_FG, COL_SET_BG);
                console_fill_rect(gvx, SCY, gvw, gm_btn_h, 0x00182838u);
                uint64_t gvpx = gvx + (gvw - gvl * fw) / 2u;
                gui_draw_str(gvpx, (uint64_t)(cy + (int64_t)((gm_btn_h - fh) / 2u)),
                             gvlbl, 0x0060a0e0u, 0x00182838u);
                g_gaming_btn_bx = gvx; g_gaming_btn_by = SCY;
                g_gaming_btn_bw = gvw; g_gaming_btn_bh = gm_btn_h;
            }
            cy += gm_btn_h + 4u;
            SADVBOT;

            /* Gaming Mode toggle button */
            {
                extern bool gaming_mode_active(void);
                bool gm_on = gaming_mode_active();
                if (SVIS) {
                    const char *gm_lbl = gm_on ? "ON " : "OFF";
                    uint64_t gbl = 3u;
                    uint64_t gbw = (gbl + 2u) * fw;
                    uint64_t gbx = val_x;
                    console_fill_rect(ix, SCY, iw, gm_btn_h, COL_SET_BG);
                    gui_draw_str(cx, (uint64_t)(cy + (int64_t)((gm_btn_h - fh) / 2u)),
                                 "Gaming Mode:", COL_SET_KEY_FG, COL_SET_BG);
                    uint32_t gm_bg = gm_on ? 0x00103820u : 0x00182838u;
                    uint32_t gm_fg = gm_on ? 0x0050e880u : 0x0060a0e0u;
                    console_fill_rect(gbx, SCY, gbw, gm_btn_h, gm_bg);
                    uint64_t gpx = gbx + (gbw - gbl * fw) / 2u;
                    gui_draw_str(gpx, (uint64_t)(cy + (int64_t)((gm_btn_h - fh) / 2u)),
                                 gm_lbl, gm_fg, gm_bg);
                    g_gaming_mode_bx = gbx; g_gaming_mode_by = SCY;
                    g_gaming_mode_bw = gbw; g_gaming_mode_bh = gm_btn_h;
                }
                cy += gm_btn_h + 4u;
            }

            if (SVIS) console_fill_rect(ix, SCY, iw, 1u, COL_SET_SEP);
            cy += 5u;
        }
    }
    SADVBOT;

    /* ── Section: Network ── */
    {
        if (SVIS) {
            console_fill_rect(ix, SCY, iw, SET_SEC_H, COL_SET_SEC_BG);
            gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_SEC_H - fh) / 2u)), "Network",
                         COL_SET_SEC_FG, COL_SET_SEC_BG);
        }
        cy += SET_SEC_H + 4u;
        SADVBOT;

        char ip_str[16], mask_str[16], gw_str[16], dns_str[16];
        gui_ip4_str(net_ip,      ip_str,   16);
        gui_ip4_str(net_mask,    mask_str, 16);
        gui_ip4_str(net_gateway, gw_str,   16);
        gui_ip4_str(net_dns,     dns_str,  16);

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
            SADVBOT;
            if (SVIS) {
                uint32_t bg = (i & 1) ? 0x000f151fu : COL_SET_BG;
                console_fill_rect(ix, SCY, iw, SET_ROW_H, bg);
                gui_draw_str(cx,    (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                             netinfo[i].key, COL_SET_KEY_FG, bg);
                uint32_t vfg = (i == 0 && net_ip)  ? 0x0060d880u :
                               (i == 0 && !net_ip) ? 0x00e88060u : COL_SET_VAL_FG;
                gui_draw_str(val_x, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                             netinfo[i].val, vfg, bg);
            }
            cy += SET_ROW_H;
        }

        /* ── Firewall toggle row ── */
        {
            __attribute__((weak)) bool gui_firewall_active(void);
            if (g_fw_state < 0 && gui_firewall_active)
                g_fw_state = gui_firewall_active() ? 1 : 0;
            bool fw_on = (g_fw_state == 1);
            SADVBOT;
            if (SVIS) {
                uint32_t bg = COL_SET_BG;
                console_fill_rect(ix, SCY, iw, SET_ROW_H, bg);
                gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                             "Firewall:", COL_SET_KEY_FG, bg);
                uint64_t fbw = 5u * fw + 2u * fw;
                uint64_t fbx = val_x;
                uint32_t fb_bg = fw_on ? 0x00103820u : 0x00381018u;
                uint32_t fb_fg = fw_on ? 0x0050e880u : 0x00e05050u;
                console_fill_rect(fbx, SCY, fbw, SET_ROW_H, fb_bg);
                const char *fw_lbl = fw_on ? " ON  " : " OFF ";
                gui_draw_str(fbx + fw, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                             fw_lbl, fb_fg, fb_bg);
                g_fw_btn_bx = fbx; g_fw_btn_by = SCY;
                g_fw_btn_bw = fbw; g_fw_btn_bh = SET_ROW_H;
            }
            cy += SET_ROW_H;
        }

        /* ── DNS server row ── */
        {
            static const char *dns_labels[] = { "Default", "Cloudflare", "Quad9" };
            SADVBOT;
            if (SVIS) {
                uint32_t bg = 0x000f151fu;
                console_fill_rect(ix, SCY, iw, SET_ROW_H, bg);
                gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                             "DNS Server:", COL_SET_KEY_FG, bg);
                const char *lbl = dns_labels[g_dns_mode];
                uint64_t dbw = (uint64_t)(gui_strlen(lbl) + 2u) * fw;
                uint64_t dbx = val_x;
                uint32_t db_bg = (g_dns_mode == 0) ? 0x00203040u :
                                 (g_dns_mode == 1) ? 0x00102820u : 0x00201030u;
                uint32_t db_fg = (g_dns_mode == 0) ? 0x0080a8d0u :
                                 (g_dns_mode == 1) ? 0x0050e880u : 0x00c080e0u;
                console_fill_rect(dbx, SCY, dbw, SET_ROW_H, db_bg);
                gui_draw_str(dbx + fw, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                             lbl, db_fg, db_bg);
                g_dns_btn_bx = dbx; g_dns_btn_by = SCY;
                g_dns_btn_bw = dbw; g_dns_btn_bh = SET_ROW_H;
            }
            cy += SET_ROW_H;
        }

        if (SVIS) console_fill_rect(ix, SCY, iw, 1u, COL_SET_SEP);
        cy += 5u;
    }
    SADVBOT;

    /* ── Section: VPN (WireGuard) ── */
    {
        __attribute__((weak)) bool gui_vpn_connected(void);
        __attribute__((weak)) bool gui_vpn_has_config(void);
        __attribute__((weak)) bool gui_vpn_autoconnect_enabled(void);

        bool vpn_on     = gui_vpn_connected        && gui_vpn_connected();
        bool has_cfg    = gui_vpn_has_config       && gui_vpn_has_config();
        bool auto_on    = gui_vpn_autoconnect_enabled && gui_vpn_autoconnect_enabled();

        if (SVIS) {
            console_fill_rect(ix, SCY, iw, SET_SEC_H, COL_SET_SEC_BG);
            gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_SEC_H - fh) / 2u)),
                         "VPN (WireGuard)", COL_SET_SEC_FG, COL_SET_SEC_BG);
        }
        cy += SET_SEC_H + 4u;
        SADVBOT;

        /* Status row */
        SADVBOT;
        if (SVIS) {
            uint32_t bg = COL_SET_BG;
            console_fill_rect(ix, SCY, iw, SET_ROW_H, bg);
            gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                         "Status:", COL_SET_KEY_FG, bg);
            const char *vstatus = vpn_on  ? "Connected"    :
                                  has_cfg ? "Disconnected" : "No config";
            uint32_t vfg = vpn_on  ? 0x0060d880u :
                           has_cfg ? 0x00e88060u  : COL_SET_HINT;
            gui_draw_str(val_x, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                         vstatus, vfg, bg);
        }
        cy += SET_ROW_H;

        /* Connect / Disconnect button (only if a config file exists) */
        if (has_cfg) {
            SADVBOT;
            if (SVIS) {
                uint32_t bg = 0x000f151fu;
                console_fill_rect(ix, SCY, iw, SET_ROW_H, bg);
                gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                             "WireGuard:", COL_SET_KEY_FG, bg);
                const char *lbl   = vpn_on ? " Disconnect " : " Connect ";
                uint64_t    llen  = (uint64_t)gui_strlen(lbl);
                uint64_t    vbw   = llen * fw;
                uint32_t    vb_bg = vpn_on ? 0x00381018u : 0x00103820u;
                uint32_t    vb_fg = vpn_on ? 0x00e05050u : 0x0050e880u;
                console_fill_rect(val_x, SCY, vbw, SET_ROW_H, vb_bg);
                gui_draw_str(val_x, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                             lbl, vb_fg, vb_bg);
                g_vpn_btn_bx = val_x; g_vpn_btn_by = SCY;
                g_vpn_btn_bw = vbw;   g_vpn_btn_bh = SET_ROW_H;
            }
            cy += SET_ROW_H;
        }

        /* Auto-connect on boot toggle */
        {
            SADVBOT;
            if (SVIS) {
                uint32_t bg = COL_SET_BG;
                console_fill_rect(ix, SCY, iw, SET_ROW_H, bg);
                gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                             "Auto-connect:", COL_SET_KEY_FG, bg);
                const char *albl   = auto_on ? " ON  " : " OFF ";
                uint64_t    alen   = (uint64_t)gui_strlen(albl);
                uint64_t    ab_w   = alen * fw;
                uint32_t    ab_bg  = auto_on ? 0x00103820u : 0x00381018u;
                uint32_t    ab_fg  = auto_on ? 0x0050e880u : 0x00e05050u;
                console_fill_rect(val_x, SCY, ab_w, SET_ROW_H, ab_bg);
                gui_draw_str(val_x, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                             albl, ab_fg, ab_bg);
                g_vpn_auto_bx = val_x; g_vpn_auto_by = SCY;
                g_vpn_auto_bw = ab_w;  g_vpn_auto_bh = SET_ROW_H;
            }
            cy += SET_ROW_H;
        }

        /* Hint when no config is present */
        if (!has_cfg) {
            SADVBOT;
            if (SVIS) {
                uint32_t bg = COL_SET_BG;
                console_fill_rect(ix, SCY, iw, SET_ROW_H, bg);
                uint64_t max_ch_vpn = fw > 0u ? (uint64_t)(ix + iw - cx) / fw : 40u;
                gui_draw_str_clip(cx, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                                  "Place wg0.conf in /fifi-data/ to connect",
                                  COL_SET_HINT, bg, max_ch_vpn);
            }
            cy += SET_ROW_H;
        }

        if (SVIS) console_fill_rect(ix, SCY, iw, 1u, COL_SET_SEP);
        cy += 5u;
    }
    SADVBOT;

    /* ── Section: WiFi ── */
    {
        __attribute__((weak)) bool gui_wifi_connected(void);
        __attribute__((weak)) bool gui_wifi_has_config(void);
        __attribute__((weak)) void gui_wifi_ssid(char *out, int outlen);

        bool wifi_on      = gui_wifi_connected  && gui_wifi_connected();
        bool wifi_cfg     = gui_wifi_has_config && gui_wifi_has_config();

        if (SVIS) {
            console_fill_rect(ix, SCY, iw, SET_SEC_H, COL_SET_SEC_BG);
            gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_SEC_H - fh) / 2u)),
                         "WiFi", COL_SET_SEC_FG, COL_SET_SEC_BG);
        }
        cy += SET_SEC_H + 4u;
        SADVBOT;

        /* Status row */
        SADVBOT;
        if (SVIS) {
            uint32_t bg = COL_SET_BG;
            console_fill_rect(ix, SCY, iw, SET_ROW_H, bg);
            gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                         "Status:", COL_SET_KEY_FG, bg);
            char ssid[64] = {0};
            const char *wstatus;
            uint32_t wfg;
            if (wifi_on && gui_wifi_ssid) {
                gui_wifi_ssid(ssid, sizeof(ssid));
                wstatus = ssid[0] ? ssid : "Connected";
                wfg = 0x0060d880u;
            } else if (wifi_cfg) {
                wstatus = "Connecting...";
                wfg = 0x00e88060u;
            } else {
                wstatus = "No config";
                wfg = COL_SET_HINT;
            }
            gui_draw_str(val_x, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                         wstatus, wfg, bg);
        }
        cy += SET_ROW_H;

        /* Setup hint when no config */
        if (!wifi_cfg) {
            SADVBOT;
            if (SVIS) {
                uint32_t bg = COL_SET_BG;
                console_fill_rect(ix, SCY, iw, SET_ROW_H, bg);
                uint64_t max_ch = fw > 0u ? (uint64_t)(ix + iw - cx) / fw : 40u;
                gui_draw_str_clip(cx, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                                  "Add /fifi-data/wifi.conf: SSID= and PASSWORD=",
                                  COL_SET_HINT, bg, max_ch);
            }
            cy += SET_ROW_H;
        }

        if (SVIS) console_fill_rect(ix, SCY, iw, 1u, COL_SET_SEP);
        cy += 5u;
    }
    SADVBOT;

    /* ── Section: Privacy and Lock ── */
    {
        if (SVIS) {
            console_fill_rect(ix, SCY, iw, SET_SEC_H, COL_SET_SEC_BG);
            gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_SEC_H - fh) / 2u)),
                         "Privacy and Lock", COL_SET_SEC_FG, COL_SET_SEC_BG);
        }
        cy += SET_SEC_H + 4u;

        /* Lock timeout row */
        {
            static const char *lto_labels[] = { "Never", "1 min", "5 min", "10 min", "30 min" };
            SADVBOT;
            if (SVIS) {
                uint32_t bg = COL_SET_BG;
                console_fill_rect(ix, SCY, iw, SET_ROW_H, bg);
                gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                             "Lock Timeout:", COL_SET_KEY_FG, bg);
                const char *lbl = lto_labels[g_lto_idx];
                uint64_t lbw = (uint64_t)(gui_strlen(lbl) + 2u) * fw;
                uint64_t lbx = val_x;
                uint32_t lb_bg = (g_lto_idx == 0) ? 0x00203040u : 0x00102030u;
                uint32_t lb_fg = (g_lto_idx == 0) ? 0x0080a8d0u : 0x0060d8f0u;
                console_fill_rect(lbx, SCY, lbw, SET_ROW_H, lb_bg);
                gui_draw_str(lbx + fw, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                             lbl, lb_fg, lb_bg);
                g_lto_btn_bx = lbx; g_lto_btn_by = SCY;
                g_lto_btn_bw = lbw; g_lto_btn_bh = SET_ROW_H;
            }
            cy += SET_ROW_H;
        }
        if (SVIS) console_fill_rect(ix, SCY, iw, 1u, COL_SET_SEP);
        cy += 5u;
    }
    SADVBOT;

    /* ── Section: Keyboard Shortcuts ── */
    {
        if (SVIS) {
            console_fill_rect(ix, SCY, iw, SET_SEC_H, COL_SET_SEC_BG);
            gui_draw_str(cx, (uint64_t)(cy + (int64_t)((SET_SEC_H - fh) / 2u)),
                         "Keyboard Shortcuts", COL_SET_SEC_FG, COL_SET_SEC_BG);
        }
        cy += SET_SEC_H + 4u;

        for (int i = 0; shortcuts[i].key; i++) {
            SADVBOT;
            if (SVIS) {
                uint32_t bg = (i & 1) ? 0x000f151fu : COL_SET_BG;
                console_fill_rect(ix, SCY, iw, SET_ROW_H, bg);
                gui_draw_str(cx,    (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                             shortcuts[i].key, COL_SET_KEY_FG, bg);
                gui_draw_str(val_x, (uint64_t)(cy + (int64_t)((SET_ROW_H - fh) / 2u)),
                             shortcuts[i].desc, COL_SET_VAL_FG, bg);
            }
            cy += SET_ROW_H;
        }
    }

settings_done: ;
    /* Clip any content that overflowed below the window content area */
    console_fill_rect(w->x, w->y + w->h - BORDER, w->w, BORDER, COL_BORDER);

    /* ── Full-panel scrollbar ── */
    if ((int64_t)total_h > (int64_t)ih) {
        uint64_t sb_x = ix + iw - 6u;
        console_fill_rect(sb_x, iy, 6u, ih, 0x00101820u);
        uint64_t thumb_h = ih * ih / total_h;
        if (thumb_h < 12u) thumb_h = 12u;
        if (thumb_h > ih) thumb_h = ih;
        uint64_t thumb_y = iy + (uint64_t)((int64_t)ih * (int64_t)g_settings_scroll / (int64_t)total_h);
        if (thumb_y + thumb_h > iy + ih) thumb_y = iy + ih - thumb_h;
        console_fill_rect(sb_x + 1u, thumb_y, 4u, thumb_h, 0x00304878u);
    }

    /* ── Hint at bottom: only when there is blank space below content ── */
    {
        uint64_t hint_y = iy + ih - fh - 8u;
        if ((int64_t)hint_y > cy + 4) {
            console_fill_rect(ix, hint_y - 4u,
                              (int64_t)total_h > (int64_t)ih ? iw - 8u : iw,
                              1u, COL_SET_SEP);
            gui_draw_str(cx, hint_y, "Press Esc or Ctrl+W to close", COL_SET_HINT, COL_SET_BG);
        }
    }
}
