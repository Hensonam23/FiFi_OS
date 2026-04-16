/*
 * rtl8168.c — Realtek RTL8111/8168 GbE driver (polling, no interrupts)
 *
 * Supports the RTL8111/8168/8211/8411 family (PCI 0x10EC:0x8168).
 * Uses BAR0 (I/O port space, 256 bytes) for all register access.
 * TX is synchronous (post one frame, wait for NIC to transmit, return).
 * RX is polled via rtl8168_recv() called from net_poll() every PIT tick.
 *
 * Memory layout:
 *   TX descriptor ring  — 1 page (16 descriptors, EOR on last)
 *   TX data buffer      — 1 page (shared scratch for all TX frames)
 *   RX descriptor ring  — 1 page (RX_RING_SIZE descriptors, EOR on last)
 *   RX data buffers     — 1 page each (RX_RING_SIZE pages)
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "rtl8168.h"
#include "pci.h"
#include "pmm.h"
#include "kprintf.h"
#include "io.h"

/* ── PCI identity ─────────────────────────────────────────────────────────── */
#define RTL_VENDOR  0x10ECu
#define RTL_DEVICE  0x8168u

/* ── Register offsets (I/O port, BAR0) ───────────────────────────────────── */
#define RTL_MAC0        0x00   /* MAC address bytes 0-5 (byte access) */
#define RTL_MAR0        0x08   /* Multicast filter (8 bytes) */
#define RTL_TNPDS       0x20   /* TX normal-priority descriptor start (64-bit) */
#define RTL_CR          0x37   /* Command register */
#define RTL_TPPOLL      0x38   /* TX priority poll (byte) */
#define RTL_IMR         0x3C   /* Interrupt mask register (word) */
#define RTL_ISR         0x3E   /* Interrupt status register (word) */
#define RTL_TCR         0x40   /* TX configuration (dword) */
#define RTL_RCR         0x44   /* RX configuration (dword) */
#define RTL_9346CR      0x50   /* 9346 EEPROM / config unlock */
#define RTL_RMS         0xDA   /* RX max packet size (word) — some chips */
#define RTL_RDSAR       0xD0   /* RX descriptor start address (64-bit) */

/* ── CR bits ──────────────────────────────────────────────────────────────── */
#define RTL_CR_RST  0x10u   /* Software reset */
#define RTL_CR_RE   0x08u   /* Receiver enable */
#define RTL_CR_TE   0x04u   /* Transmitter enable */

/* ── TCR: TX configuration ────────────────────────────────────────────────── */
/* Max DMA burst size = unlimited (bits 10:8 = 111), IFG = normal (bits 25:24 = 11) */
#define RTL_TCR_VAL  0x03000700u

/* ── RCR: RX configuration ────────────────────────────────────────────────── */
/* APM (accept physical match) | AB (accept broadcast)
 * Max DMA burst unlimited (bits 10:8 = 111)
 * RX FIFO threshold = none / wait for full packet (bits 15:13 = 111) */
#define RTL_RCR_APM  0x00000004u
#define RTL_RCR_AB   0x00000008u
#define RTL_RCR_VAL  (RTL_RCR_APM | RTL_RCR_AB | 0x0000E700u)

/* ── Descriptor format ────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t opts1;    /* flags + length */
    uint32_t opts2;    /* VLAN / checksum offload (unused: 0) */
    uint32_t buf_lo;   /* buffer physical address, low 32 bits */
    uint32_t buf_hi;   /* buffer physical address, high 32 bits */
} rtl_desc_t;

/* opts1 flags */
#define RTL_OWN  (1u << 31)   /* 1 = NIC owns this descriptor */
#define RTL_EOR  (1u << 30)   /* end of ring — wraps to start */
#define RTL_FS   (1u << 29)   /* first segment (TX) */
#define RTL_LS   (1u << 28)   /* last segment (TX) */
/* opts1[13:0] = length in bytes */
#define RTL_LEN_MASK  0x3FFFu

/* ── Ring geometry ────────────────────────────────────────────────────────── */
#define TX_RING_SIZE   16u     /* TX descriptors (we only actively use desc[0]) */
#define RX_RING_SIZE   16u     /* RX descriptors / buffers */
#define PAGE_SIZE      4096u
#define RX_BUF_SIZE    4096u   /* bytes per RX buffer — well above max frame */
#define MAX_FRAME      1514u   /* max Ethernet frame without FCS */

/* ── Driver state ─────────────────────────────────────────────────────────── */
static bool     g_present = false;
static uint16_t g_iobase  = 0;
static uint8_t  g_mac[6]  = {0};

/* TX ring — lives in one PMM page; only desc[0] used (synchronous TX) */
static rtl_desc_t *g_tx_desc   = NULL;
static uint64_t    g_tx_desc_phys = 0;

/* TX data buffer — one PMM page, shared for all TX frames */
static uint8_t  *g_tx_virt   = NULL;
static uint64_t  g_tx_phys   = 0;

