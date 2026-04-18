/*
 * xhci.c — Minimal XHCI USB host controller driver
 * Supports USB HID keyboards in Boot Protocol mode.
 * Handles keyboards directly on root hub ports OR behind a single-tier USB hub
 * (common on laptops where the internal keyboard sits behind an internal hub).
 *
 * Design: polling-only (no MSI/MSI-X), called from pit_on_tick() at 100 Hz.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "xhci.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "keyboard.h"
#include "kprintf.h"
#include "io.h"

/* ── XHCI diagnostic log buffer ──────────────────────────────────────────── */
/*
 * All key XHCI messages are stored here in addition to going to kprintf.
 * Viewable via the `xhci-log` shell command once boot completes.
 * Useful when keyboard doesn't work: read the log to diagnose what happened.
 */
#define XHCI_LOG_SIZE 32768
static char     xhci_log_buf[XHCI_LOG_SIZE];
static uint32_t xhci_log_pos = 0;

/* xlog: dual-output to kprintf + log buffer.
 * Supports: %s %u %x (all uint32_t args — cast before passing) */
static void xlog(const char *fmt, ...) {
    char tmp[256];
    int  pos = 0;
    va_list ap;
    va_start(ap, fmt);
    for (const char *f = fmt; *f && pos < 255; f++) {
        if (*f != '%') { tmp[pos++] = *f; continue; }
        f++;
        switch (*f) {
        case 'u': {
            uint32_t v = va_arg(ap, uint32_t);
            if (!v) { tmp[pos++] = '0'; break; }
            char t[12]; int n = 0;
            while (v) { t[n++] = (char)('0' + (v%10)); v /= 10; }
            for (int i = n-1; i >= 0 && pos < 255; i--) tmp[pos++] = t[i];
            break;
        }
        case 'x': {
            uint32_t v = va_arg(ap, uint32_t);
            bool lead = true;
            for (int sh = 28; sh >= 0; sh -= 4) {
                uint8_t nib = (uint8_t)((v >> sh) & 0xF);
                if (nib || !lead || sh == 0) {
                    lead = false;
                    if (pos < 255) tmp[pos++] = "0123456789abcdef"[nib];
                }
            }
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && pos < 255) tmp[pos++] = *s++;
            break;
        }
        default: if (pos < 255) tmp[pos++] = *f; break;
        }
    }
    va_end(ap);
    tmp[pos] = 0;

    /* Append to log buffer */
    for (int i = 0; tmp[i] && xhci_log_pos < XHCI_LOG_SIZE - 1; i++)
        xhci_log_buf[xhci_log_pos++] = tmp[i];
    xhci_log_buf[xhci_log_pos] = 0;

    /* Print to screen/serial */
    kprintf("%s", tmp);
}

/* Exposed so shell's xhci-log command can display the buffer */
const char *xhci_log_get(void)  { return xhci_log_buf; }
uint32_t    xhci_log_size(void) { return xhci_log_pos; }

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static void *memset_x(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

static void *memcpy_x(void *d, const void *s, size_t n) {
    uint8_t *dst = (uint8_t *)d;
    const uint8_t *src = (const uint8_t *)s;
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
    return d;
}

static void udelay(uint32_t us) {
    for (volatile uint32_t i = 0; i < us * 300u; i++)
        __asm__ volatile ("pause");
}

/* ── MMIO accessors ───────────────────────────────────────────────────────── */

static inline uint32_t mmio_r32(uint64_t base, uint32_t off) {
    return *(volatile uint32_t *)(uintptr_t)(base + off);
}
static inline void mmio_w32(uint64_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)(base + off) = val;
}
static inline void mmio_w64(uint64_t base, uint32_t off, uint64_t val) {
    /* Many XHCI controllers don't support atomic 64-bit MMIO writes.
     * Use two 32-bit writes (low first, then high) — same as Linux. */
    volatile uint32_t *ptr = (volatile uint32_t *)(uintptr_t)(base + off);
    ptr[0] = (uint32_t)(val & 0xFFFFFFFFu);
    ptr[1] = (uint32_t)(val >> 32);
}

/* ── XHCI register offsets ───────────────────────────────────────────────── */

#define CAP_CAPLENGTH   0x00u
#define CAP_HCSPARAMS1  0x04u
#define CAP_HCSPARAMS2  0x08u
#define CAP_HCCPARAMS1  0x10u
#define CAP_DBOFF       0x14u
#define CAP_RTSOFF      0x18u

#define OP_USBCMD       0x00u
#define OP_USBSTS       0x04u
#define OP_DNCTRL       0x14u
#define OP_CRCR         0x18u
#define OP_DCBAAP       0x30u
#define OP_CONFIG       0x38u
#define OP_PORTSC(n)    (0x400u + (uint32_t)(n) * 0x10u)

#define USBCMD_RS       (1u << 0)
#define USBCMD_HCRST    (1u << 1)
#define USBCMD_EWE      (1u << 10)
#define USBCMD_INTE     (1u << 2)
#define USBSTS_HCH      (1u << 0)
#define USBSTS_HSE      (1u << 2)
#define USBSTS_PCD      (1u << 4)
#define USBSTS_CNR      (1u << 11)
#define USBSTS_HCE      (1u << 12)

#define PORTSC_CCS      (1u << 0)
#define PORTSC_PED      (1u << 1)
#define PORTSC_PR       (1u << 4)
#define PORTSC_PP       (1u << 9)
#define PORTSC_CSC      (1u << 17)
#define PORTSC_PEC      (1u << 18)
#define PORTSC_PRC      (1u << 21)
#define PORTSC_WRC      (1u << 19)
#define PORTSC_OCC      (1u << 20)
#define PORTSC_PLC      (1u << 22)
#define PORTSC_CEC      (1u << 23)
#define PORTSC_W1C_BITS (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | PORTSC_OCC | \
                         PORTSC_PRC | PORTSC_PLC | PORTSC_CEC)
/* PED is ALSO write-1-to-clear (writing 1 DISABLES the port).
 * Every PORTSC write must mask PED to avoid accidentally disabling the port.
 * Same for WPR (bit 31, warm port reset — writing 1 starts a warm reset). */
#define PORTSC_RW1_DANGER (PORTSC_PED | PORTSC_W1C_BITS | (1u << 31))
#define PORTSC_SPEED(v) (((v) >> 10) & 0xFu)

/* IR = Interrupter Register Set */
#define IR_IMAN     0x00u
#define IR_IMOD     0x04u
#define IR_ERSTSZ   0x08u
#define IR_ERSTBA   0x10u
#define IR_ERDP     0x18u

/* ── TRB definitions ─────────────────────────────────────────────────────── */

typedef struct {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} __attribute__((packed, aligned(16))) xhci_trb_t;

#define TRB_NORMAL      1u
#define TRB_SETUP       2u
#define TRB_DATA        3u
#define TRB_STATUS      4u
#define TRB_LINK        6u
#define TRB_NOOP_CMD    23u
#define TRB_ENABLE_SLOT  9u
#define TRB_DISABLE_SLOT 10u
#define TRB_ADDR_DEV    11u
#define TRB_CONFIG_EP   12u
#define TRB_EVAL_CTX    13u
#define TRB_EV_XFER     32u
#define TRB_EV_CMD      33u
#define TRB_EV_PORT     34u

#define TRB_C           (1u << 0)
#define TRB_TC          (1u << 1)
#define TRB_IOC         (1u << 5)
#define TRB_IDT         (1u << 6)
#define TRB_BSR         (1u << 9)
#define TRB_DIR_IN      (1u << 16)
#define TRB_TRT_IN      (3u << 16)
#define TRB_TRT_NONE    (0u << 16)
#define TRB_TYPE(t)     ((uint32_t)(t) << 10)
#define TRB_SLOT(s)     ((uint32_t)(s) << 24)
#define TRB_EP(ep)      ((uint32_t)(ep) << 16)
#define TRB_CC(trb)     (((trb).status >> 24) & 0xFFu)
#define CC_SUCCESS      1u
#define CC_SHORT_PKT    13u

/* ── Context structures ──────────────────────────────────────────────────── */
/*
 * xHCI contexts can be 32 or 64 bytes depending on the CSZ bit in HCCPARAMS1.
 * Intel controllers typically use CSZ=1 (64-byte contexts).  Instead of
 * compile-time structs, we use byte-offset accessors via g.ctx_sz (32 or 64).
 *
 * Raw 32-byte context layouts (data portion — same for both CSZ values):
 */

typedef struct {
    uint32_t dw0, dw1, dw2, dw3;
    uint32_t rsvd[4];
} __attribute__((packed)) xhci_slot_ctx_t;   /* 32 bytes of data */

typedef struct {
    uint32_t dw0, dw1;
    uint64_t tr_dequeue_ptr;
    uint32_t dw4;
    uint32_t rsvd[3];
} __attribute__((packed)) xhci_ep_ctx_t;     /* 32 bytes of data */

typedef struct {
    uint32_t drop_flags, add_flags;
    uint32_t rsvd[6];
} __attribute__((packed)) xhci_icc_t;        /* 32 bytes of data */

/*
 * Pointer-based accessors into raw context memory (respects CSZ).
 * ctx_bytes = 32 when CSZ=0, 64 when CSZ=1.  Set once in xhci_init_one().
 *
 * Device context layout (output):
 *   slot context at offset 0
 *   EP[i] at offset ctx_bytes * (i + 1)     (i = 0..30, where 0 = EP0)
 *
 * Input context layout:
 *   ICC   at offset 0
 *   slot  at offset ctx_bytes
 *   EP[i] at offset ctx_bytes * (i + 2)
 */
static uint32_t ctx_bytes = 32;  /* updated from CSZ before any context use */

static inline xhci_slot_ctx_t *dev_ctx_slot(void *base) {
    return (xhci_slot_ctx_t *)base;
}
static inline xhci_ep_ctx_t *dev_ctx_ep(void *base, uint8_t ep_index) {
    return (xhci_ep_ctx_t *)((uint8_t *)base + ctx_bytes * (ep_index + 1u));
}
static inline xhci_icc_t *ictx_icc(void *base) {
    return (xhci_icc_t *)base;
}
static inline xhci_slot_ctx_t *ictx_slot(void *base) {
    return (xhci_slot_ctx_t *)((uint8_t *)base + ctx_bytes);
}
static inline xhci_ep_ctx_t *ictx_ep(void *base, uint8_t ep_index) {
    return (xhci_ep_ctx_t *)((uint8_t *)base + ctx_bytes * (ep_index + 2u));
}

typedef struct {
    uint64_t base;
    uint16_t size;
    uint16_t rsvd0;
    uint32_t rsvd1;
} __attribute__((packed)) xhci_erst_t;

/* ── Ring management ─────────────────────────────────────────────────────── */

#define RING_ENTRIES    256u
#define XFER_ENTRIES    64u

typedef struct {
    xhci_trb_t *trbs;
    uint64_t    phys;
    uint32_t    enq, deq;
    uint8_t     cycle;
    uint32_t    size;
} ring_t;

static ring_t ring_alloc(uint32_t entries) {
    ring_t r;
    memset_x(&r, 0, sizeof(r));
    r.size  = entries;
    r.cycle = 1;
    r.phys  = pmm_alloc_pages((entries * 16 + 4095) / 4096);
    r.trbs  = (xhci_trb_t *)pmm_phys_to_virt(r.phys);
    memset_x(r.trbs, 0, entries * 16);
    return r;
}

