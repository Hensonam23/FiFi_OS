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
#define RTL_TPPOLL      0x38   /* TX priority poll — QEMU emulation / older RTL8168 */
#define RTL_TP_POLL     0xD9   /* TX priority poll — RTL8168G/H real silicon (Linux r8169) */
#define RTL_NPQ         0x40u  /* Normal Priority Queue bit in TP_POLL */
#define RTL_IMR         0x3C   /* Interrupt mask register (word) */
#define RTL_ISR         0x3E   /* Interrupt status register (word) */
#define RTL_TCR         0x40   /* TX configuration (dword) */
#define RTL_RCR         0x44   /* RX configuration (dword) */
#define RTL_9346CR      0x50   /* 9346 EEPROM / config unlock */
#define RTL_MTPS        0x60   /* Max TX packet size (byte) — correct offset */
#define RTL_PHYSR       0x6C   /* PHY status register (byte) */
/* RDSAR: RX descriptor start address.
 * Older RTL8168B/C/D datasheets show offset 0xD0, but on RTL8168G/H/8411
 * (Linux r8169 / newer silicon) 0xD0 is DLLPR — a completely different
 * register. Writing our physical address there corrupts DLLPR and MCU (0xD3).
 * Linux r8169 (which handles all modern variants) always uses 0xE4.
 * We only write to 0xE4 to avoid corrupting DLLPR on newer chips.        */
#define RTL_MCU         0xD3   /* MCU control register (RTL8168G+) */
#define RTL_RMS         0xDA   /* RX max packet size (word) */
#define RTL_CPCMD       0xE0   /* C+ command register (word) */
#define RTL_RDSAR       0xE4   /* RX descriptor start address (Linux r8169 offset) */
#define RTL_MISC        0xF0   /* Miscellaneous register (dword) */

/* ── CR bits ──────────────────────────────────────────────────────────────── */
#define RTL_CR_RST  0x10u   /* Software reset */
#define RTL_CR_RE   0x08u   /* Receiver enable */
#define RTL_CR_TE   0x04u   /* Transmitter enable */

/* ── TCR: TX configuration ────────────────────────────────────────────────── */
/* Max DMA burst size = unlimited (bits 10:8 = 111), IFG = normal (bits 25:24 = 11)
 * AUTO_FIFO (bit 7): required on RTL8168b+ to allow TX FIFO to work correctly. */
#define RTL_TCR_VAL  0x03000780u

/* ── RCR: RX configuration ────────────────────────────────────────────────── */
/* Linux r8169 RCR bit layout (RTL8168 datasheet §6.x):
 *   Bit 0: AAP  — Accept All Packets (promiscuous)
 *   Bit 1: APM  — Accept Physical Match (unicast to our MAC)  ← we need this
 *   Bit 2: AM   — Accept Multicast (hash-filtered via MAR)
 *   Bit 3: AB   — Accept Broadcast (ff:ff:ff:ff:ff:ff)
 * Previously RTL_RCR_APM was wrongly defined as 0x04 (bit 2 = AM, not APM).
 * That meant unicast frames (including ARP replies) were silently dropped —
 * only broadcasts made it through. Fix: APM must be bit 1 = 0x02.          */
#define RTL_RCR_APM  0x00000002u   /* bit 1 — Accept Physical Match (unicast) */
#define RTL_RCR_AM   0x00000004u   /* bit 2 — Accept Multicast */
#define RTL_RCR_AB   0x00000008u   /* bit 3 — Accept Broadcast */
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
static inline uint16_t r16(uint8_t off) { return inw ((uint16_t)(g_iobase + off)); }
static inline uint32_t r32(uint8_t off) { return inl ((uint16_t)(g_iobase + off)); }
static inline void     w8 (uint8_t off, uint8_t  v) { outb((uint16_t)(g_iobase + off), v); }
static inline void     w16(uint8_t off, uint16_t v) { outw((uint16_t)(g_iobase + off), v); }
static inline void     w32(uint8_t off, uint32_t v) { outl((uint16_t)(g_iobase + off), v); }

