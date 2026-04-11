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
    *(volatile uint64_t *)(uintptr_t)(base + off) = val;
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
#define USBSTS_HCH      (1u << 0)
#define USBSTS_CNR      (1u << 11)

#define PORTSC_CCS      (1u << 0)
#define PORTSC_PED      (1u << 1)
#define PORTSC_PR       (1u << 4)
#define PORTSC_PP       (1u << 9)
#define PORTSC_CSC      (1u << 17)
#define PORTSC_PEC      (1u << 18)
#define PORTSC_PRC      (1u << 21)
#define PORTSC_WRC      (1u << 19)
#define PORTSC_W1C_BITS (PORTSC_CSC | PORTSC_PEC | PORTSC_PRC | PORTSC_WRC | \
                         (1u<<22) | (1u<<23) | (1u<<24))
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
#define TRB_ENABLE_SLOT 9u
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

typedef struct {
    uint32_t dw0, dw1, dw2, dw3;
    uint32_t rsvd[4];
} __attribute__((packed)) xhci_slot_ctx_t;

typedef struct {
    uint32_t dw0, dw1;
    uint64_t tr_dequeue_ptr;
    uint32_t dw4;
    uint32_t rsvd[3];
} __attribute__((packed)) xhci_ep_ctx_t;

typedef struct {
    xhci_slot_ctx_t slot;
    xhci_ep_ctx_t   ep[31];
} __attribute__((packed)) xhci_dev_ctx_t;

typedef struct {
    uint32_t drop_flags, add_flags;
    uint32_t rsvd[6];
} __attribute__((packed)) xhci_icc_t;

typedef struct {
    xhci_icc_t     icc;
    xhci_dev_ctx_t dev;
} __attribute__((packed)) xhci_input_ctx_t;

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

    xhci_dev_ctx_t *dev_ctx[MAX_SLOTS + 1];
    uint64_t        dev_ctx_phys[MAX_SLOTS + 1];
    ring_t          xfer[MAX_SLOTS + 1];

    int      kbd_slot;
    int      kbd_ep_dci;
    uint8_t  last_keys[6];

    uint8_t  *kbd_data[KBD_BUFS];
    uint64_t  kbd_data_phys[KBD_BUFS];
} g;

/* ── Doorbell ────────────────────────────────────────────────────────────── */

static void ring_doorbell(uint8_t slot, uint8_t dci) {
    mmio_w32(g.db, (uint32_t)slot * 4u, dci);
}

/* ── Event ring: wait for one event of a given type ─────────────────────── */