static void ring_link(ring_t *r) {
    xhci_trb_t *lnk = &r->trbs[r->size - 1];
    lnk->param   = r->phys;
    lnk->status  = 0;
    lnk->control = TRB_TYPE(TRB_LINK) | TRB_TC | (r->cycle ? TRB_C : 0);
}

static xhci_trb_t *ring_enqueue(ring_t *r, uint64_t param, uint32_t status, uint32_t control) {
    xhci_trb_t *trb = &r->trbs[r->enq];
    trb->param   = param;
    trb->status  = status;
    trb->control = control | (r->cycle ? TRB_C : 0);
    r->enq++;
    if (r->enq == r->size - 1) {
        r->trbs[r->size - 1].control =
            TRB_TYPE(TRB_LINK) | TRB_TC | (r->cycle ? TRB_C : 0);
        r->enq   = 0;
        r->cycle ^= 1;
    }
    return trb;
}

/* ── Controller global state ─────────────────────────────────────────────── */

#define MAX_SLOTS   32u
#define KBD_BUFS    8u

static struct {
    bool     present;
    uint64_t cap, op, rt, db;
    uint8_t  max_slots, max_ports;

    uint64_t *dcbaa;
    uint64_t  dcbaa_phys;

    ring_t   cmd;
    ring_t   evt;

    xhci_erst_t *erst;
    uint64_t     erst_phys;

    void           *dev_ctx[MAX_SLOTS + 1];  /* raw context memory, use accessors */
    uint64_t        dev_ctx_phys[MAX_SLOTS + 1];
    ring_t          xfer[MAX_SLOTS + 1];

    int      kbd_slot;
    int      kbd_ep_dci;
    uint8_t  last_keys[6];

    uint8_t  *kbd_data[KBD_BUFS];
    uint64_t  kbd_data_phys[KBD_BUFS];

    /* USB mass storage (for writing debug log to USB drive) */
    int      stor_slot;
    int      stor_bout_dci;
    int      stor_bin_dci;
    ring_t   stor_bout;
    ring_t   stor_bin;

    /* USB2/USB3 port protocol bitmap (from xHCI Extended Protocol Caps) */
    uint32_t usb2_ports;    /* bitmask: bit N = port N is USB2 */
    uint32_t usb3_ports;    /* bitmask: bit N = port N is USB3 */

    /* Keyboard detection diagnostic (printed at bottom of screen) */
    const char *kbd_fail;   /* last failure reason string */
    uint32_t    kbd_fail_cc; /* completion code if applicable */
    uint32_t    last_cc;    /* most recent CC from any failed transfer/cmd */
    uint16_t    kbd_vid, kbd_pid;  /* device that was attempted */
    uint8_t     kbd_ep;     /* endpoint that was selected */
    uint8_t     kbd_fail_port;  /* 1-based port where failure occurred */
    uint8_t     kbd_fail_slot;  /* slot where failure occurred */
    uint8_t     kbd_fail_speed; /* speed code used when failure occurred */
} g;

/* ── Doorbell ────────────────────────────────────────────────────────────── */

static void ring_doorbell(uint8_t slot, uint8_t dci) {
    /* Ensure all TRB writes are globally visible before the xHC reads them */
    __asm__ volatile ("sfence" ::: "memory");
    mmio_w32(g.db, (uint32_t)slot * 4u, dci);
}

/* ── Event ring: wait for one event of a given type ─────────────────────── */

static int g_wait_verbose = 3;  /* log first N non-matching events */

static bool wait_event(uint8_t want_type, uint32_t *cc, uint64_t *param, uint8_t *slot) {
    for (int i = 0; i < 200000; i++) {   /* 2 seconds (was 300ms) */
        xhci_trb_t *ev = &g.evt.trbs[g.evt.deq];
        if ((ev->control & 1u) != (uint32_t)g.evt.cycle) { udelay(10); continue; }

        uint8_t type = (uint8_t)((ev->control >> 10) & 0x3Fu);
        bool match = (want_type == 0 || type == want_type);

        if (!match && g_wait_verbose > 0) {
            g_wait_verbose--;
            xlog("[xhci] evt: got type=%u want=%u cc=%u deq=%u\n",
                 (uint32_t)type, (uint32_t)want_type,
                 TRB_CC(*ev), g.evt.deq);
        }

        if (match) {
            if (cc)    *cc    = TRB_CC(*ev);
            if (param) *param = ev->param;
            if (slot)  *slot  = (uint8_t)(ev->control >> 24);
        }

        g.evt.deq++;
        if (g.evt.deq >= g.evt.size) { g.evt.deq = 0; g.evt.cycle ^= 1; }
        mmio_w64(g.rt + 0x20u, IR_ERDP, g.evt.phys + g.evt.deq * 16u | (1u << 3));

        if (match) return true;
    }
    return false;
}

/* ── Drain all pending events from event ring (non-blocking) ────────────── */
/* Call this during port reset to prevent event ring backup.
 * Returns count of events consumed. Logs Port Status Change Events. */
static uint32_t drain_events(void) {
    uint32_t count = 0;
    for (int i = 0; i < 256; i++) {
        xhci_trb_t *ev = &g.evt.trbs[g.evt.deq];
        if ((ev->control & 1u) != (uint32_t)g.evt.cycle) break;

        uint8_t type = (uint8_t)((ev->control >> 10) & 0x3Fu);
        count++;

        /* Log port status change events (type 34) */
        if (type == 34) {
            uint8_t port_id = (uint8_t)(ev->param >> 24);
            uint32_t cc = TRB_CC(*ev);
            xlog("[xhci] PSC event: port %u cc=%u\n",
                 (uint32_t)port_id, (uint32_t)cc);
        }

        g.evt.deq++;
        if (g.evt.deq >= g.evt.size) { g.evt.deq = 0; g.evt.cycle ^= 1; }
        mmio_w64(g.rt + 0x20u, IR_ERDP, g.evt.phys + g.evt.deq * 16u | (1u << 3));
    }
    return count;
}

/* ── USB setup packet builder ────────────────────────────────────────────── */

static uint64_t usb_setup(uint8_t bmRT, uint8_t bReq,
                           uint16_t wVal, uint16_t wIdx, uint16_t wLen) {
    return (uint64_t)bmRT | ((uint64_t)bReq << 8) | ((uint64_t)wVal << 16)
         | ((uint64_t)wIdx << 32) | ((uint64_t)wLen << 48);
}

/* ── Control transfer on EP0 of slot ─────────────────────────────────────── */

static bool ctrl_xfer(uint8_t slot, uint64_t setup, uint64_t data_phys,
                      uint32_t len, bool in) {
    ring_t *r = &g.xfer[slot];
    uint32_t trt = (len == 0) ? TRB_TRT_NONE : (in ? TRB_TRT_IN : (2u << 16));
    ring_enqueue(r, setup, 8u, TRB_TYPE(TRB_SETUP) | TRB_IDT | trt);
    if (len > 0) {
        uint32_t dir = in ? TRB_DIR_IN : 0u;
        ring_enqueue(r, data_phys, len, TRB_TYPE(TRB_DATA) | dir);
    }
    ring_enqueue(r, 0, 0,
                 TRB_TYPE(TRB_STATUS) | ((len > 0 && in) ? 0u : TRB_DIR_IN) | TRB_IOC);
    ring_doorbell((uint8_t)slot, 1);

    uint32_t cc = 0;
    if (!wait_event(TRB_EV_XFER, &cc, NULL, NULL)) {
        /* Deep diagnostic: check if xHC is alive and EP0 state */
        uint32_t sts = mmio_r32(g.op, OP_USBSTS);
        xhci_ep_ctx_t *oep = dev_ctx_ep(g.dev_ctx[slot], 0);
        uint32_t ep_st = oep->dw1 & 7u;
        uint32_t ep_deq = (uint32_t)(oep->tr_dequeue_ptr & 0xFFFFFFFFu);
        xlog("[xhci]   TIMEOUT slot=%u len=%u STS=0x%x epst=%u deq=0x%x\n",
             (uint32_t)slot, len, sts, ep_st, ep_deq);
        g.last_cc = 999;
        return false;
    }
    if (cc != CC_SUCCESS && cc != CC_SHORT_PKT) {
        xlog("[xhci]   ctrl_xfer CC=%u slot=%u len=%u\n",
             cc, (uint32_t)slot, len);
        g.last_cc = cc;
        return false;
    }
    return true;
}

/* ── BIOS/OS handoff (claim controller from UEFI/BIOS) ──────────────────── */
/*
 * Without this, the BIOS SMI handler remains active and interferes with
 * all USB operations — port resets fail, bulk transfers time out, etc.
 */
static void xhci_bios_handoff(void) {
    uint32_t hcc = mmio_r32(g.cap, CAP_HCCPARAMS1);
    uint32_t xecp_off = ((hcc >> 16) & 0xFFFFu) << 2;  /* DWORDs → bytes */
    if (!xecp_off) return;

    uint32_t base = xecp_off;
    for (int i = 0; i < 32; i++) {
        uint32_t cap = mmio_r32(g.cap, base);
        uint8_t  id  = (uint8_t)(cap & 0xFFu);
        uint8_t  nxt = (uint8_t)((cap >> 8) & 0xFFu);

        if (id == 1) {
            /* USB Legacy Support capability found */
            xlog("[xhci] USBLEGSUP at 0x%x val=0x%x\n", (uint32_t)base, (uint32_t)cap);

            if (cap & (1u << 16)) {
                /* BIOS owns the controller — request handoff */
                xlog("[xhci] BIOS owns controller, requesting handoff\n");
                mmio_w32(g.cap, base, cap | (1u << 24));  /* set OS Owned bit */

                /* Wait up to 1s for BIOS to release */
                for (int w = 0; w < 1000; w++) {
                    cap = mmio_r32(g.cap, base);
                    if (!(cap & (1u << 16))) break;  /* BIOS released */
                    udelay(1000);
                }
                cap = mmio_r32(g.cap, base);
                if (cap & (1u << 16))
                    xlog("[xhci] WARNING: BIOS did not release (cap=0x%x)\n", (uint32_t)cap);
                else
                    xlog("[xhci] BIOS handoff OK\n");
            } else {
                xlog("[xhci] no BIOS ownership\n");
            }

            /* Disable all SMI sources in USBLSTS (next DWORD) */
            mmio_w32(g.cap, base + 4, 0);
            return;
        }

        if (!nxt) break;
        base += (uint32_t)nxt << 2;
    }
    xlog("[xhci] no USBLEGSUP capability\n");
}

/* ── Parse xHCI Supported Protocol capabilities (USB2 vs USB3 ports) ───── */
/*
 * Extended capability id=2 describes which XHCI port numbers correspond to
 * USB 2.0 vs USB 3.x protocols. This is critical because:
 * - A USB2 device on a physical port only appears on the USB2 XHCI port
 * - Trying to reset the corresponding USB3 port for that device always fails
 * - AMD controllers pair USB2/USB3 ports: e.g. ports 1-4=USB3, 5-8=USB2
 */