static inline void rtl_mb(void) { __asm__ __volatile__("mfence" ::: "memory"); }


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

    /* Print PCI revision to identify chip variant */
    uint8_t pci_rev = pci_read8(bus, dev, fn, 0x08);
    kprintf("[rtl8168] PCI revision 0x%02x\n", (unsigned)pci_rev);

    /* ── 3. Unlock config registers ─────────────────────────────────────── */
    w8(RTL_9346CR, 0xC0u);

    /* ── 3a. Write MAC back to MAC0 registers (while config is unlocked).
     * On RTL8168G/H, software reset clears the MAC0 register — the NIC
     * no longer matches unicast frames (APM = Accept Physical Match) until
     * the MAC is explicitly written back. Broadcasts still work via the AB
     * bit regardless, but unicast ARP replies require APM to work.
     * The Linux r8169 driver calls rtl_set_mac_address() for exactly this.
     * We write 4 bytes then 2 bytes, matching the Linux driver's pattern.  */
    w32(RTL_MAC0,     ((uint32_t)g_mac[0])        |
                      ((uint32_t)g_mac[1] <<  8)   |
                      ((uint32_t)g_mac[2] << 16)   |
                      ((uint32_t)g_mac[3] << 24));
    w32((uint8_t)(RTL_MAC0 + 4),
                      ((uint32_t)g_mac[4])          |
                      ((uint32_t)g_mac[5] <<  8));

    /* ── 4. Disable interrupts ──────────────────────────────────────────── */
    w16(RTL_IMR, 0x0000u);
    w16(RTL_ISR, 0xFFFFu);   /* clear any pending */

    /* ── 4a. Clear OOB state (RTL8168G/H/8411).
     * Newer chips boot in Out-Of-Band mode for management firmware.
     * Normal TX is blocked while NOW_IS_OOB (bit 7) is set.
     * This is a clean byte read-modify-write to MCU (0xD3) only —
     * no 32-bit write that would accidentally touch DLLPR (0xD0).        */
    w8(RTL_MCU, (uint8_t)(r8(RTL_MCU) & ~0x80u));

    /* ── 4b. Clear RXDV gate and pulse TX-lookahead reset (MISC at 0xF0).
     *        Required on RTL8168G+; harmless on older variants.          */
    {
        uint32_t misc = r32(RTL_MISC);
        misc &= ~(1u << 11);                 /* clear RXDV_GATED_EN */
        w32(RTL_MISC, misc);
        w32(RTL_MISC, misc | (1u << 29));    /* pulse TXPLA_RST high */
        w32(RTL_MISC, misc);                 /* then low */
    }

    /* ── 5. Allocate TX descriptor ring and data buffer (must be below 4 GiB) ─
     * DMA32 zone guarantees buf_hi = 0 so no 64-bit DAC mode is needed.  */
    g_tx_desc_phys = pmm_alloc_dma32_page();
    if (!g_tx_desc_phys) { kprintf("[rtl8168] no DMA32 memory\n"); return false; }
    g_tx_desc = (rtl_desc_t *)pmm_phys_to_virt(g_tx_desc_phys);
    for (size_t i = 0; i < PAGE_SIZE; i++) ((uint8_t *)g_tx_desc)[i] = 0;

    /* Set EOR on the last descriptor in the TX ring */
    g_tx_desc[TX_RING_SIZE - 1].opts1 = RTL_EOR;

    g_tx_phys = pmm_alloc_dma32_page();
    if (!g_tx_phys) { kprintf("[rtl8168] no DMA32 memory\n"); return false; }
    g_tx_virt = (uint8_t *)pmm_phys_to_virt(g_tx_phys);

    kprintf("[rtl8168] TX desc phys=0x%08x data phys=0x%08x\n",
            (unsigned)g_tx_desc_phys, (unsigned)g_tx_phys);

    /* ── 6. Allocate RX descriptor ring and buffers (below 4 GiB) ──────── */
    g_rx_desc_phys = pmm_alloc_dma32_page();
    if (!g_rx_desc_phys) { kprintf("[rtl8168] no DMA32 memory\n"); return false; }
    g_rx_desc = (rtl_desc_t *)pmm_phys_to_virt(g_rx_desc_phys);
    for (size_t i = 0; i < PAGE_SIZE; i++) ((uint8_t *)g_rx_desc)[i] = 0;

    for (uint32_t i = 0; i < RX_RING_SIZE; i++) {
        g_rx_phys[i] = pmm_alloc_dma32_page();
        if (!g_rx_phys[i]) { kprintf("[rtl8168] no DMA32 memory\n"); return false; }
        g_rx_virt[i] = (uint8_t *)pmm_phys_to_virt(g_rx_phys[i]);

        uint32_t opts1 = RTL_OWN | RX_BUF_SIZE;
        if (i == RX_RING_SIZE - 1) opts1 |= RTL_EOR;

        g_rx_desc[i].opts1  = opts1;
        g_rx_desc[i].opts2  = 0;
        g_rx_desc[i].buf_lo = (uint32_t)(g_rx_phys[i] & 0xFFFFFFFFu);
        g_rx_desc[i].buf_hi = 0;
    }
    g_rx_cur = 0;

    /* ── 7. Program descriptor base addresses ───────────────────────────── */
    /* TNPDS (TX ring) — offset 0x20, same on all RTL8168 variants */
    w32(RTL_TNPDS,                 (uint32_t)(g_tx_desc_phys & 0xFFFFFFFFu));
    w32((uint8_t)(RTL_TNPDS + 4), 0u);

    /* Read back TNPDS to confirm the write was accepted */
    uint32_t tnpds_rb = r32(RTL_TNPDS);
    kprintf("[rtl8168] TNPDS write=0x%08x readback=0x%08x %s\n",
            (unsigned)(g_tx_desc_phys & 0xFFFFFFFFu), (unsigned)tnpds_rb,
            tnpds_rb == (uint32_t)(g_tx_desc_phys & 0xFFFFFFFFu) ? "OK" : "MISMATCH!");

    /* RDSAR (RX ring) — use offset 0xE4 (Linux r8169 / RTL8168G+).
     * We do NOT write to 0xD0 — that is DLLPR on newer chips, not RDSAR. */
    w32(RTL_RDSAR,                 (uint32_t)(g_rx_desc_phys & 0xFFFFFFFFu));
    w32((uint8_t)(RTL_RDSAR + 4), 0u);
    {
        uint32_t rdsar_rb = r32(RTL_RDSAR);
        kprintf("[rtl8168] RDSAR write=0x%08x readback=0x%08x %s\n",
                (unsigned)(g_rx_desc_phys & 0xFFFFFFFFu), (unsigned)rdsar_rb,
                rdsar_rb == (uint32_t)(g_rx_desc_phys & 0xFFFFFFFFu) ? "OK" : "MISMATCH!");
    }

    /* ── 8. TX / RX configuration ───────────────────────────────────────── */
    w32(RTL_TCR, RTL_TCR_VAL);
    w32(RTL_RCR, RTL_RCR_VAL);

    /* Max RX packet size */
    w16(RTL_RMS, (uint16_t)(RX_BUF_SIZE - 1u));

    /* Max TX packet size (register at 0x60 on all RTL8168 variants) */
    w8(RTL_MTPS, 0x3Bu);

    /* C+ command register: enable TX/RX descriptor mode.
     * All DMA buffers are below 4 GiB; PCIDAC not needed.               */
    uint16_t cpcmd = r16(RTL_CPCMD);
    cpcmd |= (1u << 0)   /* CmdRxEnb — RX in C+ descriptor mode */
           | (1u << 1);  /* CmdTxEnb — TX in C+ descriptor mode */
    w16(RTL_CPCMD, cpcmd);
    kprintf("[rtl8168] CPlusCmd=0x%04x\n", (unsigned)r16(RTL_CPCMD));

    /* Multicast filter: accept all (MAR0-7 = 0xFF each) */
    for (uint8_t i = 0; i < 8; i++) w8((uint8_t)(RTL_MAR0 + i), 0xFFu);

    /* ── 9. Lock config and enable TX + RX ──────────────────────────────── */
    w8(RTL_9346CR, 0x00u);
    rtl_mb();
    w8(RTL_CR, RTL_CR_TE | RTL_CR_RE);
    kprintf("[rtl8168] CR=0x%02x\n", (unsigned)r8(RTL_CR));

    /* Log link status (PHYSR bit 1 = link up) */
    uint8_t physr = r8(RTL_PHYSR);
    kprintf("[rtl8168] link: %s (PHYSR=0x%02x)\n",
            (physr & 0x02u) ? "up" : "DOWN", (unsigned)physr);

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

    /* Descriptor 0: OWN | FS | LS | EOR | len */
    static bool s_first_tx = true;
    if (s_first_tx) {
        s_first_tx = false;
        kprintf("[rtl8168] first TX: len=%u buf=0x%08x desc=0x%08x\n",
                (unsigned)len, (unsigned)g_tx_phys, (unsigned)g_tx_desc_phys);
        kprintf("[rtl8168]   CR=0x%02x CPlusCmd=0x%04x TCR=0x%08x MCU=0x%02x ISR=0x%04x\n",
                (unsigned)r8(RTL_CR), (unsigned)r16(RTL_CPCMD),
                (unsigned)r32(RTL_TCR), (unsigned)r8(RTL_MCU),
                (unsigned)r16(RTL_ISR));
    }

    rtl_mb();
    g_tx_desc[0].opts2  = 0;
    g_tx_desc[0].buf_lo = (uint32_t)(g_tx_phys & 0xFFFFFFFFu);
    g_tx_desc[0].buf_hi = 0;
    rtl_mb();
    g_tx_desc[0].opts1  = RTL_OWN | RTL_FS | RTL_LS | RTL_EOR | (uint32_t)len;
    rtl_mb();

    /* Kick the NIC: NPQ (Normal Priority Queue) bit.
     * Write to BOTH poll registers:
     *   0x38 (RTL_TPPOLL)  — older chips and QEMU emulation
     *   0xD9 (RTL_TP_POLL) — RTL8168G/H real silicon (what Linux r8169 uses)
     * On real hardware, 0x38 is silently ignored; on QEMU, 0xD9 is ignored.
     * Writing to both is harmless and covers all variants.                  */
    w8(RTL_TPPOLL,  RTL_NPQ);
    w8(RTL_TP_POLL, RTL_NPQ);

    /* Wait for NIC to process (OWN clears when done) */
    uint32_t spin = 0;
    while (g_tx_desc[0].opts1 & RTL_OWN) {
        rtl_mb();
        if (++spin > 2000000u) {
            kprintf("[rtl8168] TX timeout opts1=0x%08x ISR=0x%04x CR=0x%02x\n",
                    (unsigned)g_tx_desc[0].opts1,
                    (unsigned)r16(RTL_ISR),
                    (unsigned)r8(RTL_CR));
            return false;
        }
    }
    return true;
}

