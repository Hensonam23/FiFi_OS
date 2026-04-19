/* i2c_hid.c — Intel LPSS DesignWare I2C + I2C-HID touchpad driver
 *
 * Probes I2C0 (first LPSS I2C controller) for known touchpad addresses,
 * reads the I2C-HID report descriptor to find X/Y fields, then polls
 * for input reports and feeds relative motion to mouse_push_rel().
 */

#include <stdint.h>
#include <stdbool.h>
#include "i2c_hid.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "kprintf.h"
#include "mouse.h"
#include "console.h"
#include "pit.h"

/* ── DesignWare I2C register offsets ───────────────────────────────── */
#define DW_CON          0x00u
#define DW_TAR          0x04u
#define DW_DATA_CMD     0x10u
#define DW_SS_HCNT      0x14u
#define DW_SS_LCNT      0x18u
#define DW_FS_HCNT      0x1Cu
#define DW_FS_LCNT      0x20u
#define DW_INTR_MASK    0x30u
#define DW_RAW_INTR     0x34u
#define DW_CLR_INTR     0x40u
#define DW_CLR_TX_ABRT  0x54u
#define DW_ENABLE       0x6Cu
#define DW_STATUS       0x70u
#define DW_TXFLR        0x74u
#define DW_RXFLR        0x78u
#define DW_TX_ABRT_SRC  0x80u
#define DW_COMP_TYPE    0xFCu

/* DW_STATUS bits */
#define ST_ACTIVITY  (1u << 0)
#define ST_TFE       (1u << 2)   /* TX FIFO empty */
#define ST_RFNE      (1u << 3)   /* RX FIFO not empty */
#define ST_MST_ACT   (1u << 5)   /* master FSM active */

/* DW_RAW_INTR bit for TX abort (NACK etc.) */
#define INTR_TX_ABRT (1u << 6)

/* DW_DATA_CMD command bits */
#define CMD_READ     0x100u
#define CMD_STOP     0x200u
#define CMD_RESTART  0x400u

/* TX FIFO depth (DesignWare default = 16) */
#define TX_DEPTH 16

/* ── Driver state ───────────────────────────────────────────────────── */
static volatile uint32_t *g_dw  = NULL;
static uint64_t g_gnvs_ic00 = 0;  /* IC00 read from GNVS at init time */
static uint32_t g_init_comp_type = 0;  /* COMP_TYPE read during init */

static bool    g_active         = false;
static uint8_t g_addr;
static uint16_t g_input_reg;
static int     g_poll_rlen;      /* capped report read length */
static uint8_t g_touch_rep_id;

/* X field */
static int32_t g_x_min, g_x_max;
static int     g_x_off, g_x_sz;
static bool    g_x_signed;

/* Y field */
static int32_t g_y_min, g_y_max;
static int     g_y_off, g_y_sz;
static bool    g_y_signed;

/* Tip switch (bit offset in report data, or -1 if none) */
static int     g_tip_off;
static int     g_max_input;

/* Last known position */
static int32_t g_last_x, g_last_y;
static bool    g_was_touching;

/* Diagnostic counters */
static uint32_t g_poll_valid;   /* reports with correct rid received */
static uint32_t g_poll_push;    /* mouse_push_rel calls made */
static uint32_t g_poll_fail;    /* dw_xfer failures */

/* ── DesignWare low-level ────────────────────────────────────────────── */

static uint32_t dw_rd(uint32_t off) { return g_dw[off >> 2]; }
static void     dw_wr(uint32_t off, uint32_t v) { g_dw[off >> 2] = v; }

static void dw_disable(void) {
    dw_wr(DW_ENABLE, 0u);
    /* Wait for master FSM to go idle */
    for (int i = 0; i < 100000; i++)
        if (!(dw_rd(DW_STATUS) & (ST_ACTIVITY | ST_MST_ACT))) break;
}

static void dw_clear_errs(void) {
    dw_rd(DW_CLR_INTR);
    dw_rd(DW_CLR_TX_ABRT);
}

/* Reconfigure for new target address (caller does not need to disable first). */
static void dw_setup(uint8_t addr) {
    dw_disable();
    /* Master mode (bit0) | fast speed (bits[2:1]=10b) | restart_en (bit5) | slave_disable (bit6) */
    dw_wr(DW_CON, 0x65u);
    /* Keep BIOS SCL counts if programmed, otherwise set safe 400kHz defaults
     * assuming ~120MHz LPSS input clock:  Thi=0.6µs → HCNT=69, Tlo=1.3µs → LCNT=155 */
    if (!dw_rd(DW_FS_HCNT)) dw_wr(DW_FS_HCNT, 69u);
    if (!dw_rd(DW_FS_LCNT)) dw_wr(DW_FS_LCNT, 155u);
    dw_wr(DW_TAR, addr & 0x7Fu);
    dw_wr(DW_INTR_MASK, 0u);          /* polling mode — all interrupts masked */
    /* Drain stale RX bytes */
    while (dw_rd(DW_RXFLR)) dw_rd(DW_DATA_CMD);
    dw_wr(DW_ENABLE, 1u);
    dw_clear_errs();
}