/* RX ring */
static rtl_desc_t *g_rx_desc   = NULL;
static uint64_t    g_rx_desc_phys = 0;
static uint16_t    g_rx_cur    = 0;   /* next descriptor to check */

/* RX buffers */
static uint64_t g_rx_phys[RX_RING_SIZE];
static uint8_t *g_rx_virt[RX_RING_SIZE];

/* ── I/O helpers ──────────────────────────────────────────────────────────── */
static inline uint8_t  r8 (uint8_t off) { return inb ((uint16_t)(g_iobase + off)); }
static inline void     w8 (uint8_t off, uint8_t  v) { outb((uint16_t)(g_iobase + off), v); }
static inline void     w16(uint8_t off, uint16_t v) { outw((uint16_t)(g_iobase + off), v); }
static inline void     w32(uint8_t off, uint32_t v) { outl((uint16_t)(g_iobase + off), v); }

static inline void rtl_mb(void) { __asm__ __volatile__("mfence" ::: "memory"); }

/* Write a 64-bit value as two 32-bit I/O writes (lo first) */
static inline void w64(uint8_t off, uint64_t v) {
    w32(off,     (uint32_t)(v & 0xFFFFFFFFu));
    w32((uint8_t)(off + 4), (uint32_t)(v >> 32));
}

/* ── rtl8168_init ─────────────────────────────────────────────────────────── */
bool rtl8168_init(void) {
    uint8_t bus, dev, fn;
    if (!pci_find(RTL_VENDOR, RTL_DEVICE, &bus, &dev, &fn)) {
        /* Not found — not an error, just not present */
        return false;
    }
    kprintf("[rtl8168] found at PCI %u:%u.%u\n",
            (unsigned)bus, (unsigned)dev, (unsigned)fn);

    pci_enable(bus, dev, fn);

    bool is_io = false;
    uint32_t bar0 = pci_bar_base(bus, dev, fn, 0, &is_io);
    if (!is_io || bar0 == 0) {
        kprintf("[rtl8168] BAR0 is not I/O space\n");
        return false;
    }
    g_iobase = (uint16_t)bar0;
    kprintf("[rtl8168] I/O base=0x%x\n", (unsigned)g_iobase);

    /* ── 1. Software reset ─────────────────────────────────────────────── */
    w8(RTL_CR, RTL_CR_RST);
    for (uint32_t i = 0; i < 100000u; i++) {
        if (!(r8(RTL_CR) & RTL_CR_RST)) break;
    }
    if (r8(RTL_CR) & RTL_CR_RST) {
        kprintf("[rtl8168] reset timeout\n");
        return false;
    }

    /* ── 2. Read MAC address ────────────────────────────────────────────── */
    for (int i = 0; i < 6; i++) g_mac[i] = r8((uint8_t)(RTL_MAC0 + i));
    kprintf("[rtl8168] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            (unsigned)g_mac[0], (unsigned)g_mac[1], (unsigned)g_mac[2],
            (unsigned)g_mac[3], (unsigned)g_mac[4], (unsigned)g_mac[5]);

    /* ── 3. Unlock config registers ─────────────────────────────────────── */
    w8(RTL_9346CR, 0xC0u);

    /* ── 4. Disable interrupts ──────────────────────────────────────────── */
    w16(RTL_IMR, 0x0000u);
    w16(RTL_ISR, 0xFFFFu);   /* clear any pending */

    /* ── 5. Allocate TX descriptor ring and data buffer ─────────────────── */
    g_tx_desc_phys = pmm_alloc_page();
    if (!g_tx_desc_phys) return false;
    g_tx_desc = (rtl_desc_t *)pmm_phys_to_virt(g_tx_desc_phys);
    for (size_t i = 0; i < PAGE_SIZE; i++) ((uint8_t *)g_tx_desc)[i] = 0;

    /* Set EOR on the last descriptor in the TX ring */
    g_tx_desc[TX_RING_SIZE - 1].opts1 = RTL_EOR;

    g_tx_phys = pmm_alloc_page();
    if (!g_tx_phys) return false;
    g_tx_virt = (uint8_t *)pmm_phys_to_virt(g_tx_phys);

    /* ── 6. Allocate RX descriptor ring and buffers ─────────────────────── */
    g_rx_desc_phys = pmm_alloc_page();
    if (!g_rx_desc_phys) return false;
    g_rx_desc = (rtl_desc_t *)pmm_phys_to_virt(g_rx_desc_phys);
    for (size_t i = 0; i < PAGE_SIZE; i++) ((uint8_t *)g_rx_desc)[i] = 0;

    for (uint32_t i = 0; i < RX_RING_SIZE; i++) {
        g_rx_phys[i] = pmm_alloc_page();
        if (!g_rx_phys[i]) return false;
        g_rx_virt[i] = (uint8_t *)pmm_phys_to_virt(g_rx_phys[i]);

        uint32_t opts1 = RTL_OWN | RX_BUF_SIZE;
        if (i == RX_RING_SIZE - 1) opts1 |= RTL_EOR;

        g_rx_desc[i].opts1  = opts1;
        g_rx_desc[i].opts2  = 0;
        g_rx_desc[i].buf_lo = (uint32_t)(g_rx_phys[i] & 0xFFFFFFFFu);
        g_rx_desc[i].buf_hi = (uint32_t)(g_rx_phys[i] >> 32);
    }
    g_rx_cur = 0;

    /* ── 7. Program descriptor base addresses ───────────────────────────── */
    w64(RTL_TNPDS, g_tx_desc_phys);
    w64(RTL_RDSAR, g_rx_desc_phys);

    /* ── 8. TX / RX configuration ───────────────────────────────────────── */
    w32(RTL_TCR, RTL_TCR_VAL);
    w32(RTL_RCR, RTL_RCR_VAL);

    /* Max RX packet size (word register on most RTL8168 variants) */
    w16(RTL_RMS, (uint16_t)(RX_BUF_SIZE - 1u));

    /* Multicast filter: accept all (MAR0-7 = 0xFF each) */
    for (uint8_t i = 0; i < 8; i++) w8((uint8_t)(RTL_MAR0 + i), 0xFFu);

    /* ── 9. Lock config and enable TX + RX ──────────────────────────────── */
    w8(RTL_9346CR, 0x00u);
    rtl_mb();
    w8(RTL_CR, RTL_CR_TE | RTL_CR_RE);

    g_present = true;
    kprintf("[rtl8168] ready\n");
    return true;
}