/* ── rtl8168_recv ─────────────────────────────────────────────────────────── */
size_t rtl8168_recv(void *buf, size_t buf_len) {
    if (!g_present) return 0;

    /* Search all descriptors starting from g_rx_cur.
     * If g_rx_cur is OWN=1 but a different descriptor is OWN=0, g_rx_cur has
     * drifted out of sync with the NIC's write pointer — jump to the right one
     * and log it so we can diagnose the underlying cause later.              */
    rtl_desc_t *desc = NULL;
    for (uint16_t scan = 0; scan < RX_RING_SIZE; scan++) {
        uint16_t idx = (uint16_t)((g_rx_cur + scan) & (RX_RING_SIZE - 1u));
        rtl_mb();
        if (g_rx_desc[idx].opts1 & RTL_OWN) continue;   /* NIC still owns */
        if (scan != 0) {
            kprintf("[rtl8168] rx sync: g_rx_cur=%u -> filled desc=%u opts1=0x%08x\n",
                    (unsigned)g_rx_cur, (unsigned)idx,
                    (unsigned)g_rx_desc[idx].opts1);
            g_rx_cur = idx;
        }
        desc = &g_rx_desc[g_rx_cur];
        break;
    }
    if (!desc) return 0;   /* all descriptors still owned by NIC */

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

        /* Log the first frame received — confirms the RX path is alive */
        static bool s_first_rx = true;
        if (s_first_rx) {
            s_first_rx = false;
            kprintf("[rtl8168] first RX: pkt_len=%u frame_len=%u opts1=0x%08x\n",
                    (unsigned)pkt_len, (unsigned)frame_len, (unsigned)opts1);
        }

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

uint16_t rtl8168_isr_rc(void) {
    if (!g_present) return 0;
    uint16_t v = r16(RTL_ISR);
    if (v) w16(RTL_ISR, v);   /* clear by writing 1 to each set bit */
    return v;
}