/* Combined write-then-repeated-start-read.
 * Returns number of bytes received, or -1 on NACK/abort. */
static int dw_xfer(const uint8_t *wb, int wn, uint8_t *rb, int rn) {
    dw_clear_errs();
    int wi = 0;   /* write bytes pushed */
    int qi = 0;   /* read commands pushed */
    int ri = 0;   /* read bytes received */

    while (wi < wn || qi < rn) {
        if (dw_rd(DW_RAW_INTR) & INTR_TX_ABRT) {
            dw_rd(DW_CLR_TX_ABRT);
            return -1;
        }
        int txfree = (int)TX_DEPTH - (int)dw_rd(DW_TXFLR);
        while (txfree > 0 && (wi < wn || qi < rn)) {
            uint32_t cmd;
            if (wi < wn) {
                cmd = (uint32_t)wb[wi++];
                /* no STOP here — keep bus for repeated start */
            } else {
                cmd = CMD_READ;
                if (qi == 0) cmd |= CMD_RESTART;     /* repeated start into read */
                if (qi == rn - 1) cmd |= CMD_STOP;   /* STOP after last byte */
                qi++;
            }
            dw_wr(DW_DATA_CMD, cmd);
            txfree--;
        }
        /* Drain RX while we still have more to push */
        while (ri < qi && dw_rd(DW_RXFLR) > 0)
            rb[ri++] = (uint8_t)(dw_rd(DW_DATA_CMD) & 0xFFu);
    }

    /* Drain remaining RX */
    for (int t = 0; t < 200000 && ri < rn; t++) {
        if (dw_rd(DW_RAW_INTR) & INTR_TX_ABRT) { dw_rd(DW_CLR_TX_ABRT); return -1; }
        while (ri < rn && dw_rd(DW_RXFLR) > 0)
            rb[ri++] = (uint8_t)(dw_rd(DW_DATA_CMD) & 0xFFu);
    }
    return (ri >= rn) ? rn : -1;
}

/* Write-only transaction with STOP at end. Returns wn or -1. */
static int dw_write_only(const uint8_t *wb, int wn) {
    dw_clear_errs();
    for (int i = 0; i < wn; i++) {
        for (int t = 0; t < 100000 && (int)dw_rd(DW_TXFLR) >= TX_DEPTH; t++);
        uint32_t cmd = (uint32_t)wb[i];
        if (i == wn - 1) cmd |= CMD_STOP;
        dw_wr(DW_DATA_CMD, cmd);
    }
    /* Wait for TX FIFO empty and master idle */
    for (int t = 0; t < 200000; t++)
        if ((dw_rd(DW_STATUS) & ST_TFE) && !(dw_rd(DW_STATUS) & ST_MST_ACT)) break;
    if (dw_rd(DW_RAW_INTR) & INTR_TX_ABRT) { dw_rd(DW_CLR_TX_ABRT); return -1; }
    return wn;
}

/* ── I2C-HID descriptor layout ─────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t wHIDDescLength;
    uint16_t bcdVersion;
    uint16_t wReportDescLength;
    uint16_t wReportDescRegister;
    uint16_t wInputRegister;
    uint16_t wMaxInputLength;
    uint16_t wOutputRegister;
    uint16_t wMaxOutputLength;
    uint16_t wCommandRegister;
    uint16_t wDataRegister;
    uint16_t wVendorID;
    uint16_t wProductID;
    uint16_t wVersionID;
    uint32_t reserved;
} hid_desc_t;

/* ── HID report descriptor parser ──────────────────────────────────── */

static int32_t item_s(const uint8_t *d, int sz) {
    uint32_t u = 0;
    for (int i = 0; i < sz; i++) u |= (uint32_t)d[i] << (8 * i);
    if (sz && sz < 4 && (u >> (8 * sz - 1)) & 1) u |= ~((1u << (8 * sz)) - 1u);
    return (int32_t)u;
}
static uint32_t item_u(const uint8_t *d, int sz) {
    uint32_t u = 0;
    for (int i = 0; i < sz; i++) u |= (uint32_t)d[i] << (8 * i);
    return u;
}

