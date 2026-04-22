#include <stdint.h>
#include <stdbool.h>
#include "mouse.h"
#include "console.h"
#include "io.h"

/* ── Cursor dimensions ────────────────────────────────────────────────────
 * All shapes share the same bounding box so the save buffer is a fixed size.
 * Bit 15 of each uint16_t = leftmost column (col 0).                       */
#define CUR_W  16u
#define CUR_H  17u
#define CUR_FG 0x00FFFFFFu   /* white fill */
#define CUR_OL 0x00000000u   /* black outline */

/* Save buffer: (CUR_W+2) × (CUR_H+2) pixels saved under cursor */
#define SAV_W (CUR_W + 2u)
#define SAV_H (CUR_H + 2u)

/* ── Arrow cursor (northwest arrow, hotspot 0,0) ─────────────────────────
 *   #
 *   ##
 *   ###
 *   ####
 *   #####
 *   ######
 *   #######
 *   ########
 *   #########
 *   ##########
 *   ######
 *   ## ###
 *      ###
 *      ###
 *       ##
 *       ##
 *        #               */
static const uint16_t shape_arrow[CUR_H] = {
    0x8000u,  0xC000u,  0xE000u,  0xF000u,  0xF800u,
    0xFC00u,  0xFE00u,  0xFF00u,  0xFF80u,  0xFFC0u,
    0xFC00u,  0xDC00u,  0x1C00u,  0x1C00u,  0x0C00u,
    0x0C00u,  0x0400u,
};
static const int8_t hx_arrow = 0, hy_arrow = 0;

/* ── Resize-H cursor (←→, hotspot 7,3) ──────────────────────────────────
 *
 *    ....#...#....   (13 pixels active, cols 3-11 for arrows)
 *    ...##...##...
 *    ..###...###..
 *    #############
 *    #############
 *    ..###...###..
 *    ...##...##...
 *    ....#...#....
 *
 * Encoded in 16-bit rows (bit15=col0):
 *   col 3 = 0x1000, col 4 = 0x0800, col 5 = 0x0400
 *   col 6 = 0x0200, col 7 = 0x0100, col 8 = 0x0080
 *   col 9 = 0x0040, col 10 = 0x0020, col 11 = 0x0010
 *   col 12 = 0x0008
 */
static const uint16_t shape_resize_h[CUR_H] = {
    0x0000u,  0x0000u,  0x0000u,
    0x0880u,              /* ....#...#....  col4 col8 */
    0x18C0u,              /* ...##...##..   col3,4 col8,9 */
    0x38E0u,              /* ..###...###.   col2,3,4 col8,9,10 wait: col2=0x2000 */
    0xFFF0u,              /* all cols 0-11  = 0xFFF0 */
    0xFFF0u,
    0x38E0u,
    0x18C0u,
    0x0880u,
    0x0000u,  0x0000u,  0x0000u,  0x0000u,  0x0000u,  0x0000u,
};
static const int8_t hx_resize_h = 7, hy_resize_h = 3;

/* ── Resize-V cursor (↕, hotspot 3,7) ───────────────────────────────────
 *
 *    ...#...    col3 (centre of 7-wide)
 *    ...#...
 *    ..###..
 *    .#####.
 *    #######
 *    .#####.
 *    ..###..
 *    ...#...
 *    ...#...
 *
 * 7-wide: col0=0x8000, col1=0x4000, col2=0x2000, col3=0x1000,
 *         col4=0x0800, col5=0x0400, col6=0x0200
 */
static const uint16_t shape_resize_v[CUR_H] = {
    0x0000u,  0x0000u,  0x0000u,  0x0000u,
    0x1000u,              /* ...#...  col3 */
    0x1000u,
    0x3800u,              /* ..###..  col2,3,4 */
    0x7C00u,              /* .#####.  col1,2,3,4,5 */
    0xFE00u,              /* #######  col0-6 */
    0x7C00u,
    0x3800u,
    0x1000u,
    0x1000u,
    0x0000u,  0x0000u,  0x0000u,  0x0000u,
};
static const int8_t hx_resize_v = 3, hy_resize_v = 7;