static void xhci_parse_protocols(void) {
    uint32_t hcc = mmio_r32(g.cap, CAP_HCCPARAMS1);
    uint32_t xecp_off = ((hcc >> 16) & 0xFFFFu) << 2;
    if (!xecp_off) return;

    uint32_t base = xecp_off;
    for (int i = 0; i < 32; i++) {
        uint32_t cap = mmio_r32(g.cap, base);
        uint8_t  id  = (uint8_t)(cap & 0xFFu);
        uint8_t  nxt = (uint8_t)((cap >> 8) & 0xFFu);

        if (id == 2) {
            /* Supported Protocol capability */
            uint8_t major = (uint8_t)((cap >> 24) & 0xFFu);
            uint32_t dw2  = mmio_r32(g.cap, base + 8);
            uint8_t  port_off   = (uint8_t)(dw2 & 0xFFu);        /* 1-based */
            uint8_t  port_count = (uint8_t)((dw2 >> 8) & 0xFFu);

            xlog("[xhci] protocol USB%u ports %u-%u\n",
                 (uint32_t)major,
                 (uint32_t)port_off,
                 (uint32_t)(port_off + port_count - 1));

            for (uint8_t p = port_off; p < port_off + port_count && p <= 32; p++) {
                uint8_t bit = (uint8_t)(p - 1);  /* 0-based */
                if (bit < 32) {
                    if (major == 2)
                        g.usb2_ports |= (1u << bit);
                    else if (major == 3)
                        g.usb3_ports |= (1u << bit);
                }
            }
        }

        if (!nxt) break;
        base += (uint32_t)nxt << 2;
    }
    xlog("[xhci] usb2_mask=0x%x usb3_mask=0x%x\n",
         (uint32_t)g.usb2_ports, (uint32_t)g.usb3_ports);
}

/* ── Controller reset + start ────────────────────────────────────────────── */

static bool ctrl_reset(void) {
    uint32_t cmd = mmio_r32(g.op, OP_USBCMD);
    mmio_w32(g.op, OP_USBCMD, cmd & ~USBCMD_RS);
    for (int i = 0; i < 100; i++) {
        if (mmio_r32(g.op, OP_USBSTS) & USBSTS_HCH) break;
        udelay(1000);
    }
    mmio_w32(g.op, OP_USBCMD, mmio_r32(g.op, OP_USBCMD) | USBCMD_HCRST);
    udelay(100);
    for (int i = 0; i < 500; i++) {
        if (!(mmio_r32(g.op, OP_USBCMD) & USBCMD_HCRST) &&
            !(mmio_r32(g.op, OP_USBSTS) & USBSTS_CNR)) return true;
        udelay(1000);
    }
    return false;
}

static bool ctrl_start(void) {
    mmio_w32(g.op, OP_USBCMD, mmio_r32(g.op, OP_USBCMD) | USBCMD_RS | USBCMD_EWE);
    for (int i = 0; i < 100; i++) {
        if (!(mmio_r32(g.op, OP_USBSTS) & USBSTS_HCH)) return true;
        udelay(1000);
    }
    return false;
}

/* ── Enable Slot command ─────────────────────────────────────────────────── */

static int g_eslot_diag_done = 0;  /* one-shot: dump raw ring once */

static bool cmd_enable_slot(uint8_t *slot_out) {
    ring_enqueue(&g.cmd, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT) | TRB_IOC);
    ring_doorbell(0, 0);
    uint32_t cc = 0; uint8_t slot = 0;
    if (!wait_event(TRB_EV_CMD, &cc, NULL, &slot)) {
        uint32_t sts = mmio_r32(g.op, OP_USBSTS);
        uint32_t cmd = mmio_r32(g.op, OP_USBCMD);
        xlog("[xhci] enable_slot TIMEOUT sts=0x%x cmd=0x%x\n", sts, cmd);
        if (!g_eslot_diag_done) {
            g_eslot_diag_done = 1;
            /* Dump our software event ring state */
            xlog("[xhci] evt.deq=%u evt.cycle=%u evt.phys=0x%x\n",
                 g.evt.deq, (uint32_t)g.evt.cycle,
                 (uint32_t)(g.evt.phys & 0xFFFFFFFFu));
            /* Dump first 4 event TRBs raw */
            for (int d = 0; d < 4; d++) {
                xlog("[xhci] evt[%d]: c=0x%x p=0x%x s=0x%x\n",
                     d, g.evt.trbs[d].control,
                     (uint32_t)(g.evt.trbs[d].param & 0xFFFFFFFFu),
                     g.evt.trbs[d].status);
            }
            /* Dump ERST entry */
            xlog("[xhci] erst[0]: base=0x%x size=%u\n",
                 (uint32_t)(g.erst[0].base & 0xFFFFFFFFu),
                 (uint32_t)g.erst[0].size);
            /* Dump interrupter 0 registers */
            uint64_t ir0 = g.rt + 0x20u;
            xlog("[xhci] IMAN=0x%x ERSTSZ=0x%x\n",
                 mmio_r32(ir0, IR_IMAN), mmio_r32(ir0, IR_ERSTSZ));
            /* ERSTBA and ERDP readback (64-bit, show low 32) */
            uint32_t erstba_lo = mmio_r32(ir0, IR_ERSTBA);
            uint32_t erdp_lo   = mmio_r32(ir0, IR_ERDP);
            xlog("[xhci] ERSTBA_lo=0x%x ERDP_lo=0x%x\n",
                 erstba_lo, erdp_lo);
            xlog("[xhci] erst_phys=0x%x\n",
                 (uint32_t)(g.erst_phys & 0xFFFFFFFFu));
        }
        return false;
    }
    if (cc != CC_SUCCESS) {
        xlog("[xhci] enable_slot cc=%u slot=%u\n", cc, (uint32_t)slot);
        return false;
    }
    *slot_out = slot;
    return true;
}

/* ── Address Device command ──────────────────────────────────────────────── */
/*
 * route_string: 20-bit path through hubs (0 = directly on root)
 * speed:        PORTSC speed code (1=FS, 2=LS, 3=HS, 4=SS)
 * root_port1:   1-based root hub port number
 * hub_slot:     slot of parent hub (0 = no hub, directly on root)
 * hub_port:     port number on parent hub (0 = no hub)
 * ep0_mps:      max packet size for EP0 (8 for LS/FS, 64 for HS)
 */
static bool cmd_address_device(uint8_t slot, uint32_t route_string, uint8_t speed,
                                uint8_t root_port1, uint8_t hub_slot, uint8_t hub_port,
                                uint16_t ep0_mps) {
    /* Allocate output device context */
    uint64_t phys = pmm_alloc_pages(2);
    if (!phys) return false;
    g.dev_ctx[slot]      = (void *)pmm_phys_to_virt(phys);
    g.dev_ctx_phys[slot] = phys;
    memset_x(g.dev_ctx[slot], 0, 8192);
    g.dcbaa[slot] = phys;

    /* Allocate input context */
    uint64_t iphys = pmm_alloc_pages(2);
    if (!iphys) return false;
    void *ic = (void *)pmm_phys_to_virt(iphys);
    memset_x(ic, 0, 8192);

    ictx_icc(ic)->add_flags = (1u << 0) | (1u << 1);  /* A0=slot, A1=EP0 */

    /* Slot context */
    xhci_slot_ctx_t *sl = ictx_slot(ic);
    sl->dw0 = (route_string & 0xFFFFFu)
            | ((uint32_t)speed << 20)
            | (1u << 27);        /* context entries = 1 (EP0 only) */
    sl->dw1 = (uint32_t)root_port1 << 16;
    if (hub_slot) {
        sl->dw2 = (uint32_t)hub_slot        /* parent hub slot */
                 | ((uint32_t)hub_port << 8); /* parent port */
    }

    /* EP0 context */
    g.xfer[slot] = ring_alloc(RING_ENTRIES);
    ring_link(&g.xfer[slot]);
    xhci_ep_ctx_t *ep0 = ictx_ep(ic, 0);
    ep0->dw1           = (3u << 1) | (4u << 3) | ((uint32_t)ep0_mps << 16);
    ep0->tr_dequeue_ptr = g.xfer[slot].phys | 1u;
    ep0->dw4           = 8u;

    ring_enqueue(&g.cmd, iphys, 0, TRB_TYPE(TRB_ADDR_DEV) | TRB_SLOT(slot) | TRB_IOC);
    ring_doorbell(0, 0);

    uint32_t cc = 0;
    if (!wait_event(TRB_EV_CMD, &cc, NULL, NULL)) {
        xlog("[xhci] addr_dev TIMEOUT slot=%u spd=%u rp=%u\n",
             (uint32_t)slot, (uint32_t)speed, (uint32_t)root_port1);
        g.last_cc = 999;
        return false;
    }
    if (cc != CC_SUCCESS) {
        xlog("[xhci] addr_dev cc=%u slot=%u spd=%u\n",
             cc, (uint32_t)slot, (uint32_t)speed);
        g.last_cc = cc;
        return false;
    }
    return true;
}

/* ── Minimal USB descriptor structs ──────────────────────────────────────── */

typedef struct {
    uint8_t  bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} __attribute__((packed)) usb_dev_desc_t;

typedef struct {
    uint8_t  bLength, bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces, bConfigurationValue, iConfiguration,
             bmAttributes, bMaxPower;
} __attribute__((packed)) usb_cfg_desc_t;

typedef struct {
    uint8_t  bLength, bDescriptorType;
    uint8_t  bInterfaceNumber, bAlternateSetting, bNumEndpoints,
             bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
} __attribute__((packed)) usb_intf_desc_t;

typedef struct {
    uint8_t  bLength, bDescriptorType;
    uint8_t  bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed)) usb_ep_desc_t;

/* USB Hub descriptor (class-specific, type 0x29) */
typedef struct {
    uint8_t  bLength, bDescriptorType;
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;   /* * 2ms = port power-on time */
    uint8_t  bHubContrCurrent;
    uint8_t  DeviceRemovable[4];
} __attribute__((packed)) usb_hub_desc_t;

/* Hub class requests */
#define HUB_SET_FEATURE     0x03
#define HUB_CLEAR_FEATURE   0x01
#define HUB_GET_STATUS      0x00
#define HUB_PORT_POWER      8
#define HUB_PORT_RESET      4
#define HUB_C_PORT_CONN     16
#define HUB_C_PORT_RESET    20

/* ── Try to enumerate one device ─────────────────────────────────────────── */
/*
 * Returns: 0=not kbd/hub, 1=keyboard OK, -1=error, 2=mass storage set up
 */