static bool parse_report_desc(const uint8_t *d, int len) {
    /* Global state */
    uint16_t up = 0;
    int32_t  lmin = 0, lmax = 0;
    uint32_t rsz = 0, rcnt = 0;
    uint8_t  rid = 0;

    /* Local state (reset after each Input/Feature/Output or Collection) */
    uint16_t usage = 0, umin = 0, umax = 0;
    bool has_urange = false;

    /* Per-report-ID bit offset tracking (max 8 IDs) */
    uint32_t offs[8] = {0};
    uint8_t  rids[8] = {0};
    int      n_rids  = 1;         /* slot 0 = report ID 0 (no explicit ID) */
    uint32_t *offp   = &offs[0];  /* pointer to current report's bit offset */

    /* Results */
    bool found_x = false, found_y = false;
    int32_t fx_min=0, fx_max=0, fy_min=0, fy_max=0;
    int  fx_off=0, fx_sz=0, fy_off=0, fy_sz=0;
    uint8_t xy_rid = 0;
    int  ftip_off = -1;
    bool fx_signed=false, fy_signed=false;

    for (int pos = 0; pos < len; ) {
        uint8_t b = d[pos++];
        if (b == 0xFEu) {               /* long item */
            if (pos + 2 > len) break;
            int lsz = d[pos++]; pos++; pos += lsz;
            continue;
        }
        int sz = b & 3; if (sz == 3) sz = 4;
        if (pos + sz > len) break;
        const uint8_t *dat = d + pos; pos += sz;

        uint8_t tag  = (b >> 4) & 0xFu;
        uint8_t type = (b >> 2) & 0x3u;

        if (type == 1u) {                /* global */
            switch (tag) {
            case 0: up   = (uint16_t)item_u(dat, sz); break;
            case 1: lmin = item_s(dat, sz);            break;
            case 2: lmax = item_s(dat, sz);            break;
            case 7: rsz  = item_u(dat, sz);            break;
            case 8: /* Report ID */
                rid = sz ? dat[0] : 0;
                offp = NULL;
                for (int i = 0; i < n_rids; i++)
                    if (rids[i] == rid) { offp = &offs[i]; break; }
                if (!offp && n_rids < 8) {
                    rids[n_rids] = rid; offs[n_rids] = 0;
                    offp = &offs[n_rids++];
                }
                if (!offp) offp = &offs[0];
                break;
            case 9: rcnt = item_u(dat, sz); break;
            }
        } else if (type == 2u) {         /* local */
            switch (tag) {
            case 0: usage = (uint16_t)item_u(dat, sz); has_urange = false; break;
            case 1: umin  = (uint16_t)item_u(dat, sz); has_urange = true;  break;
            case 2: umax  = (uint16_t)item_u(dat, sz);                     break;
            }
        } else if (type == 0u) {         /* main */
            if (tag == 8u) {             /* Input */
                bool is_var   = sz && (dat[0] & 0x02u);
                bool is_const = sz && (dat[0] & 0x01u);

                if (is_var && !is_const && offp) {
                    uint32_t off = *offp;

                    /* X coordinate */
                    if (!found_x && up == 0x01u) {
                        bool match = (usage == 0x30u) ||
                                     (has_urange && umin <= 0x30u && umax >= 0x30u);
                        if (match) {
                            fx_min = lmin; fx_max = lmax;
                            fx_off = (int)off; fx_sz = (int)rsz;
                            fx_signed = (lmin < 0);
                            xy_rid = rid;
                            found_x = true;
                        }
                    }
                    /* Y coordinate */
                    if (!found_y && up == 0x01u) {
                        bool match = (usage == 0x31u) ||
                                     (has_urange && umin <= 0x31u && umax >= 0x31u);
                        if (match) {
                            fy_min = lmin; fy_max = lmax;
                            fy_off = (int)off; fy_sz = (int)rsz;
                            /* if both X and Y in same usage-range field set,
                             * Y comes immediately after X */
                            if (has_urange && umin <= 0x30u && umax >= 0x31u)
                                fy_off = (int)(off + rsz);
                            fy_signed = (lmin < 0);
                            found_y = true;
                        }
                    }
                    /* Tip switch */
                    if (ftip_off < 0 && up == 0x0Du) {
                        bool match = (usage == 0x42u) ||
                                     (has_urange && umin <= 0x42u && umax >= 0x42u);
                        if (match) ftip_off = (int)off;
                    }
                }

                if (offp) *offp += rsz * rcnt;
                /* reset local state */
                usage = 0; umin = 0; umax = 0; has_urange = false;

            } else if (tag == 10u || tag == 12u) { /* Collection / End Collection */
                usage = 0; umin = 0; umax = 0; has_urange = false;
            }
        }
    }

    if (!found_x || !found_y) return false;

    g_x_min = fx_min; g_x_max = fx_max;
    g_x_off = fx_off; g_x_sz  = fx_sz; g_x_signed = fx_signed;
    g_y_min = fy_min; g_y_max = fy_max;
    g_y_off = fy_off; g_y_sz  = fy_sz; g_y_signed = fy_signed;
    g_touch_rep_id = xy_rid;
    g_tip_off = ftip_off;
    return true;
}