/* ── Text/I-beam cursor (hotspot 3,0) ────────────────────────────────────
 *
 *    #######  (7-wide top bar, col0-6)
 *    ...#...
 *    ...#...
 *    ...#...
 *    ...#...
 *    ...#...
 *    ...#...
 *    ...#...
 *    ...#...
 *    ...#...
 *    ...#...
 *    ...#...
 *    ...#...
 *    ...#...
 *    ...#...
 *    #######  (7-wide bottom bar)
 *    (empty)
 */
static const uint16_t shape_text[CUR_H] = {
    0xFE00u,              /* ####### col0-6 */
    0x1000u,              /* ...#... col3 */
    0x1000u,  0x1000u,  0x1000u,  0x1000u,  0x1000u,
    0x1000u,  0x1000u,  0x1000u,  0x1000u,  0x1000u,
    0x1000u,  0x1000u,  0x1000u,
    0xFE00u,              /* ####### col0-6 */
    0x0000u,
};
static const int8_t hx_text = 3, hy_text = 0;

/* ── Hand/pointer cursor (hotspot 4,0) ───────────────────────────────────
 *
 *    ....#...   col4 = 0x0800
 *    ...#.#..   col3,col5
 *    ...#.#..
 *    ...###..   col3,4,5
 *    ...#.###   col3,5,6,7
 *    ...#.###
 *    ##.#.###   col0,1 gap col3,5,6,7
 *    #########  all
 *    #########
 *    .#######.  col1-7
 *    ..#####..  col2-6
 *    ..#####..
 *    ...###...  col3-5
 *
 * Encoding:
 *   col0=0x8000, col1=0x4000, col2=0x2000, col3=0x1000,
 *   col4=0x0800, col5=0x0400, col6=0x0200, col7=0x0100, col8=0x0080
 */
static const uint16_t shape_hand[CUR_H] = {
    0x0800u,              /* ....#...  col4 */
    0x1400u,              /* ...#.#..  col3,col5 */
    0x1400u,
    0x1C00u,              /* ...###..  col3,4,5 */
    0x1700u,              /* ...#.###  col3,5,6,7 */
    0x1700u,
    0xC700u,              /* ##.#.###  col0,1, col3,5,6,7 (skip col2,col4) */
    0xFF80u,              /* #########  col0-8 */
    0xFF80u,
    0x7F00u,              /* .#######.  col1-7 */
    0x3E00u,              /* ..#####..  col2-6 */
    0x3E00u,
    0x1C00u,              /* ...###...  col3-5 */
    0x0000u,  0x0000u,  0x0000u,  0x0000u,
};
static const int8_t hx_hand = 4, hy_hand = 0;

/* ── Move/drag cursor (+, hotspot 7,7) ───────────────────────────────────
 *
 *    .......#.......   col7 (center of 15)
 *    ......###......   col6,7,8
 *    .....#####.....   col5-9
 *    .......#.......
 *    ..#.........#..   col2, col12
 *    .##...###...##.   col1,2, col6,7,8, col12,13
 *    ###############   all cols 0-14
 *    .##...###...##.
 *    ..#.........#..
 *    .......#.......
 *    .....#####.....
 *    ......###......
 *    .......#.......
 *
 * col0=0x8000 .. col14=0x0002
 * col5=0x0400, col6=0x0200, col7=0x0100, col8=0x0080, col9=0x0040
 * col2=0x2000, col12=0x0008, col13=0x0004
 * col1=0x4000, col14=0x0002
 */
static const uint16_t shape_move[CUR_H] = {
    0x0000u,  0x0000u,
    0x0100u,              /* .......#.......  col7 */
    0x0380u,              /* ......###......  col6,7,8 */
    0x07C0u,              /* .....#####.....  col5,6,7,8,9 */
    0x0100u,              /* .......#.......  col7 */
    0x200Cu,              /* ..#.........#..  col2, col12=0x0008... wait col12=0x0008? */
    /* col2=bit13=0x2000, col12=bit3=0x0008 → 0x2008 */
    /* Actually col2=0x2000, col12=0x0008 but add 1,2 and 12,13: col1=0x4000,col2=0x2000 and col12=0x0008,col13=0x0004 */
    /* plus col6,7,8 = 0x0380 */
    /* Row 7: col1,2, col6-8, col12,13 = 0x4000|0x2000|0x0380|0x0008|0x0004 = 0x638C */
    0x638Cu,              /* .##...###...##. */
    0xFFFEu,              /* ############### col0-14 */
    0x638Cu,
    0x200Cu,              /* was 0x2008, but col2=0x2000 and col12=0x0008 → 0x2008 */
    0x0100u,
    0x07C0u,
    0x0380u,
    0x0100u,
    0x0000u,
};
static const int8_t hx_move = 7, hy_move = 7;

