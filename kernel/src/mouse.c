#include <stdint.h>
#include <stdbool.h>
#include "mouse.h"
#include "console.h"

/* ── Cursor bitmap (8×8 filled arrow, hotspot at top-left) ──────────────────
 * MSB = leftmost pixel.  Tapering triangle pointing up-left.
 *   ########
 *   #######.
 *   ######..
 *   #####...
 *   ####....
 *   ###.....
 *   ##......
 *   #.......  */
#define CUR_W  8u
#define CUR_H  8u
#define CUR_FG 0x00FFFFFFu   /* white cursor */

static const uint8_t cursor_bits[CUR_H] = {
    0xFF, 0xFE, 0xFC, 0xF8,
    0xF0, 0xE0, 0xC0, 0x80,
};

/* ── Framebuffer cursor state ────────────────────────────────────────────── */
static uint32_t m_bg[CUR_H][CUR_W];   /* pixels saved under cursor */
static bool     m_bg_valid = false;
static int32_t  m_bg_x = 0, m_bg_y = 0;

/* ── Mouse packet state ──────────────────────────────────────────────────── */
static uint8_t  m_pkt[3];
static uint32_t m_pkt_idx = 0;

/* ── Cursor position and button state ────────────────────────────────────── */
static int32_t m_x = 0, m_y = 0;
static bool    m_lbtn = false, m_rbtn = false;
static bool    m_active = false;   /* true once first packet arrives */

/* ── Internal cursor draw/restore ────────────────────────────────────────── */

static void cursor_restore(void) {
    if (!m_bg_valid) return;
    volatile uint32_t *fb = console_fb_ptr();
    if (!fb) return;
    uint64_t pitch = console_pitch32();
    uint64_t sw = console_fb_width(), sh = console_fb_height();
    for (uint32_t r = 0; r < CUR_H; r++) {
        int32_t py = m_bg_y + (int32_t)r;
        if (py < 0 || (uint64_t)py >= sh) continue;
        for (uint32_t c = 0; c < CUR_W; c++) {
            int32_t px = m_bg_x + (int32_t)c;
            if (px < 0 || (uint64_t)px >= sw) continue;
            fb[(uint64_t)py * pitch + (uint64_t)px] = m_bg[r][c];
        }
    }
    m_bg_valid = false;
}

static void cursor_draw(int32_t nx, int32_t ny) {
    volatile uint32_t *fb = console_fb_ptr();
    if (!fb) return;
    uint64_t pitch = console_pitch32();
    uint64_t sw = console_fb_width(), sh = console_fb_height();

    m_bg_x = nx; m_bg_y = ny;

    for (uint32_t r = 0; r < CUR_H; r++) {
        int32_t py = ny + (int32_t)r;
        uint8_t bits = cursor_bits[r];
        for (uint32_t c = 0; c < CUR_W; c++) {
            int32_t px = nx + (int32_t)c;
            if (py < 0 || (uint64_t)py >= sh || px < 0 || (uint64_t)px >= sw) {
                m_bg[r][c] = 0;
                continue;
            }
            uint64_t idx = (uint64_t)py * pitch + (uint64_t)px;
            m_bg[r][c] = fb[idx];
            if ((bits >> (7u - c)) & 1u)
                fb[idx] = CUR_FG;
        }
    }
    m_bg_valid = true;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void mouse_init(void) {
    m_x = (int32_t)(console_fb_width()  / 2u);
    m_y = (int32_t)(console_fb_height() / 2u);
    m_pkt_idx  = 0;
    m_bg_valid = false;
    m_active   = false;
}

void mouse_on_byte(uint8_t b) {
    if (m_pkt_idx == 0 && !(b & 0x08u))
        return;  /* first byte must have the always-1 bit */

    m_pkt[m_pkt_idx++] = b;
    if (m_pkt_idx < 3) return;
    m_pkt_idx = 0;

    uint8_t  flags = m_pkt[0];
    int32_t  dx    = (int32_t)m_pkt[1] - ((flags & 0x10u) ? 256 : 0);
    int32_t  dy    = (int32_t)m_pkt[2] - ((flags & 0x20u) ? 256 : 0);

    m_lbtn = (flags & 0x01u) != 0;
    m_rbtn = (flags & 0x02u) != 0;

    /* PS/2 Y axis: positive = up on physical mouse → invert for screen */
    int32_t nx = m_x + dx;
    int32_t ny = m_y - dy;

    int32_t sw = (int32_t)console_fb_width();
    int32_t sh = (int32_t)console_fb_height();
    if (nx < 0) nx = 0;
    if (nx >= sw) nx = sw - 1;
    if (ny < 0) ny = 0;
    if (ny >= sh) ny = sh - 1;

    if (!m_active || nx != m_x || ny != m_y) {
        cursor_restore();
        m_x = nx; m_y = ny;
        cursor_draw(nx, ny);
        m_active = true;
    }
}

void mouse_get_state(int32_t *x, int32_t *y, bool *lbtn, bool *rbtn) {
    if (x)    *x    = m_x;
    if (y)    *y    = m_y;
    if (lbtn) *lbtn = m_lbtn;
    if (rbtn) *rbtn = m_rbtn;
}