/* ── Bit field extraction ────────────────────────────────────────────── */

static int32_t extract_field(const uint8_t *data, int dlen, int bit_off, int bit_sz, bool is_signed) {
    uint32_t val = 0;
    for (int i = 0; i < bit_sz; i++) {
        int by = (bit_off + i) / 8;
        int bi = (bit_off + i) % 8;
        if (by < dlen && ((data[by] >> bi) & 1u))
            val |= (1u << i);
    }
    if (is_signed && bit_sz > 0 && bit_sz < 32 && (val >> (bit_sz - 1)) & 1u)
        val |= ~((1u << bit_sz) - 1u);
    return (int32_t)val;
}

/* ── Probe one candidate address ─────────────────────────────────────── */

static bool try_init(uint8_t addr, uint16_t desc_reg) {
    dw_setup(addr);

    /* Read 30-byte I2C-HID descriptor */
    uint8_t wreg[2] = { (uint8_t)(desc_reg & 0xFFu), (uint8_t)(desc_reg >> 8) };
    uint8_t dbuf[30];
    int xr = dw_xfer(wreg, 2, dbuf, 30);
    kprintf("i2c_hid: probe 0x%02x dreg=0x%04x xfer=%d [%02x %02x %02x %02x %02x %02x %02x %02x]\n",
            addr, desc_reg, xr,
            dbuf[0],dbuf[1],dbuf[2],dbuf[3],dbuf[4],dbuf[5],dbuf[6],dbuf[7]);
    if (xr < 0) return false;

    hid_desc_t *hd = (hid_desc_t *)dbuf;
    if (hd->wHIDDescLength != 30u || hd->bcdVersion != 0x0100u) return false;
    if (hd->wMaxInputLength < 4u || hd->wMaxInputLength > 512u) return false;

    uint16_t input_reg  = hd->wInputRegister;
    uint16_t max_input  = hd->wMaxInputLength;
    uint16_t rdesc_len  = hd->wReportDescLength;
    uint16_t rdesc_reg  = hd->wReportDescRegister;
    uint16_t cmd_reg    = hd->wCommandRegister;

    if (!rdesc_len || rdesc_len > 2048u) return false;

    kprintf("i2c_hid: addr=0x%02x vid=0x%04x pid=0x%04x rdesc=%u\n",
            addr, hd->wVendorID, hd->wProductID, rdesc_len);

    /* SET_POWER ON (opcode 8) */
    uint8_t pwron[4] = { (uint8_t)(cmd_reg & 0xFFu), (uint8_t)(cmd_reg >> 8), 0x00u, 0x08u };
    dw_write_only(pwron, 4);

    /* Wait ~5ms */
    uint64_t t0 = pit_ticks();
    while (pit_ticks() - t0 < 1u);

    /* RESET (opcode 1) */
    uint8_t resetcmd[4] = { (uint8_t)(cmd_reg & 0xFFu), (uint8_t)(cmd_reg >> 8), 0x00u, 0x01u };
    dw_write_only(resetcmd, 4);

    /* Wait ~50ms for device to reset and assert ATTN */
    t0 = pit_ticks();
    while (pit_ticks() - t0 < 5u);

    /* Drain the reset-response null report from the input register.
     * After RESET the device holds ATTN until the host reads this response;
     * without this read the input register stays zero forever. */
    uint8_t ireg_drain[2] = { (uint8_t)(input_reg & 0xFFu), (uint8_t)(input_reg >> 8) };
    uint8_t reset_resp[4] = {0};
    dw_xfer(ireg_drain, 2, reset_resp, 4);
    kprintf("i2c_hid: reset_resp=[%02x %02x %02x %02x]\n",
            reset_resp[0], reset_resp[1], reset_resp[2], reset_resp[3]);

    /* Wait another ~10ms for device to be ready */
    t0 = pit_ticks();
    while (pit_ticks() - t0 < 1u);

    /* Read report descriptor */
    static uint8_t rdesc[2048];
    uint8_t rdreg[2] = { (uint8_t)(rdesc_reg & 0xFFu), (uint8_t)(rdesc_reg >> 8) };
    if (dw_xfer(rdreg, 2, rdesc, (int)rdesc_len) < 0) {
        kprintf("i2c_hid: rdesc read failed\n");
        return false;
    }

    if (!parse_report_desc(rdesc, (int)rdesc_len)) {
        kprintf("i2c_hid: no X/Y fields in report descriptor\n");
        return false;
    }

    /* Multitouch HID descriptors are complex and our simple parser can
     * misidentify the tip switch bit.  Disable tip checking entirely:
     * treat every valid report as a touch, and rely on null reports
     * (replen=0) to detect finger-up. */
    if (g_tip_off >= 0)
        kprintf("i2c_hid: tip@bit%d parsed but disabled — using null-report for finger-up\n",
                g_tip_off);
    g_tip_off = -1;

    kprintf("i2c_hid: X[%d..%d]@bit%d(%db) Y[%d..%d]@bit%d(%db) tip@bit%d rid=%u\n",
            g_x_min, g_x_max, g_x_off, g_x_sz,
            g_y_min, g_y_max, g_y_off, g_y_sz,
            g_tip_off, g_touch_rep_id);

    g_addr      = addr;
    g_input_reg = input_reg;
    g_max_input = (int)max_input;
    /* Cap read length to 125 bytes; minimum 8 to cover header + X + Y */
    g_poll_rlen = (int)(max_input > 125u ? 125u : max_input);
    if (g_poll_rlen < 8) g_poll_rlen = 8;
    kprintf("i2c_hid: poll_rlen=%d max_input=%d\n", g_poll_rlen, g_max_input);
    g_last_x    = (g_x_min + g_x_max) / 2;
    g_last_y    = (g_y_min + g_y_max) / 2;
    g_was_touching = false;
    g_active    = true;
    return true;
}