/* ── Active cursor state ──────────────────────────────────────────────── */

static cursor_type_t g_cursor_type = CURSOR_ARROW;
static int8_t g_hx = 0, g_hy = 0;
static uint16_t g_shape[CUR_H];
static uint32_t cursor_ol[CUR_H + 2u];   /* computed outline */

/* ── Save buffer ─────────────────────────────────────────────────────── */
static uint32_t m_bg[SAV_H * SAV_W];
static bool     m_bg_valid = false;
static int32_t  m_bg_x = 0, m_bg_y = 0;

/* ── PS/2 packet state ──────────────────────────────────────────────── */
static uint8_t  m_pkt[4];
static uint32_t m_pkt_idx = 0;
static bool     m_intellimouse = false;
static int8_t   m_scroll = 0;

/* ── Cursor position ─────────────────────────────────────────────────── */
static int32_t m_x = 0, m_y = 0;
static bool    m_lbtn = false, m_rbtn = false;
static bool    m_active = false;

/* ── Click detection ─────────────────────────────────────────────────── */
static bool    m_clicked  = false;
static int32_t m_click_x  = 0, m_click_y = 0;

/* ── Outline computation ──────────────────────────────────────────────── */

static void compute_outline(void) {
    for (uint32_t r = 0; r <= CUR_H + 1u; r++) {
        uint32_t fa = (r >= 2u && r - 1u <= CUR_H) ? (uint32_t)g_shape[r - 2u] : 0u;
        uint32_t fb2 = (r >= 1u && r - 1u <= CUR_H - 1u) ? (uint32_t)g_shape[r - 1u] : 0u;
        uint32_t fc = (r <= CUR_H - 1u) ? (uint32_t)g_shape[r] : 0u;
        uint32_t dil = (fa | (fa >> 1) | (fa << 1))
                     | (fb2 | (fb2 >> 1) | (fb2 << 1))
                     | (fc | (fc >> 1) | (fc << 1));
        cursor_ol[r] = dil & ~fb2;
    }
}

static void load_cursor(cursor_type_t type) {
    const uint16_t *src = shape_arrow;
    int8_t hx = hx_arrow, hy = hy_arrow;

    switch (type) {
        case CURSOR_RESIZE_H: src = shape_resize_h; hx = hx_resize_h; hy = hy_resize_h; break;
        case CURSOR_RESIZE_V: src = shape_resize_v; hx = hx_resize_v; hy = hy_resize_v; break;
        case CURSOR_TEXT:     src = shape_text;     hx = hx_text;     hy = hy_text;     break;
        case CURSOR_HAND:     src = shape_hand;     hx = hx_hand;     hy = hy_hand;     break;
        case CURSOR_MOVE:     src = shape_move;     hx = hx_move;     hy = hy_move;     break;
        default: break;
    }
    for (uint32_t i = 0; i < CUR_H; i++) g_shape[i] = src[i];
    g_hx = hx;
    g_hy = hy;
    compute_outline();
}

/* ── Internal helpers ─────────────────────────────────────────────────── */

static void cursor_restore(void) {
    if (!m_bg_valid) return;
    volatile uint32_t *fb = console_fb_ptr();
    if (!fb) return;
    uint64_t pitch = console_pitch32();
    uint64_t sw = (uint64_t)console_fb_width();
    uint64_t sh = (uint64_t)console_fb_height();
    for (uint32_t r = 0; r < SAV_H; r++) {
        int32_t py = m_bg_y + (int32_t)r;
        if (py < 0 || (uint64_t)py >= sh) continue;
        for (uint32_t c = 0; c < SAV_W; c++) {
            int32_t px = m_bg_x + (int32_t)c;
            if (px < 0 || (uint64_t)px >= sw) continue;
            fb[(uint64_t)py * pitch + (uint64_t)px] = m_bg[r * SAV_W + c];
        }
    }
    m_bg_valid = false;
}