/* ── rtl8168_send ─────────────────────────────────────────────────────────── */
bool rtl8168_send(const void *frame, size_t len) {
    if (!g_present) return false;
    if (len == 0 || len > MAX_FRAME) return false;

    /* Copy frame into the TX data page */
    const uint8_t *src = (const uint8_t *)frame;
    for (size_t i = 0; i < len; i++) g_tx_virt[i] = src[i];

    /* Descriptor 0: OWN | FS | LS | EOR | len
     * EOR is always set since we use a single-entry TX ring at desc[0].   */
    rtl_mb();
    g_tx_desc[0].opts2  = 0;
    g_tx_desc[0].buf_lo = (uint32_t)(g_tx_phys & 0xFFFFFFFFu);
    g_tx_desc[0].buf_hi = (uint32_t)(g_tx_phys >> 32);
    rtl_mb();
    g_tx_desc[0].opts1  = RTL_OWN | RTL_FS | RTL_LS | RTL_EOR | (uint32_t)len;
    rtl_mb();

    /* Kick the NIC: NPQ bit — normal priority TX queue */
    w8(RTL_TPPOLL, 0x40u);

    /* Wait for NIC to process (OWN clears when done) */
    uint32_t spin = 0;
    while (g_tx_desc[0].opts1 & RTL_OWN) {
        rtl_mb();
        if (++spin > 2000000u) {
            kprintf("[rtl8168] TX timeout\n");
            return false;
        }
    }
    return true;
}

/* ── rtl8168_recv ─────────────────────────────────────────────────────────── */
size_t rtl8168_recv(void *buf, size_t buf_len) {
    if (!g_present) return 0;

    rtl_desc_t *desc = &g_rx_desc[g_rx_cur];
    rtl_mb();

    /* OWN=1 means NIC still owns the descriptor (buffer empty) */
    if (desc->opts1 & RTL_OWN) return 0;

    uint32_t opts1 = desc->opts1;

    /* Error summary bit (bit 21) — discard and repost */
    if (opts1 & (1u << 21)) {
        goto repost;
    }

    {
        /* Length field includes 4-byte FCS — strip it */
        size_t pkt_len = (size_t)(opts1 & RTL_LEN_MASK);
        if (pkt_len < 4u) goto repost;
        size_t frame_len = pkt_len - 4u;
        if (frame_len > buf_len) frame_len = buf_len;

        /* Copy frame out */
        const uint8_t *src = g_rx_virt[g_rx_cur];
        uint8_t       *dst = (uint8_t *)buf;
        for (size_t i = 0; i < frame_len; i++) dst[i] = src[i];

        /* Repost the descriptor for NIC reuse */
        repost:;
        uint32_t new_opts1 = RTL_OWN | RX_BUF_SIZE;
        if (g_rx_cur == RX_RING_SIZE - 1u) new_opts1 |= RTL_EOR;

        desc->opts2  = 0;
        desc->buf_lo = (uint32_t)(g_rx_phys[g_rx_cur] & 0xFFFFFFFFu);
        desc->buf_hi = (uint32_t)(g_rx_phys[g_rx_cur] >> 32);
        rtl_mb();
        desc->opts1 = new_opts1;

        g_rx_cur = (uint16_t)((g_rx_cur + 1u) & (RX_RING_SIZE - 1u));

        if (opts1 & (1u << 21)) return 0;   /* error — discard */
        return frame_len;
    }
}

/* ── accessors ───────────────────────────────────────────────────────────── */
void rtl8168_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = g_mac[i];
}

bool rtl8168_present(void) { return g_present; }