/* ── Public API ─────────────────────────────────────────────────────── */

/* Two virtual pages for LPSS init:
 *   DW_SCAN_VIRT+0x0000 → DW I2C MMIO registers (4017000000)
 *   DW_SCAN_VIRT+0x1000 → ECAM PCI config space page  */
#define DW_SCAN_VIRT  0xFFFFFF0060000000ULL
#define DW_ECAM_VIRT  (DW_SCAN_VIRT + 0x1000ULL)

/* Restore Intel LPSS I2C0 from ACPI D3 state, program BAR, map DW regs.
 *
 * On this Meteor Lake laptop the BIOS calls SOD3() before handing off to the
 * OS, which: sets PMCSR=D3hot AND zeros BAR0 in PCI config space.  We undo
 * that via ECAM (PCIe config space MMIO) at the known address and then map
 * the controller at its known physical address from Linux iomem.
 *
 * ECAM base 0xC0000000 from MCFG. I2C0 at bus=0 dev=0x15 fn=0:
 *   config phys = 0xC0000000 + (0<<20)|(0x15<<15)|(0<<12) = 0xC00A8000
 * DW MMIO phys from iomem: 0x4017000000 (I2C0) / 0x4017001000 (I2C1)
 * PM capability at config offset 0x84 (per DSDT SOD3 analysis).
 */
static bool lpss_i2c_restore(uint64_t dw_phys, uint64_t ecam_phys) {
    /* Map ECAM config page */
    uint64_t ecam_page = ecam_phys & ~0xFFFULL;
    uint64_t ecam_off  = ecam_phys &  0xFFFu;
    if (!vmm_map_page(DW_ECAM_VIRT, ecam_page, VMM_WRITE | VMM_UNCACHE))
        return false;
    vmm_invlpg(DW_ECAM_VIRT);

    volatile uint32_t *cfg = (volatile uint32_t *)(DW_ECAM_VIRT + ecam_off);

    uint32_t vid_did = cfg[0x00u >> 2];
    kprintf("i2c_hid: ECAM vid:did=0x%08x\n", vid_did);

    /* D3hot → D0: clear bits 1:0 of PMCSR at config offset 0x84 */
    uint32_t pm = cfg[0x84u >> 2];
    cfg[0x84u >> 2] = pm & ~0x3u;
    uint64_t t0 = pit_ticks();
    while (pit_ticks() - t0 < 2u);   /* wait ~20ms for D0 */

    /* Disable memory decode, write 64-bit BAR, re-enable */
    uint32_t cmd = cfg[0x04u >> 2];
    cfg[0x04u >> 2] = cmd & ~0x02u;
    cfg[0x10u >> 2] = (uint32_t)(dw_phys & 0xFFFFFFFFu) | 0x04u; /* low + 64-bit type */
    cfg[0x14u >> 2] = (uint32_t)(dw_phys >> 32);
    cfg[0x04u >> 2] = cmd | 0x06u;   /* MSE + BME */

    vmm_unmap_page(DW_ECAM_VIRT);
    vmm_invlpg(DW_ECAM_VIRT);

    /* Map DW register page (also covers LPSS private regs at +0x200) */
    uint64_t dw_page = dw_phys & ~0xFFFULL;
    uint64_t dw_off  = dw_phys &  0xFFFu;
    if (!vmm_map_page(DW_SCAN_VIRT, dw_page, VMM_WRITE | VMM_UNCACHE))
        return false;
    vmm_invlpg(DW_SCAN_VIRT);

    volatile uint32_t *dw   = (volatile uint32_t *)(DW_SCAN_VIRT + dw_off);
    /* LPSS private regs at DW_base + 0x200 (same 4KB page).
     * priv+0x04 = RESETS: bit2=FUNC, bits1:0=IDMA — write 0x7 to de-assert all.
     * priv+0x00 = CLK: bit0=enable, bits15:1=divider — write 0x1 (no division). */
    volatile uint32_t *priv = (volatile uint32_t *)(DW_SCAN_VIRT + dw_off + 0x200u);
    kprintf("i2c_hid: priv_resets=0x%08x priv_clk=0x%08x\n",
            priv[0x04u >> 2], priv[0x00u >> 2]);
    priv[0x00u >> 2] = 0x01u;   /* enable clock, divider=1 */
    priv[0x04u >> 2] = 0x07u;   /* de-assert FUNC + IDMA resets */
    uint64_t t1 = pit_ticks();
    while (pit_ticks() - t1 < 1u);   /* ~10ms for clock + reset settle */

    uint32_t ct = dw[DW_COMP_TYPE >> 2];
    g_init_comp_type = ct;
    kprintf("i2c_hid: LPSS COMP_TYPE=0x%08x (after reset de-assert)\n", ct);

    if (ct == 0x44570140u) {
        g_dw = dw;
        return true;
    }
    vmm_unmap_page(DW_SCAN_VIRT);
    vmm_invlpg(DW_SCAN_VIRT);
    return false;
}