static int try_enumerate_kbd(uint8_t slot, uint8_t root_port1, uint8_t speed,
                              uint32_t route, uint8_t hub_slot, uint8_t hub_port) {
    uint16_t ep0_mps = (speed == 4) ? 512 : (speed == 2) ? 8 : 64;

    xlog("[xhci]   enum slot=%u rp=%u spd=%u mps=%u\n",
         (uint32_t)slot, (uint32_t)root_port1,
         (uint32_t)speed, (uint32_t)ep0_mps);

    if (!cmd_address_device(slot, route, speed, root_port1,
                             hub_slot, hub_port, ep0_mps)) {
        xlog("[xhci]   slot %u addr FAIL spd=%u mps=%u\n",
             (uint32_t)slot, (uint32_t)speed, (uint32_t)ep0_mps);
        if (!g.kbd_fail) { g.kbd_fail = "ADDR"; g.kbd_fail_cc = g.last_cc;
        g.kbd_fail_port = root_port1; g.kbd_fail_slot = slot; g.kbd_fail_speed = speed; }
        return -1;
    }
    xlog("[xhci]   slot %u addr OK spd=%u\n",
         (uint32_t)slot, (uint32_t)speed);

    /* Verify EP0 state in output device context */
    {
        uint32_t epst = dev_ctx_ep(g.dev_ctx[slot], 0)->dw0 & 7u;
        if (epst != 1)
            xlog("[xhci]   WARNING: EP0 state=%u (expected 1=Running)\n", epst);
    }

    /* USB spec: 2ms SetAddress recovery time before first transfer */
    udelay(10000);  /* 10ms — generous */
    drain_events(); /* clear any stale events before first EP0 transfer */

    uint64_t buf_phys = pmm_alloc_page();
    if (!buf_phys) return -1;
    uint8_t *buf = (uint8_t *)pmm_phys_to_virt(buf_phys);

    /* Try 8-byte device descriptor read with retries.
     * Many USB devices need time after SET_ADDRESS before responding. */
    bool got_dd8 = false;
    for (int retry = 0; retry < 4 && !got_dd8; retry++) {
        if (retry > 0) {
            udelay(100000);  /* 100ms between retries */
            xlog("[xhci]   slot %u devdesc8 retry %d\n",
                 (uint32_t)slot, retry);
        }
        memset_x(buf, 0, 64);
        got_dd8 = ctrl_xfer(slot, usb_setup(0x80, 6, 0x0100, 0, 8),
                             buf_phys, 8, true);
    }
    if (!got_dd8) {
        xlog("[xhci]   slot %u devdesc8 FAIL x4 cc=%u\n",
             (uint32_t)slot, g.last_cc);
        {
            g.kbd_fail = "DEVDESC"; g.kbd_fail_cc = g.last_cc;
            g.kbd_fail_port = root_port1; g.kbd_fail_slot = slot; g.kbd_fail_speed = speed;

            return -1;
        }
    }
    /* Got first 8 bytes — check actual EP0 max packet size */
    usb_dev_desc_t *dd = (usb_dev_desc_t *)buf;
    uint16_t real_mps = dd->bMaxPacketSize0;
    if (real_mps == 0) real_mps = ep0_mps;
    xlog("[xhci]   slot %u devdesc8 OK mps0=%u\n",
         (uint32_t)slot, (uint32_t)real_mps);
    /* Now read the full 18-byte device descriptor */
    memset_x(buf, 0, 4096);
    if (!ctrl_xfer(slot, usb_setup(0x80, 6, 0x0100, 0, 18), buf_phys, 18, true)) {
        xlog("[xhci]   slot %u devdesc18 FAIL\n", (uint32_t)slot);
        g.kbd_fail = "DEVDESC18"; g.kbd_fail_cc = g.last_cc;
        g.kbd_fail_port = root_port1; g.kbd_fail_slot = slot; g.kbd_fail_speed = speed;
        return -1;
    }

    dd = (usb_dev_desc_t *)buf;
    uint8_t dev_class = dd->bDeviceClass;
    xlog("[xhci]   slot %u class=%x vid=%x pid=%x\n",
         (uint32_t)slot, (uint32_t)dev_class,
         (uint32_t)dd->idVendor, (uint32_t)dd->idProduct);

    if (dev_class == 0x09) return 0;   /* hub — caller handles */

    memset_x(buf, 0, 255);
    if (!ctrl_xfer(slot, usb_setup(0x80, 6, 0x0200, 0, 255), buf_phys, 255, true)) {
        g.kbd_fail = "CFGDESC"; g.kbd_fail_cc = g.last_cc;
        g.kbd_vid = dd->idVendor; g.kbd_pid = dd->idProduct;
        g.kbd_fail_port = root_port1; g.kbd_fail_slot = slot; g.kbd_fail_speed = speed;
        return -1;
    }

    usb_cfg_desc_t *cd = (usb_cfg_desc_t *)buf;
    if (cd->bDescriptorType != 2) return 0;

    uint8_t  cfg_val    = cd->bConfigurationValue;
    uint8_t  ep_addr    = 0x81;
    uint16_t ep_mps     = 8;
    uint8_t  ep_iv      = 10;
    uint8_t  kbd_intf   = 0;   /* bInterfaceNumber of keyboard interface */
    bool     found_kbd  = false;
    uint8_t  kbd_prio   = 0;   /* 0=none, 1=generic HID, 2=boot kbd (3/1/1) */
    /* Mass storage detection */
    bool     found_msc  = false;
    uint8_t  msc_bout_addr = 0, msc_bin_addr = 0;
    uint16_t msc_bout_mps = 64, msc_bin_mps = 64;
    uint16_t total = cd->wTotalLength;
    if (total > 255) total = 255;
    uint16_t off = cd->bLength;
    bool     cur_kbd = false, cur_msc = false;

    while (off < total) {
        uint8_t len  = buf[off];
        uint8_t type = buf[off + 1];
        if (len < 2) break;
        if (type == 4) {    /* Interface descriptor */
            usb_intf_desc_t *id = (usb_intf_desc_t *)(buf + off);
            xlog("[xhci]   intf#%u class=%x sub=%x proto=%x nep=%u\n",
                 (uint32_t)id->bInterfaceNumber,
                 (uint32_t)id->bInterfaceClass,
                 (uint32_t)id->bInterfaceSubClass,
                 (uint32_t)id->bInterfaceProtocol,
                 (uint32_t)id->bNumEndpoints);
            /* Prefer boot keyboard (3/1/1) over generic HID (3/x/x).
             * Don't overwrite a boot kbd endpoint with a later generic
             * HID interface (e.g. consumer control on the same device). */
            cur_kbd = false;
            cur_msc = (id->bInterfaceClass == 0x08);
            if (id->bInterfaceClass == 3) {
                uint8_t prio = (id->bInterfaceSubClass == 1
                             && id->bInterfaceProtocol == 1) ? 2u : 1u;
                if (prio >= kbd_prio) {
                    cur_kbd = true;
                    kbd_prio = prio;
                    found_kbd = true;
                    kbd_intf = id->bInterfaceNumber;
                }
            }
            if (cur_msc) found_msc = true;
        } else if (type == 5) {     /* Endpoint descriptor */
            usb_ep_desc_t *ep = (usb_ep_desc_t *)(buf + off);
            if (cur_kbd && (ep->bEndpointAddress & 0x80u) &&
                (ep->bmAttributes & 3u) == 3u) {
                ep_addr = ep->bEndpointAddress;
                ep_mps  = ep->wMaxPacketSize;
                ep_iv   = ep->bInterval;
            }
            if (cur_msc && (ep->bmAttributes & 3u) == 2u) {   /* bulk */
                if (ep->bEndpointAddress & 0x80u) {
                    msc_bin_addr = ep->bEndpointAddress;
                    msc_bin_mps  = ep->wMaxPacketSize;
                } else {
                    msc_bout_addr = ep->bEndpointAddress;
                    msc_bout_mps  = ep->wMaxPacketSize;
                }
            }
        }
        off = (uint16_t)(off + len);
    }

    /* ── Keyboard path ── */
    if (found_kbd) {
        g.kbd_vid = dd->idVendor;
        g.kbd_pid = dd->idProduct;
        g.kbd_ep  = ep_addr;

        uint8_t ep_num = ep_addr & 0x0Fu;
        uint8_t ep_dir = (ep_addr & 0x80u) ? 1u : 0u;
        uint8_t dci    = (uint8_t)(ep_num * 2u + ep_dir);
        g.kbd_ep_dci   = dci;

        xlog("[xhci]   KBD ep=%x dci=%u mps=%u iv=%u cfg=%u\n",
             (uint32_t)ep_addr, (uint32_t)dci, (uint32_t)ep_mps,
             (uint32_t)ep_iv, (uint32_t)cfg_val);

        /* ── Configure Endpoint ── */
        uint64_t iphys = pmm_alloc_pages(2);
        if (!iphys) return -1;
        void *ic = (void *)pmm_phys_to_virt(iphys);
        memset_x(ic, 0, 8192);

        ictx_icc(ic)->add_flags = (1u << 0) | (1u << dci);

        /* Copy slot context from output device context (xHC compares Route
         * String, Speed, Hub, MTT, Root Hub Port # against output — must
         * match).  Zero dw3: spec says Slot State & Device Address = 0. */
        memcpy_x(ictx_slot(ic), dev_ctx_slot(g.dev_ctx[slot]),
                 sizeof(xhci_slot_ctx_t));
        ictx_slot(ic)->dw0 = (ictx_slot(ic)->dw0 & ~(0x1Fu << 27))
                             | ((uint32_t)dci << 27);
        ictx_slot(ic)->dw3 = 0;  /* Slot State & USB Device Address must be 0 */

        /* Interrupt-IN transfer ring */
        ring_t ep0_ring = g.xfer[slot];  /* save EP0 ring */
        g.xfer[slot] = ring_alloc(XFER_ENTRIES);
        ring_link(&g.xfer[slot]);

        /* xHCI interval: HS/SS use bInterval-1; FS/LS use log2(bInterval*8) */
        uint8_t xi;
        if (speed >= 3) {
            xi = (ep_iv >= 1) ? (uint8_t)(ep_iv - 1u) : 0u;
            if (xi > 15) xi = 15;
        } else {
            xi = 3;
            if (ep_iv >= 1) {
                uint32_t mf = (uint32_t)ep_iv * 8u;
                uint8_t  lg = 0;
                while ((1u << lg) < mf && lg < 15) lg++;
                xi = lg;
            }
        }

        /* Max ESIT Payload = wMaxPacketSize for simple interrupt endpoints
         * (Mult=0, MaxBurst=0).  xHCI spec requires this for ALL periodic
         * endpoints — 0 causes Parameter Error on Intel controllers. */
        uint32_t max_esit = (uint32_t)ep_mps;

        xhci_ep_ctx_t *epc = ictx_ep(ic, (uint8_t)(dci - 1));
        epc->dw0 = (uint32_t)xi << 16;
        epc->dw1 = (3u << 1) | (7u << 3) | ((uint32_t)ep_mps << 16);
        epc->tr_dequeue_ptr = g.xfer[slot].phys | 1u;
        epc->dw4 = max_esit | (max_esit << 16);

        ring_enqueue(&g.cmd, iphys, 0,
                     TRB_TYPE(TRB_CONFIG_EP) | TRB_SLOT(slot) | TRB_IOC);
        ring_doorbell(0, 0);

        uint32_t cc = 0;
        if (!wait_event(TRB_EV_CMD, &cc, NULL, NULL)) {
            kprintf("\n\n!!! CFG_EP TIMEOUT slot=%u dci=%u !!!\n",
                    (unsigned)slot, (unsigned)dci);
            g.kbd_fail = "CFG_TIMEOUT";
            g.kbd_fail_cc = 999;
            g.kbd_fail_port = root_port1; g.kbd_fail_slot = slot; g.kbd_fail_speed = speed;
            g.xfer[slot] = ep0_ring;
            return -1;
        }
        if (cc != CC_SUCCESS) {
            kprintf("\n\n!!! CFG_EP FAIL CC=%u slot=%u dci=%u !!!\n",
                    (unsigned)cc, (unsigned)slot, (unsigned)dci);
            g.kbd_fail = "CFG_EP";
            g.kbd_fail_cc = cc;
            g.kbd_fail_port = root_port1; g.kbd_fail_slot = slot; g.kbd_fail_speed = speed;
            g.xfer[slot] = ep0_ring;
            return -1;
        }
        xlog("[xhci]   CONFIG_EP OK\n");

        /* ── SET_CONFIGURATION + SET_PROTOCOL via EP0 ── */
        ring_t intr_ring = g.xfer[slot];
        g.xfer[slot] = ep0_ring;
        if (!ctrl_xfer(slot, usb_setup(0x00, 9, cfg_val, 0, 0), 0, 0, false)) {
            xlog("[xhci]   SET_CONFIG FAIL\n");
            g.kbd_fail = "SET_CFG";
            g.xfer[slot] = intr_ring;
            return -1;
        }
        xlog("[xhci]   SET_CONFIG OK\n");

        /* SET_PROTOCOL(0=boot) on the correct interface */
        bool sp_ok = ctrl_xfer(slot,
            usb_setup(0x21, 0x0B, 0, kbd_intf, 0), 0, 0, false);
        xlog("[xhci]   SET_PROTOCOL(boot) intf=%u %s\n",
             (uint32_t)kbd_intf, sp_ok ? "OK" : "FAIL");

        /* SET_IDLE(0) — some devices need this to start reporting */
        ctrl_xfer(slot,
            usb_setup(0x21, 0x0A, 0, kbd_intf, 0), 0, 0, false);

        g.xfer[slot] = intr_ring;
        xlog("[xhci]   KBD READY slot=%u intf=%u\n",
             (uint32_t)slot, (uint32_t)kbd_intf);

        ring_t *xr = &g.xfer[slot];
        for (uint32_t i = 0; i < KBD_BUFS; i++) {
            g.kbd_data_phys[i] = pmm_alloc_page();
            g.kbd_data[i]      = (uint8_t *)pmm_phys_to_virt(g.kbd_data_phys[i]);
            memset_x(g.kbd_data[i], 0, 4096);
            ring_enqueue(xr, g.kbd_data_phys[i], 8u,
                         TRB_TYPE(TRB_NORMAL) | TRB_IOC);
        }
        ring_doorbell((uint8_t)slot, (uint8_t)g.kbd_ep_dci);
        return 1;  /* keyboard ready */
    }

    /* ── Mass storage path (for log writing to USB drive) ── */
    if (found_msc && msc_bout_addr && msc_bin_addr && g.stor_slot < 0) {
        xlog("[xhci]   mass storage slot %u bout=%x bin=%x\n",
             (uint32_t)slot, (uint32_t)msc_bout_addr, (uint32_t)msc_bin_addr);
        if (!ctrl_xfer(slot, usb_setup(0x00, 9, cfg_val, 0, 0), 0, 0, false))
            return 0;

        uint8_t bout_dci = (uint8_t)((msc_bout_addr & 0x0Fu) * 2u);
        uint8_t bin_dci  = (uint8_t)((msc_bin_addr & 0x0Fu) * 2u + 1u);
        uint8_t max_dci  = (bout_dci > bin_dci) ? bout_dci : bin_dci;

        uint64_t iphys = pmm_alloc_pages(2);
        if (!iphys) return 0;
        void *ic = (void *)pmm_phys_to_virt(iphys);
        memset_x(ic, 0, 8192);

        ictx_icc(ic)->add_flags = (1u << 0) | (1u << bout_dci) | (1u << bin_dci);
        memcpy_x(ictx_slot(ic), dev_ctx_slot(g.dev_ctx[slot]),
                 sizeof(xhci_slot_ctx_t));
        ictx_slot(ic)->dw0 = (ictx_slot(ic)->dw0 & ~(0x1Fu << 27))
                            | ((uint32_t)max_dci << 27);

        /* Bulk OUT */
        g.stor_bout = ring_alloc(XFER_ENTRIES);
        ring_link(&g.stor_bout);
        xhci_ep_ctx_t *bout_ep = ictx_ep(ic, (uint8_t)(bout_dci - 1));
        bout_ep->dw0 = 0;
        bout_ep->dw1 = (3u << 1) | (2u << 3) | ((uint32_t)msc_bout_mps << 16);
        bout_ep->tr_dequeue_ptr = g.stor_bout.phys | 1u;
        bout_ep->dw4 = msc_bout_mps;

        /* Bulk IN */
        g.stor_bin = ring_alloc(XFER_ENTRIES);
        ring_link(&g.stor_bin);
        xhci_ep_ctx_t *bin_ep = ictx_ep(ic, (uint8_t)(bin_dci - 1));
        bin_ep->dw0 = 0;
        bin_ep->dw1 = (3u << 1) | (6u << 3) | ((uint32_t)msc_bin_mps << 16);
        bin_ep->tr_dequeue_ptr = g.stor_bin.phys | 1u;
        bin_ep->dw4 = msc_bin_mps;

        ring_enqueue(&g.cmd, iphys, 0, TRB_TYPE(TRB_CONFIG_EP) | TRB_SLOT(slot) | TRB_IOC);
        ring_doorbell(0, 0);
        uint32_t cc = 0;
        if (wait_event(TRB_EV_CMD, &cc, NULL, NULL) && cc == CC_SUCCESS) {
            g.stor_slot     = slot;
            g.stor_bout_dci = bout_dci;
            g.stor_bin_dci  = bin_dci;
            xlog("[xhci]   storage ready slot %u\n", (uint32_t)slot);
        }
        return 2;   /* mass storage, don't probe as hub */
    }

    return 0;  /* not a keyboard */
}