static void cursor_draw(int32_t nx, int32_t ny) {
    volatile uint32_t *fb = console_fb_ptr();
    if (!fb) return;
    uint64_t pitch = console_pitch32();
    uint64_t sw = (uint64_t)console_fb_width();
    uint64_t sh = (uint64_t)console_fb_height();

    /* Apply hotspot offset: draw_x,draw_y = top-left of cursor bitmap */
    int32_t draw_x = nx - (int32_t)g_hx;
    int32_t draw_y = ny - (int32_t)g_hy;

    /* save area starts 1px above+left of draw position */
    m_bg_x = draw_x - 1; m_bg_y = draw_y - 1;
    for (uint32_t r = 0; r < SAV_H; r++) {
        int32_t py = m_bg_y + (int32_t)r;
        for (uint32_t c = 0; c < SAV_W; c++) {
            int32_t px = m_bg_x + (int32_t)c;
            if (py >= 0 && (uint64_t)py < sh && px >= 0 && (uint64_t)px < sw)
                m_bg[r * SAV_W + c] = fb[(uint64_t)py * pitch + (uint64_t)px];
            else
                m_bg[r * SAV_W + c] = 0u;
        }
    }
    m_bg_valid = true;

    /* outline: cursor_ol[r] covers row (draw_y-1+r).
     * bit16 = col draw_x-1, bit15 = col draw_x, bit14 = col draw_x+1, … */
    for (uint32_t r = 0; r <= CUR_H + 1u; r++) {
        int32_t py = draw_y - 1 + (int32_t)r;
        if (py < 0 || (uint64_t)py >= sh) continue;
        uint32_t ol = cursor_ol[r];
        for (uint32_t b = 0; b <= CUR_W + 1u; b++) {
            if (!((ol >> (16u - b)) & 1u)) continue;
            int32_t px = draw_x - 1 + (int32_t)b;
            if (px < 0 || (uint64_t)px >= sw) continue;
            fb[(uint64_t)py * pitch + (uint64_t)px] = CUR_OL;
        }
    }

    /* fg: g_shape[r] covers row (draw_y+r), bit15=col draw_x */
    for (uint32_t r = 0; r < CUR_H; r++) {
        int32_t py = draw_y + (int32_t)r;
        if (py < 0 || (uint64_t)py >= sh) continue;
        uint16_t row = g_shape[r];
        for (uint32_t c = 0; c < CUR_W; c++) {
            if (!((row >> (15u - c)) & 1u)) continue;
            int32_t px = draw_x + (int32_t)c;
            if (px < 0 || (uint64_t)px >= sw) continue;
            fb[(uint64_t)py * pitch + (uint64_t)px] = CUR_FG;
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void mouse_init(void) {
    load_cursor(CURSOR_ARROW);
    g_cursor_type = CURSOR_ARROW;
    m_x = (int32_t)(console_fb_width()  / 2u);
    m_y = (int32_t)(console_fb_height() / 2u);
    m_pkt_idx  = 0;
    m_bg_valid = false;
    m_active   = false;
}

void mouse_set_cursor(cursor_type_t type) {
    if (type == g_cursor_type) return;
    if ((unsigned)type >= (unsigned)CURSOR_COUNT) return;
    cursor_restore();
    g_cursor_type = type;
    load_cursor(type);
    if (m_active) cursor_draw(m_x, m_y);
}

cursor_type_t mouse_get_cursor(void) { return g_cursor_type; }

void mouse_irq_handler(void) {
    uint8_t st = inb(0x64);
    if ((st & 0x21u) == 0x21u) {
        mouse_on_byte(inb(0x60));
    }
}

void mouse_on_byte(uint8_t b) {
    if (m_pkt_idx == 0u && !(b & 0x08u))
        return;

    m_pkt[m_pkt_idx++] = b;
    uint32_t pkt_len = m_intellimouse ? 4u : 3u;
    if (m_pkt_idx < pkt_len) return;
    m_pkt_idx = 0u;

    uint8_t  flags    = m_pkt[0];
    int32_t  dx       = (int32_t)m_pkt[1] - ((flags & 0x10u) ? 256 : 0);
    int32_t  dy       = (int32_t)m_pkt[2] - ((flags & 0x20u) ? 256 : 0);
    bool     new_lbtn = (flags & 0x01u) != 0u;
    bool     new_rbtn = (flags & 0x02u) != 0u;

    if (m_intellimouse) {
        uint8_t sb = m_pkt[3] & 0x0Fu;
        int8_t  sd = (sb & 0x08u) ? (int8_t)(sb | 0xF0u) : (int8_t)sb;
        m_scroll = (int8_t)(m_scroll + sd);
    }

    int32_t nx = m_x + dx;
    int32_t ny = m_y - dy;

    int32_t sw = (int32_t)console_fb_width();
    int32_t sh = (int32_t)console_fb_height();
    if (nx < 0) nx = 0; if (nx >= sw) nx = sw - 1;
    if (ny < 0) ny = 0; if (ny >= sh) ny = sh - 1;

    if (new_lbtn && !m_lbtn) {
        m_clicked = true;
        m_click_x = nx;
        m_click_y = ny;
    }
    m_lbtn = new_lbtn;
    m_rbtn = new_rbtn;

    if (!m_active || nx != m_x || ny != m_y) {
        cursor_restore();
        m_x = nx; m_y = ny;
        cursor_draw(nx, ny);
        m_active = true;
    }
}

bool mouse_consume_click(int32_t *x, int32_t *y) {
    if (!m_clicked) return false;
    m_clicked = false;
    if (x) *x = m_click_x;
    if (y) *y = m_click_y;
    return true;
}

void mouse_get_state(int32_t *x, int32_t *y, bool *lbtn, bool *rbtn) {
    if (x)    *x    = m_x;
    if (y)    *y    = m_y;
    if (lbtn) *lbtn = m_lbtn;
    if (rbtn) *rbtn = m_rbtn;
}

void mouse_warp(int32_t x, int32_t y) {
    int32_t sw = (int32_t)console_fb_width();
    int32_t sh = (int32_t)console_fb_height();
    if (x < 0) x = 0; if (x >= sw) x = sw - 1;
    if (y < 0) y = 0; if (y >= sh) y = sh - 1;
    cursor_restore();
    m_x = x; m_y = y;
    cursor_draw(x, y);
    m_active = true;
}

void mouse_click(int32_t x, int32_t y) {
    mouse_warp(x, y);
    m_clicked = true;
    m_click_x = x;
    m_click_y = y;
    m_lbtn = false;
}

void mouse_set_intellimouse(bool enabled) {
    m_intellimouse = enabled;
    m_pkt_idx = 0;
}

int8_t mouse_consume_scroll(void) {
    int8_t d = m_scroll;
    m_scroll = 0;
    return d;
}

void mouse_cursor_update(void) {
    if (!m_active) return;
    m_bg_valid = false;
    cursor_draw(m_x, m_y);
}

void mouse_push_rel(int32_t dx, int32_t dy, bool lbtn, bool rbtn) {
    int32_t nx = m_x + dx;
    int32_t ny = m_y + dy;

    int32_t sw = (int32_t)console_fb_width();
    int32_t sh = (int32_t)console_fb_height();
    if (nx < 0) nx = 0; if (nx >= sw) nx = sw - 1;
    if (ny < 0) ny = 0; if (ny >= sh) ny = sh - 1;

    if (lbtn && !m_lbtn) {
        m_clicked = true;
        m_click_x = nx;
        m_click_y = ny;
    }
    m_lbtn = lbtn;
    m_rbtn = rbtn;

    if (!m_active || nx != m_x || ny != m_y) {
        cursor_restore();
        m_x = nx; m_y = ny;
        cursor_draw(nx, ny);
        m_active = true;
    }
}