void i2c_hid_init(void) {
    /* Find Intel LPSS I2C controllers (PCI class=0x0C sub=0x80 pi=0x00) */
    uint8_t bus[8], dev[8], fn[8];
    uint32_t n = pci_find_all_class(0x0Cu, 0x80u, 0x00u, bus, dev, fn, 8u);
    if (!n) {
        kprintf("i2c_hid: no LPSS I2C controllers found\n");
        return;
    }
    kprintf("i2c_hid: found %u LPSS I2C controller(s)\n", n);

    /* Read GNVS.IM00 (byte 0x17A) and GNVS.IC00 (byte 0x182) for diagnostics */
    const uint8_t *gnvs = (const uint8_t *)pmm_phys_to_virt(0x73857018ULL);
    uint8_t  im00 = gnvs[0x17Au];
    uint64_t ic00 = 0;
    for (int b = 0; b < 8; b++)
        ic00 |= (uint64_t)gnvs[0x182u + b] << (8 * b);
    g_gnvs_ic00 = ic00;
    kprintf("i2c_hid: GNVS.IM00=0x%02x IC00=0x%08x_%08x\n",
            im00, (uint32_t)(ic00 >> 32), (uint32_t)ic00);

    /* Restore I2C0 via ECAM: wake D3→D0, rewrite BAR, enable memory decode.
     * Addresses confirmed from Linux iomem and MCFG on this Meteor Lake laptop:
     *   ECAM base 0xC0000000, I2C0 config=0xC00A8000, DW MMIO=0x4017000000 */
    if (lpss_i2c_restore(0x4017000000ULL, 0xC00A8000ULL)) {
        kprintf("i2c_hid: LPSS restore succeeded\n");
    } else {
        kprintf("i2c_hid: LPSS restore failed\n");
        return;
    }

    uint32_t ctype = dw_rd(DW_COMP_TYPE);
    kprintf("i2c_hid: COMP_TYPE=0x%08x DW_STATUS=0x%08x\n", ctype, dw_rd(DW_STATUS));

    /* FocalTech FTCS0038 needs ~500ms after LPSS reset to power up.
     * Wait then retry the full probe table up to 4 times with 100ms gaps. */
    uint64_t t_wait = pit_ticks();
    while (pit_ticks() - t_wait < 50u);   /* 500ms initial power-up wait */

    static const struct { uint8_t addr; uint16_t desc_reg; } cands[] = {
        { 0x38u, 0x0001u },  /* FocalTech FTCS0038 — confirmed on this hardware */
        { 0x2Cu, 0x0020u },  /* Synaptics SYNA2BA6 */
        { 0x15u, 0x0001u },  /* ELAN ELAN06FA */
        { 0x5Du, 0x0001u },  /* Goodix GXTP5100 */
        { 0x2Cu, 0x0001u },  /* Synaptics alternate desc reg */
        { 0x40u, 0x0001u },  /* Synaptics alternate address */
    };
    for (int attempt = 0; attempt < 4; attempt++) {
        for (int i = 0; i < (int)(sizeof cands / sizeof cands[0]); i++) {
            if (try_init(cands[i].addr, cands[i].desc_reg)) {
                kprintf("i2c_hid: touchpad ready (attempt %d)\n", attempt);
                return;
            }
        }
        /* Wait 100ms between attempts */
        uint64_t t_retry = pit_ticks();
        while (pit_ticks() - t_retry < 10u);
    }
    kprintf("i2c_hid: no touchpad responded — controller stays mapped for diagnostics\n");
    /* Leave g_dw set so touchpad command can re-probe */
}