static bool wait_event(uint8_t want_type, uint32_t *cc, uint64_t *param, uint8_t *slot) {
    for (int i = 0; i < 100000; i++) {
        xhci_trb_t *ev = &g.evt.trbs[g.evt.deq];
        if ((ev->control & 1u) != (uint32_t)g.evt.cycle) { udelay(10); continue; }

        uint8_t type = (uint8_t)((ev->control >> 10) & 0x3Fu);
        bool match = (want_type == 0 || type == want_type);

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
        ring_enqueue(r, data_phys, len, TRB_TYPE(TRB_DATA) | dir | TRB_IOC);
    }
    ring_enqueue(r, 0, 0,
                 TRB_TYPE(TRB_STATUS) | ((len > 0 && in) ? 0u : TRB_DIR_IN) | TRB_IOC);
    ring_doorbell((uint8_t)slot, 1);

    uint32_t cc = 0;
    if (!wait_event(TRB_EV_XFER, &cc, NULL, NULL)) return false;
    return (cc == CC_SUCCESS || cc == CC_SHORT_PKT);
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

static bool cmd_enable_slot(uint8_t *slot_out) {
    ring_enqueue(&g.cmd, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
    ring_doorbell(0, 0);
    uint32_t cc = 0; uint8_t slot = 0;
    if (!wait_event(TRB_EV_CMD, &cc, NULL, &slot)) return false;
    if (cc != CC_SUCCESS) return false;
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
    g.dev_ctx[slot]      = (xhci_dev_ctx_t *)pmm_phys_to_virt(phys);
    g.dev_ctx_phys[slot] = phys;
    memset_x(g.dev_ctx[slot], 0, sizeof(xhci_dev_ctx_t));
    g.dcbaa[slot] = phys;

    /* Allocate input context */
    uint64_t iphys = pmm_alloc_pages(2);
    if (!iphys) return false;
    xhci_input_ctx_t *ictx = (xhci_input_ctx_t *)pmm_phys_to_virt(iphys);
    memset_x(ictx, 0, sizeof(xhci_input_ctx_t));

    ictx->icc.add_flags = (1u << 0) | (1u << 1);  /* A0=slot, A1=EP0 */

    /* Slot context */
    ictx->dev.slot.dw0 = (route_string & 0xFFFFFu)
                       | ((uint32_t)speed << 20)
                       | (1u << 27);        /* context entries = 1 (EP0 only) */
    ictx->dev.slot.dw1 = (uint32_t)root_port1 << 16;
    if (hub_slot) {
        ictx->dev.slot.dw2 = (uint32_t)hub_slot        /* parent hub slot */
                           | ((uint32_t)hub_port << 8); /* parent port */
    }

    /* EP0 context */
    g.xfer[slot] = ring_alloc(RING_ENTRIES);
    ring_link(&g.xfer[slot]);
    ictx->dev.ep[0].dw1      = (4u << 3) | ((uint32_t)ep0_mps << 16);
    ictx->dev.ep[0].tr_dequeue_ptr = g.xfer[slot].phys | 1u;
    ictx->dev.ep[0].dw4      = 8u;

    ring_enqueue(&g.cmd, iphys, 0, TRB_TYPE(TRB_ADDR_DEV) | TRB_SLOT(slot));
    ring_doorbell(0, 0);

    uint32_t cc = 0;
    if (!wait_event(TRB_EV_CMD, &cc, NULL, NULL)) return false;
    return (cc == CC_SUCCESS);
}

/* ── Configure interrupt-IN endpoint ─────────────────────────────────────── */

static bool cmd_config_ep(uint8_t slot, uint8_t ep_addr,
                           uint8_t interval, uint16_t ep_mps) {
    uint8_t ep_num = ep_addr & 0x0Fu;
    uint8_t dir    = (ep_addr & 0x80u) ? 1u : 0u;
    uint8_t dci    = (uint8_t)(ep_num * 2u + dir);
    g.kbd_ep_dci   = dci;

    uint64_t iphys = pmm_alloc_pages(2);
    if (!iphys) return false;
    xhci_input_ctx_t *ictx = (xhci_input_ctx_t *)pmm_phys_to_virt(iphys);
    memset_x(ictx, 0, sizeof(xhci_input_ctx_t));

    ictx->icc.add_flags = (1u << 0) | (1u << dci);

    /* Copy and update slot context */
    memcpy_x(&ictx->dev.slot, &g.dev_ctx[slot]->slot, sizeof(xhci_slot_ctx_t));
    ictx->dev.slot.dw0 = (ictx->dev.slot.dw0 & ~(0x1Fu << 27)) | ((uint32_t)dci << 27);

    /* Interrupt-IN transfer ring */
    g.xfer[slot] = ring_alloc(XFER_ENTRIES);
    ring_link(&g.xfer[slot]);

    /* xHCI interval: FS bInterval (ms) → log2(bInterval * 8 microframes) */
    uint8_t xi = 3;
    if (interval >= 1) {
        uint32_t mf = (uint32_t)interval * 8u;
        uint8_t  lg = 0;
        while ((1u << lg) < mf && lg < 15) lg++;
        xi = lg;
    }

    ictx->dev.ep[dci - 1].dw0 = (uint32_t)xi << 16;
    ictx->dev.ep[dci - 1].dw1 = (7u << 3) | (1u << 6) | ((uint32_t)ep_mps << 16);
    ictx->dev.ep[dci - 1].tr_dequeue_ptr = g.xfer[slot].phys | 1u;
    ictx->dev.ep[dci - 1].dw4 = 8u;

    ring_enqueue(&g.cmd, iphys, 0, TRB_TYPE(TRB_CONFIG_EP) | TRB_SLOT(slot));
    ring_doorbell(0, 0);

    uint32_t cc = 0;
    if (!wait_event(TRB_EV_CMD, &cc, NULL, NULL)) return false;
    return (cc == CC_SUCCESS);
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

/* ── Try to enumerate one device as a HID boot keyboard ──────────────────── */
/*
 * slot:        already-enabled xHCI slot
 * root_port1:  1-based root hub port (or same as parent's root port for hub children)
 * speed:       PORTSC speed code
 * route:       20-bit route string (0 for root, hub_port in bits 3:0 for tier-1 hub)
 * hub_slot:    parent hub slot (0 = no hub)
 * hub_port:    1-based port on parent hub (0 = no hub)
 *
 * Returns: 0=not a keyboard, 1=keyboard set up OK, -1=error
 */
static int try_enumerate_kbd(uint8_t slot, uint8_t root_port1, uint8_t speed,
                              uint32_t route, uint8_t hub_slot, uint8_t hub_port) {
    /* Choose EP0 MPS based on speed: LS=8, FS=8, HS=64 */
    uint16_t ep0_mps = (speed == 3) ? 64 : 8;

    if (!cmd_address_device(slot, route, speed, root_port1,
                             hub_slot, hub_port, ep0_mps)) {
        return -1;
    }

    /* Allocate descriptor buffer */
    uint64_t buf_phys = pmm_alloc_page();
    if (!buf_phys) return -1;
    uint8_t *buf = (uint8_t *)pmm_phys_to_virt(buf_phys);

    /* GET_DESCRIPTOR: device descriptor (8 bytes to get class + MPS) */
    memset_x(buf, 0, 4096);
    if (!ctrl_xfer(slot, usb_setup(0x80, 6, 0x0100, 0, 8), buf_phys, 8, true))
        return -1;

    usb_dev_desc_t *dd = (usb_dev_desc_t *)buf;
    uint8_t dev_class = dd->bDeviceClass;

    /* Class 0x09 = Hub (caller handles this) */
    if (dev_class == 0x09) return 0;

    /* GET_DESCRIPTOR: full configuration descriptor */
    memset_x(buf, 0, 255);
    if (!ctrl_xfer(slot, usb_setup(0x80, 6, 0x0200, 0, 255), buf_phys, 255, true))
        return -1;

    /* Parse for HID boot keyboard interface + interrupt-IN endpoint */
    usb_cfg_desc_t *cd = (usb_cfg_desc_t *)buf;
    if (cd->bDescriptorType != 2) return 0;

    uint8_t  cfg_val    = cd->bConfigurationValue;
    uint8_t  ep_addr    = 0x81;
    uint16_t ep_mps     = 8;
    uint8_t  ep_iv      = 10;
    bool     found_kbd  = false;
    uint16_t total      = cd->wTotalLength;
    if (total > 255) total = 255;
    uint16_t off = cd->bLength;

    while (off < total) {
        uint8_t len  = buf[off];
        uint8_t type = buf[off + 1];
        if (len < 2) break;
        if (type == 4) {
            usb_intf_desc_t *id = (usb_intf_desc_t *)(buf + off);
            if (id->bInterfaceClass == 3 &&
                id->bInterfaceSubClass == 1 &&
                id->bInterfaceProtocol == 1)
                found_kbd = true;
            else
                found_kbd = false;
        } else if (type == 5 && found_kbd) {
            usb_ep_desc_t *ep = (usb_ep_desc_t *)(buf + off);
            if ((ep->bEndpointAddress & 0x80u) && (ep->bmAttributes & 3u) == 3u) {
                ep_addr = ep->bEndpointAddress;
                ep_mps  = ep->wMaxPacketSize;
                ep_iv   = ep->bInterval;
                break;
            }
        }
        off = (uint16_t)(off + len);
    }
    if (!found_kbd) return 0;  /* Not a keyboard */

    /* SET_CONFIGURATION */
    if (!ctrl_xfer(slot, usb_setup(0x00, 9, cfg_val, 0, 0), 0, 0, false))
        return -1;

    /* SET_PROTOCOL = 0 (boot protocol) */
    if (!ctrl_xfer(slot, usb_setup(0x21, 0x0B, 0, 0, 0), 0, 0, false))
        return -1;

    /* Configure interrupt-IN endpoint */
    if (!cmd_config_ep(slot, ep_addr, ep_iv, ep_mps)) return -1;

    /* Queue Normal TRBs */
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

/* ── Enumerate a USB hub and search for keyboard on its ports ────────────── */

static bool enumerate_hub(uint8_t hub_slot, uint8_t root_port1, uint8_t hub_speed __attribute__((unused))) {
    /* SET_CONFIGURATION 1 */
    if (!ctrl_xfer(hub_slot, usb_setup(0x00, 9, 1, 0, 0), 0, 0, false)) {
        kprintf("[xhci] hub SET_CONFIG failed\n");
        return false;
    }

    /* GET_DESCRIPTOR: Hub descriptor (type 0x29) */
    uint64_t buf_phys = pmm_alloc_page();
    if (!buf_phys) return false;
    uint8_t *buf = (uint8_t *)pmm_phys_to_virt(buf_phys);
    memset_x(buf, 0, 64);

    if (!ctrl_xfer(hub_slot, usb_setup(0xA0, 6, 0x2900, 0, 8), buf_phys, 8, true)) {
        kprintf("[xhci] hub GET_DESCRIPTOR failed\n");
        return false;
    }
    usb_hub_desc_t *hd = (usb_hub_desc_t *)buf;
    uint8_t nports     = hd->bNbrPorts;
    uint32_t pwr_delay = (uint32_t)hd->bPwrOn2PwrGood * 2u + 50u; /* ms */
    if (nports == 0 || nports > 15) nports = 8;
    if (pwr_delay < 50) pwr_delay = 50;

    kprintf("[xhci] hub slot %u has %u ports, pwr_delay=%ums\n",
            hub_slot, nports, pwr_delay);

    /* Power each downstream port */
    for (uint8_t p = 1; p <= nports; p++) {
        ctrl_xfer(hub_slot, usb_setup(0x23, HUB_SET_FEATURE, HUB_PORT_POWER, p, 0),
                  0, 0, false);
    }
    udelay(pwr_delay * 1000u);  /* wait for power-on */

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

        kprintf("[xhci] hub port %u: device connected speed=%u\n", p, pspeed);

        /* Reset the hub port */
        ctrl_xfer(hub_slot, usb_setup(0x23, HUB_SET_FEATURE, HUB_PORT_RESET, p, 0),
                  0, 0, false);
        udelay(60000);  /* 60ms reset */

        /* Clear C_PORT_RESET */
        ctrl_xfer(hub_slot, usb_setup(0x23, HUB_CLEAR_FEATURE, HUB_C_PORT_RESET, p, 0),
                  0, 0, false);
        udelay(5000);

        /* Enable a slot for this device */
        uint8_t slot = 0;
        if (!cmd_enable_slot(&slot)) {
            kprintf("[xhci] hub port %u: enable_slot failed\n", p);
            continue;
        }

        /* Route string: tier-1 = hub port number in bits 3:0 */
        uint32_t route = p & 0xFu;

        int r = try_enumerate_kbd(slot, root_port1, pspeed,
                                  route, hub_slot, p);
        if (r == 1) {
            kprintf("[xhci] USB HID keyboard on hub slot %u port %u\n", hub_slot, p);
            g.kbd_slot = slot;
            return true;
        }
    }
    return false;
}

/* ── Root port reset ─────────────────────────────────────────────────────── */

static bool root_port_reset(uint8_t port0, uint8_t *speed_out) {
    uint32_t sc = mmio_r32(g.op, OP_PORTSC(port0));
    if (!(sc & PORTSC_CCS)) return false;

    /* Assert port reset (clear W1C bits first) */
    sc = (sc & ~PORTSC_W1C_BITS) | PORTSC_PR;
    mmio_w32(g.op, OP_PORTSC(port0), sc);
    udelay(60000);  /* 60ms */

    /* Wait for PRC */
    for (int i = 0; i < 100; i++) {
        sc = mmio_r32(g.op, OP_PORTSC(port0));
        if (sc & PORTSC_PRC) break;
        udelay(2000);
    }
    /* Clear W1C */
    mmio_w32(g.op, OP_PORTSC(port0), (sc & ~PORTSC_W1C_BITS) | PORTSC_W1C_BITS);
    udelay(2000);

    sc = mmio_r32(g.op, OP_PORTSC(port0));
    *speed_out = (uint8_t)PORTSC_SPEED(sc);
    return (sc & PORTSC_PED) != 0;
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
    /* 0x46-0x4E */ 0,0,0,0,0,0,0,0,0,
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
    0,0,0,0,0,0,0,0,0,
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

/* ── Per-controller init (called for each XHCI PCI device found) ─────────── */

static bool xhci_init_one(uint8_t bus, uint8_t dev, uint8_t fn) {
    kprintf("[xhci] trying controller %u:%u.%u\n", bus, dev, fn);

    uint64_t mmio_phys = pci_bar_base64(bus, dev, fn, 0);
    if (!mmio_phys) { kprintf("[xhci] bad BAR0\n"); return false; }

    /* Each controller gets its own 64KB MMIO window */
    static uint64_t mmio_virt_next = 0xFFFFFF0040000000ULL;
    uint64_t mmio_virt = mmio_virt_next;
    mmio_virt_next += 0x10000ULL;
    if (!vmm_map_range(mmio_virt, mmio_phys, 0x10000, VMM_WRITE)) {
        kprintf("[xhci] MMIO map failed\n"); return false;
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

    kprintf("[xhci] phys=%p maxslots=%u maxports=%u\n",
            (void *)mmio_phys, g.max_slots, g.max_ports);

    if (!ctrl_reset()) { kprintf("[xhci] reset timed out\n"); return false; }

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

    if (!ctrl_start()) { kprintf("[xhci] start failed\n"); return false; }
    g.present = true;
    kprintf("[xhci] controller running\n");

    /* Power all ports and wait for devices to appear */
    for (uint8_t p = 0; p < g.max_ports; p++) {
        uint32_t sc = mmio_r32(g.op, OP_PORTSC(p));
        if (!(sc & PORTSC_PP)) {
            mmio_w32(g.op, OP_PORTSC(p), (sc & ~PORTSC_W1C_BITS) | PORTSC_PP);
        }
    }
    udelay(300000);  /* 300ms for port power + device enumeration */

    /* Retry port scan up to 5 times with 100ms gaps */
    for (int attempt = 0; attempt < 5 && g.kbd_slot < 0; attempt++) {
        if (attempt > 0) udelay(100000);

        for (uint8_t p = 0; p < g.max_ports; p++) {
            if (g.kbd_slot >= 0) break;
            uint32_t sc = mmio_r32(g.op, OP_PORTSC(p));
            if (!(sc & PORTSC_CCS)) continue;

            uint8_t speed = 0;
            if (!root_port_reset(p, &speed)) {
                kprintf("[xhci] port %u reset failed (sc=%x)\n", p,
                        mmio_r32(g.op, OP_PORTSC(p)));
                continue;
            }
            kprintf("[xhci] port %u: device speed=%u\n", p, speed);

            uint8_t slot = 0;
            if (!cmd_enable_slot(&slot)) {
                kprintf("[xhci] port %u: enable_slot failed\n", p);
                continue;
            }

            /* First: try direct keyboard enumeration */
            int r = try_enumerate_kbd(slot, (uint8_t)(p + 1), speed,
                                      0, 0, 0);
            if (r == 1) {
                kprintf("[xhci] USB HID keyboard ready on slot %u port %u\n",
                        slot, p + 1);
                g.kbd_slot = slot;
                break;
            }

            /* Not a keyboard — check if it's a hub */
            /* Re-get device descriptor to check class */
            uint64_t buf_phys = pmm_alloc_page();
            uint8_t *buf = (uint8_t *)pmm_phys_to_virt(buf_phys);
            memset_x(buf, 0, 64);

            /* address_device was already called inside try_enumerate_kbd,
             * so we can reuse the existing EP0 control pipe for this slot */
            if (ctrl_xfer(slot, usb_setup(0x80, 6, 0x0100, 0, 8),
                          buf_phys, 8, true)) {
                usb_dev_desc_t *dd = (usb_dev_desc_t *)buf;
                if (dd->bDeviceClass == 0x09) {
                    kprintf("[xhci] port %u: USB hub detected, scanning...\n", p);
                    if (enumerate_hub(slot, (uint8_t)(p + 1), speed)) {
                        /* kbd_slot set inside enumerate_hub */
                        break;
                    }
                }
            }
        }
    }

    if (g.kbd_slot < 0)
        kprintf("[xhci] no USB keyboard on this controller\n");

    return (g.kbd_slot >= 0);
}

/* ── xhci_init ────────────────────────────────────────────────────────────── */

void xhci_init(void) {
    memset_x(&g, 0, sizeof(g));
    g.kbd_slot = -1;

    /* Collect all XHCI controllers (AMD Ryzen has 2: one external, one internal) */
    uint8_t all_bus[8], all_dev[8], all_fn[8];
    uint32_t n = pci_find_all_class(0x0C, 0x03, 0x30,
                                     all_bus, all_dev, all_fn, 8);
    if (n == 0) {
        kprintf("[xhci] no XHCI controller found\n");
        return;
    }
    kprintf("[xhci] found %u XHCI controller(s)\n", (unsigned)n);

    for (uint32_t i = 0; i < n; i++) {
        /* Reset global state for each controller attempt */
        memset_x(&g, 0, sizeof(g));
        g.kbd_slot = -1;
        if (xhci_init_one(all_bus[i], all_dev[i], all_fn[i]))
            return;  /* keyboard found, stop */
    }
    kprintf("[xhci] no USB keyboard found on any controller\n");
}

/* ── xhci_poll ─ called from pit_on_tick() ───────────────────────────────── */

void xhci_poll(void) {
    if (!g.present || g.kbd_slot < 0) return;

    for (int limit = 0; limit < 16; limit++) {
        xhci_trb_t *ev = &g.evt.trbs[g.evt.deq];
        if ((ev->control & 1u) != (uint32_t)g.evt.cycle) break;

        uint8_t type = (uint8_t)((ev->control >> 10) & 0x3Fu);
        if (type == TRB_EV_XFER && (uint8_t)(ev->control >> 24) == (uint8_t)g.kbd_slot) {
            uint64_t trb_phys = ev->param;
            for (uint32_t i = 0; i < KBD_BUFS; i++) {
                if (trb_phys == g.kbd_data_phys[i]) {
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