/* ── Write XHCI log buffer to USB drive via SCSI WRITE(10) ─────────────── */

#define LOG_LBA 16384u   /* sector 16384 = byte 8MB — well past 4.2MB ISO */

static void usb_storage_write_log(void) {
    if (g.stor_slot < 0 || xhci_log_pos == 0) {
        xlog("[xhci] log write skip: slot=%u logpos=%u\n",
             (uint32_t)(g.stor_slot & 0xFF), (uint32_t)xhci_log_pos);
        return;
    }

    uint8_t slot = (uint8_t)g.stor_slot;
    xlog("[xhci] log write: slot=%u bout_dci=%u bin_dci=%u bytes=%u\n",
         (uint32_t)slot, (uint32_t)g.stor_bout_dci,
         (uint32_t)g.stor_bin_dci, (uint32_t)xhci_log_pos);

    uint64_t cbw_phys = pmm_alloc_page();
    uint64_t dat_phys = pmm_alloc_page();
    uint64_t csw_phys = pmm_alloc_page();
    if (!cbw_phys || !dat_phys || !csw_phys) {
        xlog("[xhci] log write: alloc fail\n");
        return;
    }

    uint8_t *cbw = (uint8_t *)pmm_phys_to_virt(cbw_phys);
    uint8_t *dat = (uint8_t *)pmm_phys_to_virt(dat_phys);
    uint8_t *csw = (uint8_t *)pmm_phys_to_virt(csw_phys);
    memset_x(cbw, 0, 512);
    memset_x(dat, 0, 4096);
    memset_x(csw, 0, 512);

    /* Copy log to data buffer (zero-padded to 4096) */
    uint32_t n = xhci_log_pos < 4096 ? xhci_log_pos : 4095;
    memcpy_x(dat, xhci_log_buf, n);

    /* Build CBW for SCSI WRITE(10): 31 bytes */
    cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43; /* USBC */
    cbw[4] = 0x01; cbw[5] = 0x00; cbw[6] = 0x00; cbw[7] = 0x00; /* tag=1 */
    cbw[8] = 0x00; cbw[9] = 0x10; cbw[10] = 0x00; cbw[11] = 0x00; /* 4096 LE */
    cbw[12] = 0x00;     /* direction: OUT */
    cbw[13] = 0;        /* LUN 0 */
    cbw[14] = 10;       /* CDB length */
    /* SCSI WRITE(10): opcode 0x2A, LBA=16384=0x00004000 BE, xfer_len=8 BE */
    cbw[15] = 0x2A;
    cbw[16] = 0x00;     /* flags */
    cbw[17] = 0x00; cbw[18] = 0x00; cbw[19] = 0x40; cbw[20] = 0x00; /* LBA */
    cbw[21] = 0x00;     /* group */
    cbw[22] = 0x00; cbw[23] = 0x08; /* transfer length: 8 sectors */
    cbw[24] = 0x00;     /* control */

    uint32_t cc = 0;

    /* 1. Send CBW (31 bytes) on bulk OUT */
    ring_enqueue(&g.stor_bout, cbw_phys, 31, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    ring_doorbell(slot, (uint8_t)g.stor_bout_dci);
    if (!wait_event(TRB_EV_XFER, &cc, NULL, NULL)) {
        xlog("[xhci] log CBW: no event\n");
        return;
    }
    if (cc != CC_SUCCESS && cc != CC_SHORT_PKT) {
        xlog("[xhci] log CBW: cc=%u\n", (uint32_t)cc);
        return;
    }
    xlog("[xhci] log CBW: ok\n");

    /* 2. Send data (4096 bytes) on bulk OUT */
    ring_enqueue(&g.stor_bout, dat_phys, 4096, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    ring_doorbell(slot, (uint8_t)g.stor_bout_dci);
    if (!wait_event(TRB_EV_XFER, &cc, NULL, NULL)) {
        xlog("[xhci] log DATA: no event\n");
        return;
    }
    if (cc != CC_SUCCESS && cc != CC_SHORT_PKT) {
        xlog("[xhci] log DATA: cc=%u\n", (uint32_t)cc);
        return;
    }
    xlog("[xhci] log DATA: ok\n");

    /* 3. Read CSW (13 bytes) on bulk IN */
    ring_enqueue(&g.stor_bin, csw_phys, 13, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    ring_doorbell(slot, (uint8_t)g.stor_bin_dci);
    if (!wait_event(TRB_EV_XFER, &cc, NULL, NULL)) {
        xlog("[xhci] log CSW: no event\n");
        return;
    }
    xlog("[xhci] log CSW: cc=%u sig=%x st=%u\n",
         (uint32_t)cc,
         (uint32_t)(csw[0] | ((uint32_t)csw[1]<<8) | ((uint32_t)csw[2]<<16) | ((uint32_t)csw[3]<<24)),
         (uint32_t)csw[12]);

    if (csw[12] == 0)
        xlog("[xhci] log -> USB sector %u OK (%u bytes)\n",
             (uint32_t)LOG_LBA, (uint32_t)n);
    else
        xlog("[xhci] log write FAIL csw_status=%u\n", (uint32_t)csw[12]);
}

/* ── Enumerate a USB hub and search for keyboard on its ports ────────────── */

static bool enumerate_hub(uint8_t hub_slot, uint8_t root_port1, uint8_t hub_speed __attribute__((unused))) {
    /* SET_CONFIGURATION 1 */
    if (!ctrl_xfer(hub_slot, usb_setup(0x00, 9, 1, 0, 0), 0, 0, false)) {
        xlog("[xhci] hub SET_CONFIG failed\n");
        return false;
    }

    /* GET_DESCRIPTOR: Hub descriptor (type 0x29) */
    uint64_t buf_phys = pmm_alloc_page();
    if (!buf_phys) return false;
    uint8_t *buf = (uint8_t *)pmm_phys_to_virt(buf_phys);
    memset_x(buf, 0, 64);

    if (!ctrl_xfer(hub_slot, usb_setup(0xA0, 6, 0x2900, 0, 8), buf_phys, 8, true)) {
        xlog("[xhci] hub GET_DESCRIPTOR failed\n");
        return false;
    }
    usb_hub_desc_t *hd = (usb_hub_desc_t *)buf;
    uint8_t nports     = hd->bNbrPorts;
    uint32_t pwr_delay = (uint32_t)hd->bPwrOn2PwrGood * 2u + 50u; /* ms */
    if (nports == 0 || nports > 15) nports = 8;
    if (pwr_delay < 50)  pwr_delay = 50;
    if (pwr_delay > 200) pwr_delay = 200;  /* cap at 200ms */

    xlog("[xhci] hub slot=%u ports=%u pwr=%ums\n",
         (uint32_t)hub_slot, (uint32_t)nports, (uint32_t)pwr_delay);

    /* Power each downstream port */
    for (uint8_t p = 1; p <= nports; p++) {
        ctrl_xfer(hub_slot, usb_setup(0x23, HUB_SET_FEATURE, HUB_PORT_POWER, p, 0),
                  0, 0, false);
    }
    udelay(pwr_delay * 1000u);

    /* Check each port and try to find keyboard */
    for (uint8_t p = 1; p <= nports; p++) {
        /* GET_STATUS for this port */
        memset_x(buf, 0, 8);
        if (!ctrl_xfer(hub_slot,
                       usb_setup(0xA3, HUB_GET_STATUS, 0, p, 4),
                       buf_phys, 4, true))
            continue;

        uint32_t port_status = *(uint32_t *)buf;
        if (!(port_status & 1u)) continue;  /* CCS = bit 0 */

        uint8_t pspeed = (port_status & (1u << 9)) ? 2u  /* LS */
                       : (port_status & (1u << 10)) ? 3u /* HS */
                       : 1u;                             /* FS */

        xlog("[xhci] hub port %u: connected spd=%u\n", (uint32_t)p, (uint32_t)pspeed);

        /* Reset the hub port */
        ctrl_xfer(hub_slot, usb_setup(0x23, HUB_SET_FEATURE, HUB_PORT_RESET, p, 0),
                  0, 0, false);
        udelay(30000);  /* 30ms reset */

        /* Clear C_PORT_RESET */
        ctrl_xfer(hub_slot, usb_setup(0x23, HUB_CLEAR_FEATURE, HUB_C_PORT_RESET, p, 0),
                  0, 0, false);
        udelay(3000);

        /* Enable a slot for this device */
        uint8_t slot = 0;
        if (!cmd_enable_slot(&slot)) {
            xlog("[xhci] hub port %u: enable_slot failed\n", (uint32_t)p);
            continue;
        }

        /* Route string: tier-1 = hub port number in bits 3:0 */
        uint32_t route = p & 0xFu;

        int r = try_enumerate_kbd(slot, root_port1, pspeed,
                                  route, hub_slot, p);
        if (r == 1) {
            xlog("[xhci] keyboard on hub slot=%u port=%u\n",
                 (uint32_t)hub_slot, (uint32_t)p);
            g.kbd_slot = slot;
            return true;
        }
    }
    return false;
}

/* ── Root port reset ─────────────────────────────────────────────────────── */
#define PLS(sc)  (((sc) >> 5) & 0xFu)

/* Issue a single port reset (PR or WPR) and wait for PED.
 * Returns true if PED set. Logs postPR state for diagnostics. */
static bool do_one_reset(uint8_t port0, bool warm) {
    uint32_t sc = mmio_r32(g.op, OP_PORTSC(port0));
    if (!(sc & PORTSC_CCS)) return false;

    /* Drain any pending events first — prevents ring backup */
    drain_events();

    /* Clear change bits first */
    if (sc & PORTSC_W1C_BITS)
        mmio_w32(g.op, OP_PORTSC(port0),
                 (sc & ~PORTSC_RW1_DANGER) | PORTSC_W1C_BITS);
    udelay(2000);

    /* Issue the reset — use Linux-style neutral mask: clear ALL non-RO non-RWS bits */
    sc = mmio_r32(g.op, OP_PORTSC(port0));
    uint32_t rst_bit = warm ? (1u << 31) : PORTSC_PR;
    /* Neutral: preserve RO (CCS,OCA,Speed,DR) + RWS (PLS,PP,PIC,WCE,WDE,WOE) */
    uint32_t neutral = sc & ~(PORTSC_PED | PORTSC_PR | (1u<<16)/*LWS*/ |
                              PORTSC_W1C_BITS | (1u<<24)/*CAS*/ | (1u<<31)/*WPR*/);
    mmio_w32(g.op, OP_PORTSC(port0), neutral | rst_bit);

    /* Verify reset was asserted */
    udelay(1000);
    sc = mmio_r32(g.op, OP_PORTSC(port0));
    if (warm) { if (!(sc & (1u << 31))) xlog("[xhci] P%u: WPR didn't stick!\n", (uint32_t)(port0+1)); }
    else      { if (!(sc & PORTSC_PR))  xlog("[xhci] P%u: PR didn't stick!\n",  (uint32_t)(port0+1)); }

    /* Wait up to 500ms for reset to complete, draining events periodically */
    for (int i = 0; i < 250; i++) {
        udelay(2000);
        drain_events();  /* process Port Status Change Events */
        sc = mmio_r32(g.op, OP_PORTSC(port0));
        if (warm) { if (!(sc & (1u << 31))) break; }
        else      { if (!(sc & PORTSC_PR))  break; }
    }

    /* USB spec: 10ms recovery time after reset */
    udelay(10000);
    drain_events();

    sc = mmio_r32(g.op, OP_PORTSC(port0));
    uint32_t prc = (sc >> 21) & 1;
    xlog("[xhci] P%u %s: sc=0x%x pls=%u ped=%u prc=%u spd=%u\n",
         (uint32_t)(port0 + 1), warm ? "WPR" : "PR",
         (uint32_t)sc, (uint32_t)PLS(sc),
         (uint32_t)((sc >> 1) & 1), (uint32_t)prc,
         (uint32_t)PORTSC_SPEED(sc));

    /* Clear PRC if set (acknowledge the reset) */
    if (prc) {
        mmio_w32(g.op, OP_PORTSC(port0),
                 (sc & ~PORTSC_RW1_DANGER) | PORTSC_PRC);
        udelay(2000);
        sc = mmio_r32(g.op, OP_PORTSC(port0));
    }

    if (sc & PORTSC_PED) return true;

    /* USB2 LWS quirk: force link state to U0 */
    bool is_usb2 = !!(g.usb2_ports & (1u << port0));
    if (is_usb2 && !warm) {
        sc = mmio_r32(g.op, OP_PORTSC(port0));
        uint32_t val = sc & ~(PORTSC_RW1_DANGER | (0xFu << 5) | (1u<<16));
        val |= (1u << 16);  /* LWS = 1 */
        mmio_w32(g.op, OP_PORTSC(port0), val);
        udelay(10000);
        sc = mmio_r32(g.op, OP_PORTSC(port0));
        if (sc & PORTSC_PED) {
            xlog("[xhci] P%u LWS→U0 worked\n", (uint32_t)(port0+1));
            return true;
        }
    }

    /* Final 200ms wait for PED */
    for (int i = 0; i < 40; i++) {
        drain_events();
        sc = mmio_r32(g.op, OP_PORTSC(port0));
        if (!(sc & PORTSC_CCS)) return false;
        if (sc & PORTSC_PED) return true;
        udelay(5000);
    }
    return false;
}

static bool root_port_reset(uint8_t port0, uint8_t *speed_out) {
    uint32_t sc = mmio_r32(g.op, OP_PORTSC(port0));
    bool is_usb2 = !!(g.usb2_ports & (1u << port0));
    bool is_usb3 = !!(g.usb3_ports & (1u << port0));

    if (!(sc & PORTSC_CCS)) return false;

    /* USB3: if already enabled (link trained), use it directly.
     * USB2: ALWAYS do a proper port reset — USB2 devices need a reset
     * signal on the data lines to enter Default state for SET_ADDRESS. */
    if (!is_usb2 && (sc & PORTSC_PED) && PLS(sc) == 0) {
        *speed_out = (uint8_t)PORTSC_SPEED(sc);
        xlog("[xhci] P%u: already enabled spd=%u\n",
             (uint32_t)(port0 + 1), (uint32_t)*speed_out);
        return true;
    }
    if (is_usb2 && (sc & PORTSC_PED))
        xlog("[xhci] P%u: USB2 PED set, forcing reset\n",
             (uint32_t)(port0 + 1));

    /* Try regular port reset up to 3 times */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (do_one_reset(port0, false)) goto done;
        udelay(50000); /* 50ms between attempts */
        sc = mmio_r32(g.op, OP_PORTSC(port0));
        if (!(sc & PORTSC_CCS)) return false;
    }

    /* USB3 ports: try warm port reset if regular reset failed */
    if (is_usb3 && !is_usb2) {
        xlog("[xhci] P%u: trying warm reset\n", (uint32_t)(port0 + 1));
        if (do_one_reset(port0, true)) goto done;
    }

    sc = mmio_r32(g.op, OP_PORTSC(port0));
    xlog("[xhci] P%u: FAIL sc=0x%x\n",
         (uint32_t)(port0 + 1), (uint32_t)sc);
    return false;

done:
    sc = mmio_r32(g.op, OP_PORTSC(port0));
    /* Clear remaining change bits */
    if (sc & PORTSC_W1C_BITS)
        mmio_w32(g.op, OP_PORTSC(port0),
                 (sc & ~PORTSC_RW1_DANGER) | PORTSC_W1C_BITS);
    sc = mmio_r32(g.op, OP_PORTSC(port0));
    *speed_out = (uint8_t)PORTSC_SPEED(sc);
    xlog("[xhci] P%u: OK spd=%u sc=0x%x\n",
         (uint32_t)(port0 + 1), (uint32_t)*speed_out, (uint32_t)sc);
    return true;
}

/* ── HID boot-protocol key translation ───────────────────────────────────── */

static const uint8_t hid2asc[256] = {
    /* 0x00 */ 0,0,0,0,
    /* 0x04 a..z */ 'a','b','c','d','e','f','g','h','i','j','k','l','m',
                    'n','o','p','q','r','s','t','u','v','w','x','y','z',
    /* 0x1E 1..0 */ '1','2','3','4','5','6','7','8','9','0',
    /* 0x28 */'\n', 27, '\b', '\t', ' ',
    /* 0x2D */'-','=','[',']','\\',0,';','\'','`',',','.','/',
    /* 0x39 Caps */ 0,
    /* 0x3A-0x45 F1-F12 */ 0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x46-0x49 */ 0,0,0,0,
    /* 0x4A */ KEY_HOME,
    /* 0x4B */ 0,  /* Page Up — not handled */
    /* 0x4C */ KEY_DELETE,
    /* 0x4D */ KEY_END,
    /* 0x4E */ 0,  /* Page Down — not handled */
    /* 0x4F */ KEY_RIGHT,
    /* 0x50 */ KEY_LEFT,
    /* 0x51 */ KEY_DOWN,
    /* 0x52 */ KEY_UP,
};

static const uint8_t hid2asc_sh[256] = {
    0,0,0,0,
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    '\n', 27, '\b', '\t', ' ',
    '_','+','{','}','|',0,':','"','~','<','>','?',
    0,
    0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0, KEY_HOME, 0, KEY_DELETE, KEY_END, 0,
    KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
};

static void process_hid_report(const uint8_t *rep) {
    uint8_t mods  = rep[0];
    bool    shift = (mods & 0x22u) != 0;
    bool    ctrl  = (mods & 0x11u) != 0;

    for (int i = 2; i < 8; i++) {
        uint8_t kc = rep[i];
        if (!kc || kc == 1) continue;
        bool was_down = false;
        for (int j = 0; j < 6; j++) { if (g.last_keys[j] == kc) { was_down = true; break; } }
        if (was_down) continue;

        uint8_t ch = shift ? hid2asc_sh[kc] : hid2asc[kc];
        if (!ch) continue;

        if (ch >= 0x80) {
            /* Special key: inject via PS/2 extended scancode path */
            uint8_t ext = 0;
            switch (ch) {
                case KEY_LEFT:   ext = 0x4Bu; break;
                case KEY_RIGHT:  ext = 0x4Du; break;
                case KEY_UP:     ext = 0x48u; break;
                case KEY_DOWN:   ext = 0x50u; break;
                case KEY_DELETE: ext = 0x53u; break;
                case KEY_HOME:   ext = 0x47u; break;
                case KEY_END:    ext = 0x4Fu; break;
            }
            if (ext) { keyboard_on_scancode(0xE0); keyboard_on_scancode(ext); }
        } else if (ctrl && ch >= 'a' && ch <= 'z') {
            keyboard_push_char((uint8_t)(ch - 'a' + 1));
        } else {
            keyboard_push_char(ch);
        }
    }
    for (int i = 0; i < 6; i++) g.last_keys[i] = rep[i + 2];
}

/* ── Set up DCBAA, command ring, event ring, scratchpad ─────────────────── */

static bool setup_data_structures(void) {
    mmio_w32(g.op, OP_CONFIG, g.max_slots);

    /* DCBAA */
    g.dcbaa_phys = pmm_alloc_page();
    g.dcbaa      = (uint64_t *)pmm_phys_to_virt(g.dcbaa_phys);
    memset_x(g.dcbaa, 0, 4096);

    /* Scratchpad buffers */
    uint32_t hcs2 = mmio_r32(g.cap, CAP_HCSPARAMS2);
    uint32_t nsp  = ((hcs2 >> 27) & 0x1Fu) | (((hcs2 >> 21) & 0x1Fu) << 5);
    if (nsp > 0) {
        uint64_t sp_phys = pmm_alloc_page();
        uint64_t *sp_arr = (uint64_t *)pmm_phys_to_virt(sp_phys);
        memset_x(sp_arr, 0, 4096);
        for (uint32_t i = 0; i < nsp && i < 512u; i++)
            sp_arr[i] = pmm_alloc_page();
        g.dcbaa[0] = sp_phys;
    }
    mmio_w64(g.op, OP_DCBAAP, g.dcbaa_phys);

    /* Command ring */
    g.cmd = ring_alloc(RING_ENTRIES);
    ring_link(&g.cmd);
    mmio_w64(g.op, OP_CRCR, g.cmd.phys | 1u);

    /* Event ring + ERST */
    g.evt       = ring_alloc(RING_ENTRIES);
    g.evt.cycle = 1;
    g.erst_phys = pmm_alloc_page();
    g.erst      = (xhci_erst_t *)pmm_phys_to_virt(g.erst_phys);
    memset_x(g.erst, 0, 4096);
    g.erst[0].base = g.evt.phys;
    g.erst[0].size = (uint16_t)g.evt.size;

    uint64_t ir0 = g.rt + 0x20u;
    mmio_w32(ir0, IR_ERSTSZ, 1u);
    mmio_w64(ir0, IR_ERSTBA, g.erst_phys);
    mmio_w64(ir0, IR_ERDP,   g.evt.phys | (1u << 3));
    return true;
}

/* ── Enumerate a single port: reset, enable slot, probe device ──────────── */

static bool try_port(uint8_t p) {
    uint32_t sc = mmio_r32(g.op, OP_PORTSC(p));
    if (!(sc & PORTSC_CCS)) return false;

    const char *ptype = (g.usb2_ports & (1u << p)) ? "USB2"
                      : (g.usb3_ports & (1u << p)) ? "USB3" : "?";
    xlog("[xhci] try port %u (%s) sc=0x%x\n",
         (uint32_t)(p + 1), ptype, (uint32_t)sc);

    uint8_t speed = 0;
    if (!root_port_reset(p, &speed)) return false;

    xlog("[xhci] port %u: spd=%u\n",
         (uint32_t)(p + 1), (uint32_t)speed);

    /* Drain stale events before command — prevents event ring desync */
    drain_events();

    uint8_t slot = 0;
    if (!cmd_enable_slot(&slot)) {
        xlog("[xhci] port %u: enable_slot fail\n", (uint32_t)(p + 1));
        return false;
    }

    int r = try_enumerate_kbd(slot, (uint8_t)(p + 1), speed, 0, 0, 0);
    if (r == 1) {
        xlog("[xhci] keyboard slot=%u port=%u\n",
             (uint32_t)slot, (uint32_t)(p + 1));
        g.kbd_slot = slot;
        return true;
    }
    if (r == 2) return false;   /* mass storage — already saved, move on */

    /* Check for hub */
    uint64_t bp = pmm_alloc_page();
    uint8_t *bb = (uint8_t *)pmm_phys_to_virt(bp);
    memset_x(bb, 0, 64);
    if (ctrl_xfer(slot, usb_setup(0x80, 6, 0x0100, 0, 8), bp, 8, true)) {
        usb_dev_desc_t *dd = (usb_dev_desc_t *)bb;
        if (dd->bDeviceClass == 0x09) {
            xlog("[xhci] port %u: hub\n", (uint32_t)(p + 1));
            if (enumerate_hub(slot, (uint8_t)(p + 1), speed))
                return true;
        }
    }
    return false;
}

/* ── Enumerate all connected ports (USB2 first, then USB3) ─────────────── */

static void enumerate_ports(void) {
    /* Pass 1: Try ALL USB2 ports — don't stop at first keyboard.
     * Lenovo Legion has two ITE USB controllers (C994 port 7, C992 port 12)
     * both report as boot keyboards but only one generates keystrokes.
     * Let the last keyboard found win. */
    for (uint8_t p = 0; p < g.max_ports && p < 32; p++) {
        if (!(g.usb2_ports & (1u << p))) continue;
        try_port(p);
    }
    /* Then USB3 ports (stop if keyboard already found) */
    for (uint8_t p = 0; p < g.max_ports && p < 32; p++) {
        if (g.kbd_slot >= 0) return;
        if (!(g.usb3_ports & (1u << p))) continue;
        if (try_port(p)) return;
    }
    /* Any remaining unclassified ports */
    for (uint8_t p = 0; p < g.max_ports && p < 32; p++) {
        if (g.kbd_slot >= 0) return;
        if ((g.usb2_ports | g.usb3_ports) & (1u << p)) continue;
        if (try_port(p)) return;
    }

    /* Pass 2: Power-cycle all connected-but-not-enabled ports, then retry.
     * Some controllers need a full PP cycle after HCRST to bring links up. */
    if (g.kbd_slot >= 0) return;
    xlog("[xhci] pass 1 failed, power-cycling ports\n");
    for (uint8_t p = 0; p < g.max_ports && p < 32; p++) {
        uint32_t sc = mmio_r32(g.op, OP_PORTSC(p));
        if (!(sc & PORTSC_CCS)) continue;
        if (sc & PORTSC_PED) continue;
        /* Turn off port power */
        mmio_w32(g.op, OP_PORTSC(p), (sc & ~PORTSC_RW1_DANGER) & ~PORTSC_PP);
    }
    udelay(100000); /* 100ms with power off */
    for (uint8_t p = 0; p < g.max_ports && p < 32; p++) {
        uint32_t sc = mmio_r32(g.op, OP_PORTSC(p));
        if (!(sc & PORTSC_PP))
            mmio_w32(g.op, OP_PORTSC(p), (sc & ~PORTSC_RW1_DANGER) | PORTSC_PP);
    }
    udelay(500000); /* 500ms for re-detect after power-on */

    /* Retry USB2 first */
    for (uint8_t p = 0; p < g.max_ports && p < 32; p++) {
        if (g.kbd_slot >= 0) return;
        if (!(g.usb2_ports & (1u << p))) continue;
        if (try_port(p)) return;
    }
    for (uint8_t p = 0; p < g.max_ports && p < 32; p++) {
        if (g.kbd_slot >= 0) return;
        if (g.usb2_ports & (1u << p)) continue;
        if (try_port(p)) return;
    }
}

/* ── Enumerate ports that are ALREADY enabled (no port reset) ──────────── */
/* Used in soft-restart path: BIOS left ports enabled, we just need to
 * allocate slots and address devices. No PR needed. */

/* ── Per-controller init (called for each XHCI PCI device found) ─────────── */

static bool xhci_init_one(uint8_t bus, uint8_t dev, uint8_t fn) {
    xlog("[xhci] ctrl %u:%u.%u\n", (uint32_t)bus, (uint32_t)dev, (uint32_t)fn);

    uint64_t mmio_phys = pci_bar_base64(bus, dev, fn, 0);
    if (!mmio_phys) { xlog("[xhci] bad BAR0\n"); return false; }

    /* Each controller gets its own 64KB MMIO window */
    static uint64_t mmio_virt_next = 0xFFFFFF0040000000ULL;
    uint64_t mmio_virt = mmio_virt_next;
    mmio_virt_next += 0x10000ULL;
    if (!vmm_map_range(mmio_virt, mmio_phys, 0x10000, VMM_WRITE | VMM_UNCACHE)) {
        xlog("[xhci] MMIO map failed\n"); return false;
    }
    pci_enable(bus, dev, fn);

    g.cap = mmio_virt;
    uint8_t cap_len = (uint8_t)mmio_r32(g.cap, CAP_CAPLENGTH);
    g.op  = g.cap + cap_len;
    g.db  = g.cap + (mmio_r32(g.cap, CAP_DBOFF) & ~3u);
    g.rt  = g.cap + (mmio_r32(g.cap, CAP_RTSOFF) & ~0x1Fu);

    uint32_t hcs1 = mmio_r32(g.cap, CAP_HCSPARAMS1);
    g.max_slots   = (uint8_t)((hcs1) & 0xFFu);
    g.max_ports   = (uint8_t)(hcs1 >> 24);
    if (g.max_slots > MAX_SLOTS) g.max_slots = (uint8_t)MAX_SLOTS;

    uint32_t hcc1 = mmio_r32(g.cap, CAP_HCCPARAMS1);
    uint8_t  csz  = (uint8_t)((hcc1 >> 2) & 1u);  /* Context Size: 0=32B, 1=64B */
    ctx_bytes = csz ? 64u : 32u;
    xlog("[xhci] BAR=0x%x slots=%u ports=%u CSZ=%u HCC=0x%x\n",
         (uint32_t)(mmio_phys & 0xFFFFFFFFu),
         (uint32_t)g.max_slots, (uint32_t)g.max_ports,
         (uint32_t)csz, (uint32_t)hcc1);

    /* ── Snapshot BIOS port states (before we touch anything) ──────────── */
    xlog("[xhci] BIOS STS=0x%x CMD=0x%x\n",
         mmio_r32(g.op, OP_USBSTS), mmio_r32(g.op, OP_USBCMD));
    for (uint8_t p = 0; p < g.max_ports && p < 16; p++) {
        uint32_t sc = mmio_r32(g.op, OP_PORTSC(p));
        if (sc & PORTSC_CCS)   /* only log connected ports */
            xlog("[xhci] BIOS port%u: 0x%x pls=%u ped=%u spd=%u\n",
                 (uint32_t)(p + 1), (uint32_t)sc, (uint32_t)PLS(sc),
                 (uint32_t)((sc >> 1) & 1), (uint32_t)PORTSC_SPEED(sc));
    }

    /* ── Parse port protocols (USB2 vs USB3) ────────────────────────── */
    xhci_parse_protocols();

    /* ── BIOS handoff then hard reset ────────────────────────────────── */
    /* Soft takeover was tried in earlier versions but enable_slot always
     * timed out — the command ring never processed.  Go straight to HCRST. */
    xhci_bios_handoff();

    /* ── Hard reset (HCRST) ─────────────────────────────────────────── */
    xlog("[xhci] hard reset (HCRST)\n");
    g.present = false;
    g.kbd_slot  = -1;
    g.stor_slot = -1;

    if (!ctrl_reset()) { xlog("[xhci] HCRST timed out\n"); return false; }
    xlog("[xhci] HCRST OK\n");

    /* AMD PLL stabilization: wait 200ms after HCRST before touching ports */
    udelay(200000);

    setup_data_structures();

    if (!ctrl_start()) { xlog("[xhci] hard start failed\n"); return false; }
    g.present = true;

    /* Check for controller errors after start */
    {
        uint32_t sts = mmio_r32(g.op, OP_USBSTS);
        uint32_t cmd2 = mmio_r32(g.op, OP_USBCMD);
        xlog("[xhci] post-start CMD=0x%x STS=0x%x\n",
             (uint32_t)cmd2, (uint32_t)sts);
        if (sts & USBSTS_HSE) xlog("[xhci] WARNING: Host System Error!\n");
        if (sts & USBSTS_HCE) xlog("[xhci] WARNING: Host Controller Error!\n");
    }

    /* ── Verify command ring with a No-Op command ────────────────────── */
    {
        xlog("[xhci] NOOP test: cmd.phys=0x%x cmd.enq=%u cmd.cycle=%u\n",
             (uint32_t)(g.cmd.phys & 0xFFFFFFFFu),
             g.cmd.enq, (uint32_t)g.cmd.cycle);
        ring_enqueue(&g.cmd, 0, 0, TRB_TYPE(TRB_NOOP_CMD) | TRB_IOC);
        /* Verify TRB was written to memory */
        xhci_trb_t *cmd_trb = &g.cmd.trbs[0];
        xlog("[xhci] cmd[0] written: c=0x%x p=0x%x\n",
             cmd_trb->control, (uint32_t)(cmd_trb->param & 0xFFFFFFFFu));
        ring_doorbell(0, 0);
        uint32_t noop_cc = 0;
        if (!wait_event(TRB_EV_CMD, &noop_cc, NULL, NULL)) {
            xlog("[xhci] NOOP TIMEOUT — cmd ring broken!\n");
            /* Dump interrupter state */
            uint64_t ir0 = g.rt + 0x20u;
            xlog("[xhci] IMAN=0x%x ERDP_lo=0x%x\n",
                 mmio_r32(ir0, IR_IMAN), mmio_r32(ir0, IR_ERDP));
            xlog("[xhci] evt.deq=%u cycle=%u phys=0x%x\n",
                 g.evt.deq, (uint32_t)g.evt.cycle,
                 (uint32_t)(g.evt.phys & 0xFFFFFFFFu));
            /* Dump first 4 evt TRBs */
            for (int d = 0; d < 4; d++)
                xlog("[xhci] evt[%d]: c=0x%x\n",
                     d, g.evt.trbs[d].control);
            /* Dump CRCR readback (only valid when halted, but try anyway) */
            xlog("[xhci] CRCR_lo=0x%x DCBAAP_lo=0x%x\n",
                 mmio_r32(g.op, 0x18), mmio_r32(g.op, 0x30));
        } else {
            xlog("[xhci] NOOP OK cc=%u — cmd ring works!\n", noop_cc);
        }
    }

    /* Power all ports and wait for devices to appear */
    for (uint8_t p = 0; p < g.max_ports; p++) {
        uint32_t sc2 = mmio_r32(g.op, OP_PORTSC(p));
        if (!(sc2 & PORTSC_PP))
            mmio_w32(g.op, OP_PORTSC(p), (sc2 & ~PORTSC_RW1_DANGER) | PORTSC_PP);
    }
    udelay(500000);  /* 500ms for port power settle */

    /* Drain any Port Status Change events generated during power-up */
    {
        uint32_t nevt = drain_events();
        if (nevt) xlog("[xhci] drained %u events after power-up\n", nevt);
    }

    /* Check USBSTS PCD (port change detect) */
    {
        uint32_t sts = mmio_r32(g.op, OP_USBSTS);
        if (sts & USBSTS_PCD) {
            xlog("[xhci] PCD set after power — good\n");
            /* Clear PCD by writing 1 */
            mmio_w32(g.op, OP_USBSTS, USBSTS_PCD);
        } else {
            xlog("[xhci] WARNING: no PCD after 500ms power wait\n");
        }
    }

    /* Log ALL port states after power-up — critical diagnostic */
    {
        uint32_t n_ccs = 0;
        for (uint8_t p2 = 0; p2 < g.max_ports && p2 < 32; p2++) {
            uint32_t sc2 = mmio_r32(g.op, OP_PORTSC(p2));
            if (sc2 & PORTSC_CCS) {
                n_ccs++;
                const char *pt = (g.usb2_ports & (1u << p2)) ? "2"
                               : (g.usb3_ports & (1u << p2)) ? "3" : "?";
                xlog("[xhci] P%u(%s): 0x%x pls=%u ped=%u spd=%u pp=%u\n",
                     (uint32_t)(p2+1), pt, (uint32_t)sc2,
                     (uint32_t)PLS(sc2), (uint32_t)((sc2>>1)&1),
                     (uint32_t)PORTSC_SPEED(sc2), (uint32_t)((sc2>>9)&1));
            }
        }
        xlog("[xhci] hard: %u/%u ports with CCS\n", (uint32_t)n_ccs, (uint32_t)g.max_ports);
    }

    enumerate_ports();

    if (g.kbd_slot < 0)
        xlog("[xhci] no kbd on this controller\n");

    /* Write log to USB while THIS controller is still alive */
    if (g.stor_slot >= 0)
        usb_storage_write_log();

    return (g.kbd_slot >= 0);
}

/* ── xhci_init ────────────────────────────────────────────────────────────── */

void xhci_init(void) {
    memset_x(&g, 0, sizeof(g));
    g.kbd_slot  = -1;
    g.stor_slot = -1;
    xhci_log_pos = 0;
    xhci_log_buf[0] = 0;

    uint8_t all_bus[8], all_dev[8], all_fn[8];
    uint32_t n = pci_find_all_class(0x0C, 0x03, 0x30,
                                     all_bus, all_dev, all_fn, 8);
    if (n == 0) {
        kprintf("[xhci] no XHCI controller found\n");
        return;
    }
    xlog("[xhci] found %u controller(s)\n", (uint32_t)n);

    for (uint32_t i = 0; i < n; i++) {
        memset_x(&g, 0, sizeof(g));
        g.kbd_slot  = -1;
        g.stor_slot = -1;
        if (xhci_init_one(all_bus[i], all_dev[i], all_fn[i]))
            break;   /* keyboard found */
    }
    if (g.kbd_slot >= 0) {
        kprintf("[xhci] USB keyboard ready (slot %d)\n", g.kbd_slot);
    } else {
        kprintf("[xhci] no USB keyboard found — log follows:\n");
        if (xhci_log_pos > 0)
            kprintf("%s", xhci_log_buf);
    }
}

/* ── xhci_poll ─ called from pit_on_tick() ───────────────────────────────── */

void xhci_poll(void) {
    if (!g.present) return;

    if (g.kbd_slot < 0) {
        /* Hotplug detection: check every 100 ticks (1 s at 100 Hz) for a
         * newly-connected port and attempt enumeration.  Covers the case
         * where the keyboard was plugged in after xhci_init ran.
         * wait_event() is pure MMIO polling so this is safe in IRQ context;
         * the stall while enumerating (~1-2 s) is acceptable because the
         * keyboard is non-functional anyway.                                */
        static uint32_t s_hp_ticks = 0;
        static uint32_t s_hp_prev  = 0;   /* CCS bitmask seen last check */
        if (++s_hp_ticks < 100u) return;
        s_hp_ticks = 0;

        uint32_t cur = 0;
        for (uint8_t p = 0; p < g.max_ports && p < 32; p++)
            if (mmio_r32(g.op, OP_PORTSC(p)) & PORTSC_CCS)
                cur |= (1u << p);

        uint32_t newly = cur & ~s_hp_prev;
        s_hp_prev = cur;
        if (!newly) return;   /* nothing new since last check */

        kprintf("[xhci] hotplug: new connection(s) detected, enumerating\n");
        drain_events();
        enumerate_ports();
        if (g.kbd_slot >= 0)
            kprintf("[xhci] USB keyboard ready (slot %d)\n", g.kbd_slot);
        return;
    }

    for (int limit = 0; limit < 16; limit++) {
        xhci_trb_t *ev = &g.evt.trbs[g.evt.deq];
        if ((ev->control & 1u) != (uint32_t)g.evt.cycle) break;

        uint8_t type = (uint8_t)((ev->control >> 10) & 0x3Fu);
        if (type == TRB_EV_XFER && (uint8_t)(ev->control >> 24) == (uint8_t)g.kbd_slot) {
            uint64_t trb_phys = ev->param;
            xhci_trb_t *src_trb = (xhci_trb_t *)pmm_phys_to_virt(trb_phys);
            uint64_t data_phys = src_trb->param;
            for (uint32_t i = 0; i < KBD_BUFS; i++) {
                if (data_phys == g.kbd_data_phys[i]) {
                    process_hid_report(g.kbd_data[i]);
                    ring_enqueue(&g.xfer[g.kbd_slot], g.kbd_data_phys[i], 8u,
                                 TRB_TYPE(TRB_NORMAL) | TRB_IOC);
                    ring_doorbell((uint8_t)g.kbd_slot, (uint8_t)g.kbd_ep_dci);
                    break;
                }
            }
        }

        g.evt.deq++;
        if (g.evt.deq >= g.evt.size) { g.evt.deq = 0; g.evt.cycle ^= 1; }
        mmio_w64(g.rt + 0x20u, IR_ERDP,
                 g.evt.phys + g.evt.deq * 16u | (1u << 3));
    }
}