void i2c_hid_poll(void) {
    if (!g_active || !g_dw) return;

    /* Re-arm the controller each poll: the DW I2C core needs a fresh
     * setup after the init sequence leaves it in an indeterminate state. */
    dw_setup(g_addr);

    uint8_t ireg[2] = { (uint8_t)(g_input_reg & 0xFFu), (uint8_t)(g_input_reg >> 8) };
    uint8_t rbuf[128];

    if (dw_xfer(ireg, 2, rbuf, g_poll_rlen) < 0) {
        g_poll_fail++;
        g_was_touching = false;
        return;
    }

    /* First 2 bytes: actual report byte count (includes the 2-byte length itself) */
    uint16_t replen = (uint16_t)rbuf[0] | ((uint16_t)rbuf[1] << 8);
    /* Null report (replen=0) or garbage — device is idle, finger lifted */
    if (replen < 3u || replen > 512u) {
        g_was_touching = false;
        return;
    }

    /* Byte 2: report ID — accept rid=1 (relative mouse) or g_touch_rep_id (multitouch) */
    if (rbuf[2] != g_touch_rep_id && rbuf[2] != 1u) {
        g_was_touching = false;
        return;
    }

    /* Report data starts at byte 3; cap dlen to bytes we actually read */
    const uint8_t *data = rbuf + 3;
    int dlen = (int)replen - 3;
    if (dlen > g_poll_rlen - 3) dlen = g_poll_rlen - 3;
    if (dlen <= 0) {
        g_was_touching = false;
        return;
    }

    g_poll_valid++;

    /* Report ID 1 = standard HID relative mouse report.
     * data[0] = button/status byte, data[1] = rel X, data[2] = rel Y. */
    if (rbuf[2] == 1u && dlen >= 3) {
        int32_t dx = (int32_t)(int8_t)data[1];
        int32_t dy = (int32_t)(int8_t)data[2];
        bool lbtn = (data[0] & 0x01u) != 0u;
        bool rbtn = (data[0] & 0x02u) != 0u;
        if (dx || dy || lbtn || rbtn) {
            g_poll_push++;
            mouse_push_rel(dx, dy, lbtn, rbtn);
        }
        g_was_touching = true;
        return;
    }

    /* Report ID matches g_touch_rep_id: absolute multitouch report.
     * Convert absolute position delta to relative screen motion. */
    int32_t x = extract_field(data, dlen, g_x_off, g_x_sz, g_x_signed);
    int32_t y = extract_field(data, dlen, g_y_off, g_y_sz, g_y_signed);

    if (x < g_x_min) x = g_x_min;
    if (x > g_x_max) x = g_x_max;
    if (y < g_y_min) y = g_y_min;
    if (y > g_y_max) y = g_y_max;

    if (g_was_touching) {
        int32_t dx = x - g_last_x;
        int32_t dy = y - g_last_y;
        int32_t xr = g_x_max - g_x_min; if (xr <= 0) xr = 4096;
        int32_t yr = g_y_max - g_y_min; if (yr <= 0) yr = 4096;
        int32_t sw = (int32_t)console_fb_width();
        int32_t sh = (int32_t)console_fb_height();
        int32_t dx_s = (dx * sw) / (xr * 3);
        int32_t dy_s = (dy * sh) / (yr * 3);
        if (dx_s || dy_s) {
            g_poll_push++;
            mouse_push_rel(dx_s, dy_s, false, false);
        }
    }

    g_last_x = x;
    g_last_y = y;
    g_was_touching = true;
}

