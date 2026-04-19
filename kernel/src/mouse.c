#include <stdint.h>
#include <stdbool.h>
#include "mouse.h"
#include "console.h"
#include "io.h"

/* ── Cursor shape ────────────────────────────────────────────────────────────
 * 10×17 northwest arrow, hotspot at (0,0).
 * Each uint16_t: bit 15 = col 0 (leftmost), bit 6 = col 9.
 *
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
#define CUR_W  10u
#define CUR_H  17u
#define CUR_FG 0x00FFFFFFu   /* white fill */
#define CUR_OL 0x00000000u   /* black outline */

static const uint16_t cursor_fg[CUR_H] = {
    0x8000u,               /*  #          */
    0xC000u,               /*  ##         */
    0xE000u,               /*  ###        */
    0xF000u,               /*  ####       */
    0xF800u,               /*  #####      */
    0xFC00u,               /*  ######     */
    0xFE00u,               /*  #######    */
    0xFF00u,               /*  ########   */
    0xFF80u,               /*  #########  */
    0xFFC0u,               /*  ########## */
    0xFC00u,               /*  ######     */
    0xDC00u,               /*  ## ###     */
    0x1C00u,               /*     ###     */
    0x1C00u,               /*     ###     */
    0x0C00u,               /*      ##     */
    0x0C00u,               /*      ##     */
    0x0400u,               /*       #     */
};

/* outline[0..CUR_H+1]: row index 0 = one pixel above cursor top.
 * Uses bit 16 = col(cursor_x - 1), bit 15 = col(cursor_x), bit 14 = col+1, …
 * Computed at runtime in mouse_init() by dilating cursor_fg by 1 px. */
static uint32_t cursor_ol[CUR_H + 2u];

/* ── Save buffer: (CUR_W+2) × (CUR_H+2) pixels saved under cursor ────────── */
#define SAV_W (CUR_W + 2u)
#define SAV_H (CUR_H + 2u)
static uint32_t m_bg[SAV_H * SAV_W];
static bool     m_bg_valid = false;
static int32_t  m_bg_x = 0, m_bg_y = 0;   /* top-left of save area */

/* ── PS/2 packet state ──────────────────────────────────────────────────── */
static uint8_t  m_pkt[3];
static uint32_t m_pkt_idx = 0;

/* ── Cursor position ─────────────────────────────────────────────────────── */
static int32_t m_x = 0, m_y = 0;
static bool    m_lbtn = false, m_rbtn = false;
static bool    m_active = false;

/* ── Internal helpers ────────────────────────────────────────────────────── */

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

    /* save area starts 1px above+left of hotspot */
    m_bg_x = nx - 1; m_bg_y = ny - 1;
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

    /* outline: cursor_ol[r] covers row (ny-1+r).
     * bit16 = col nx-1, bit15 = col nx, bit14 = col nx+1, … */
    for (uint32_t r = 0; r <= CUR_H + 1u; r++) {
        int32_t py = ny - 1 + (int32_t)r;
        if (py < 0 || (uint64_t)py >= sh) continue;
        uint32_t ol = cursor_ol[r];
        for (uint32_t b = 0; b <= CUR_W + 1u; b++) {
            if (!((ol >> (16u - b)) & 1u)) continue;
            int32_t px = nx - 1 + (int32_t)b;
            if (px < 0 || (uint64_t)px >= sw) continue;
            fb[(uint64_t)py * pitch + (uint64_t)px] = CUR_OL;
        }
    }

    /* fg: cursor_fg[r] covers row (ny+r), bit15=col nx */
    for (uint32_t r = 0; r < CUR_H; r++) {
        int32_t py = ny + (int32_t)r;
        if (py < 0 || (uint64_t)py >= sh) continue;
        uint16_t row = cursor_fg[r];
        for (uint32_t c = 0; c < CUR_W; c++) {
            if (!((row >> (15u - c)) & 1u)) continue;
            int32_t px = nx + (int32_t)c;
            if (px < 0 || (uint64_t)px >= sw) continue;
            fb[(uint64_t)py * pitch + (uint64_t)px] = CUR_FG;
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void mouse_init(void) {
    /* Compute outline by dilating cursor_fg 1px in all 8 directions */
    for (uint32_t r = 0; r <= CUR_H + 1u; r++) {
        /* fg at rows r-1, r, r+1 relative to fg array (index 1..CUR_H) */
        uint32_t fa = (r >= 2u && r - 1u <= CUR_H) ? (uint32_t)cursor_fg[r - 2u] : 0u;
        uint32_t fb = (r >= 1u && r - 1u <= CUR_H - 1u) ? (uint32_t)cursor_fg[r - 1u] : 0u;
        uint32_t fc = (r <= CUR_H - 1u) ? (uint32_t)cursor_fg[r] : 0u;
        /* horizontal dilation of each adjacent row */
        uint32_t dil = (fa | (fa >> 1) | (fa << 1))
                     | (fb | (fb >> 1) | (fb << 1))
                     | (fc | (fc >> 1) | (fc << 1));
        cursor_ol[r] = dil & ~fb;   /* outline = dilated minus fg at this row */
    }

    m_x = (int32_t)(console_fb_width()  / 2u);
    m_y = (int32_t)(console_fb_height() / 2u);
    m_pkt_idx  = 0;
    m_bg_valid = false;
    m_active   = false;
}

void mouse_irq_handler(void) {
    /* Read byte only if OBF is set and it's an AUX byte */
    uint8_t st = inb(0x64);
    if ((st & 0x21u) == 0x21u) {
        mouse_on_byte(inb(0x60));
    }
}

void mouse_on_byte(uint8_t b) {
    if (m_pkt_idx == 0u && !(b & 0x08u))
        return;  /* first byte must have the always-1 bit */

    m_pkt[m_pkt_idx++] = b;
    if (m_pkt_idx < 3u) return;
    m_pkt_idx = 0u;

    uint8_t  flags = m_pkt[0];
    int32_t  dx    = (int32_t)m_pkt[1] - ((flags & 0x10u) ? 256 : 0);
    int32_t  dy    = (int32_t)m_pkt[2] - ((flags & 0x20u) ? 256 : 0);

    m_lbtn = (flags & 0x01u) != 0u;
    m_rbtn = (flags & 0x02u) != 0u;

    /* PS/2 Y is inverted: positive delta = upward on screen */
    int32_t nx = m_x + dx;
    int32_t ny = m_y - dy;

    int32_t sw = (int32_t)console_fb_width();
    int32_t sh = (int32_t)console_fb_height();
    if (nx < 0) nx = 0; if (nx >= sw) nx = sw - 1;
    if (ny < 0) ny = 0; if (ny >= sh) ny = sh - 1;

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

void mouse_push_rel(int32_t dx, int32_t dy, bool lbtn, bool rbtn) {
    m_lbtn = lbtn;
    m_rbtn = rbtn;

    int32_t nx = m_x + dx;
    int32_t ny = m_y + dy;

    int32_t sw = (int32_t)console_fb_width();
    int32_t sh = (int32_t)console_fb_height();
    if (nx < 0) nx = 0; if (nx >= sw) nx = sw - 1;
    if (ny < 0) ny = 0; if (ny >= sh) ny = sh - 1;

    if (!m_active || nx != m_x || ny != m_y) {
        cursor_restore();
        m_x = nx; m_y = ny;
        cursor_draw(nx, ny);
        m_active = true;
    }
}