void i2c_hid_status(void) {
    /* Always re-probe PCI to show diagnostic info regardless of init result */
    uint8_t bus[8], dev[8], fn[8];
    uint32_t n = pci_find_all_class(0x0Cu, 0x80u, 0x00u, bus, dev, fn, 8u);
    kprintf("touchpad: pci_find_all_class(0x0c,0x80,0x00) = %u result(s)\n", n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t id = pci_read32(bus[i], dev[i], fn[i], 0x00u);
        uint32_t bar0_raw = pci_read32(bus[i], dev[i], fn[i], 0x10u);
        uint64_t bar64    = pci_bar_base64(bus[i], dev[i], fn[i], 0);
        kprintf("  [%u] %02x:%02x.%x  vid:did=%08x  BAR0_raw=0x%08x  bar64_hi=0x%08x_lo=0x%08x\n",
                i, bus[i], dev[i], fn[i], id, bar0_raw,
                (uint32_t)(bar64 >> 32), (uint32_t)(bar64 & 0xFFFFFFFFu));
    }

    kprintf("touchpad: GNVS.IC00=0x%08x_%08x  init_COMP_TYPE=0x%08x\n",
            (uint32_t)(g_gnvs_ic00 >> 32), (uint32_t)g_gnvs_ic00, g_init_comp_type);
    if (!g_dw) {
        kprintf("touchpad: g_dw=NULL — LPSS restore failed\n");
        return;
    }
    if (!g_active) {
        kprintf("touchpad: controller mapped COMP=0x%08x STATUS=0x%08x\n",
                dw_rd(DW_COMP_TYPE), dw_rd(DW_STATUS));
        /* Live re-probe: try FTCS0038@0x38 and show raw bytes */
        static const struct { uint8_t addr; uint16_t dreg; } live[] = {
            { 0x38u, 0x0001u },
            { 0x2Cu, 0x0020u },
            { 0x15u, 0x0001u },
            { 0x5Du, 0x0001u },
        };
        for (int i = 0; i < 4; i++) {
            dw_setup(live[i].addr);
            uint8_t wr[2] = { (uint8_t)(live[i].dreg & 0xFF), (uint8_t)(live[i].dreg >> 8) };
            uint8_t rb[30] = {0};
            int r = dw_xfer(wr, 2, rb, 30);
            kprintf("touchpad: probe 0x%02x dreg=0x%04x xfer=%d "
                    "[%02x %02x %02x %02x %02x %02x %02x %02x]\n",
                    live[i].addr, live[i].dreg, r,
                    rb[0],rb[1],rb[2],rb[3],rb[4],rb[5],rb[6],rb[7]);
        }
        return;
    }
    kprintf("touchpad: ACTIVE addr=0x%02x input_reg=0x%04x poll_rlen=%d max_input=%d\n",
            g_addr, g_input_reg, g_poll_rlen, g_max_input);
    kprintf("touchpad: X[%d..%d] bit_off=%d bit_sz=%d %s\n",
            g_x_min, g_x_max, g_x_off, g_x_sz, g_x_signed ? "signed" : "unsigned");
    kprintf("touchpad: Y[%d..%d] bit_off=%d bit_sz=%d %s\n",
            g_y_min, g_y_max, g_y_off, g_y_sz, g_y_signed ? "signed" : "unsigned");
    kprintf("touchpad: tip_bit=%d rep_id=%u\n", g_tip_off, g_touch_rep_id);
    kprintf("touchpad: last_pos=%d,%d touching=%d\n",
            g_last_x, g_last_y, (int)g_was_touching);
    kprintf("touchpad: poll_valid=%u poll_push=%u poll_fail=%u DW_STATUS=0x%08x\n",
            g_poll_valid, g_poll_push, g_poll_fail, dw_rd(DW_STATUS));

    /* Temporarily disable background poll so PIT can't race with our live read */
    g_active = false;

    /* Live poll — run 'touchpad' while finger is on pad to see report bytes */
    dw_setup(g_addr);
    uint8_t ireg[2] = { (uint8_t)(g_input_reg & 0xFFu), (uint8_t)(g_input_reg >> 8) };
    uint8_t rb[128] = {0};
    int r = dw_xfer(ireg, 2, rb, g_poll_rlen);
    uint16_t rlen = (uint16_t)rb[0] | ((uint16_t)rb[1] << 8);
    kprintf("touchpad: live_poll xfer=%d replen=%u rid=%u\n", r, rlen, rb[2]);
    kprintf("touchpad: raw[0..15]=%02x %02x %02x %02x %02x %02x %02x %02x"
            " %02x %02x %02x %02x %02x %02x %02x %02x\n",
            rb[0],rb[1],rb[2],rb[3],rb[4],rb[5],rb[6],rb[7],
            rb[8],rb[9],rb[10],rb[11],rb[12],rb[13],rb[14],rb[15]);
    kprintf("touchpad: raw[16..31]=%02x %02x %02x %02x %02x %02x %02x %02x"
            " %02x %02x %02x %02x %02x %02x %02x %02x\n",
            rb[16],rb[17],rb[18],rb[19],rb[20],rb[21],rb[22],rb[23],
            rb[24],rb[25],rb[26],rb[27],rb[28],rb[29],rb[30],rb[31]);
    /* Always show extracted values so we can verify offsets */
    {
        const uint8_t *d = rb + 3;
        int dl = g_poll_rlen - 3;
        int32_t px = extract_field(d, dl, g_x_off, g_x_sz, g_x_signed);
        int32_t py = extract_field(d, dl, g_y_off, g_y_sz, g_y_signed);
        kprintf("touchpad: extracted x=%d y=%d (valid=%s)\n",
                px, py,
                (r > 0 && rlen >= 3u && rb[2] == g_touch_rep_id) ? "yes" : "no");
    }

    /* Re-enable background poll and restore controller for poll use */
    dw_setup(g_addr);
    g_active = true;
}
