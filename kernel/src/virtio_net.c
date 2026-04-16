/*
 * virtio_net.c — Legacy VirtIO 0.9.5 network driver
 *
 * Uses the transitional PCI I/O BAR interface (vendor 0x1AF4, device 0x1000).
 * Two virtqueues: RX (0) and TX (1).  Poll-based, no MSI/interrupt.
 *
 * Legacy register map (I/O at BAR0) — same layout as virtio_blk:
 *   +0x00  DEVICE_FEATURES   (r)
 *   +0x04  DRIVER_FEATURES   (w)
 *   +0x08  QUEUE_PFN         (r/w)  — queue phys addr >> 12
 *   +0x0C  QUEUE_SIZE        (r)
 *   +0x0E  QUEUE_SELECT      (w)
 *   +0x10  QUEUE_NOTIFY      (w)
 *   +0x12  DEVICE_STATUS     (r/w)
 *   +0x13  ISR_STATUS        (r)
 *   +0x14  device-specific config (MAC bytes 0-5, status)
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "virtio_net.h"
#include "pci.h"
#include "pmm.h"
#include "kprintf.h"
#include "io.h"

/* ── PCI IDs ─────────────────────────────────────────────────────────────── */
#define VIRTIO_VENDOR   0x1AF4u
#define VIRTIO_DEV_NET  0x1000u   /* legacy network device */

/* ── Legacy I/O register offsets ─────────────────────────────────────────── */
#define VREG_DEV_FEAT   0x00
#define VREG_DRV_FEAT   0x04
#define VREG_QUEUE_PFN  0x08
#define VREG_QUEUE_SZ   0x0C
#define VREG_QUEUE_SEL  0x0E
#define VREG_QUEUE_NOT  0x10
#define VREG_STATUS     0x12
#define VREG_ISR        0x13
#define VREG_CFG_BASE   0x14   /* device-specific config starts here */

/* Device status bits */
#define VSTAT_ACK       0x01
#define VSTAT_DRIVER    0x02
#define VSTAT_DRIVER_OK 0x04

/* Feature bits */
#define VNET_F_MAC      (1u << 5)   /* device has a MAC address in config */
#define VNET_F_STATUS   (1u << 16)  /* device supports link status */

/* Queue indices */
#define VQ_RX   0
#define VQ_TX   1

/* ── Virtqueue geometry ───────────────────────────────────────────────────── */
/*
 * VQ_SIZE MUST equal the queue size the device reports via QUEUE_SIZE.
 * QEMU virtio-net (and blk) reports 256.  The device computes ring offsets as:
 *   avail_ring = queue_pfn*4096 + queue_size*16
 *   used_ring  = next page boundary after avail_ring
 * If VQ_SIZE mismatches the device's queue size, ring layouts are misaligned
 * and packets are never processed.
 */
#define VQ_SIZE     256u     /* MUST match device-reported queue size */
#define PAGE_SIZE   4096u

/*
 * Per-queue memory layout for VQ_SIZE=256:
 *   desc table:   256 * 16 = 4096 bytes  (page 0)
 *   avail ring:   4 + 256*2 = 516 bytes  (page 1, offset 4096)
 *   <pad to next page boundary>
 *   used ring:    4 + 256*8 = 2052 bytes (page 2, offset 8192)
 *
 * Total: 3 pages (12 KiB) per queue.
 */
#define DESC_TABLE_SZ   (VQ_SIZE * 16u)
#define AVAIL_RING_OFF  DESC_TABLE_SZ          /* = 4096, on page boundary */
#define USED_RING_OFF   (2u * PAGE_SIZE)       /* = 8192, on page boundary */
#define VQ_PAGES        3u                     /* 12 KiB per queue */

/* ── Virtqueue structs ───────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vring_desc_t;

#define VRING_DESC_F_NEXT   1u
#define VRING_DESC_F_WRITE  2u

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VQ_SIZE];
} vring_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} vring_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t          flags;
    uint16_t          idx;
    vring_used_elem_t ring[VQ_SIZE];
} vring_used_t;

/*
 * virtio-net prepends a 12-byte header to every packet.
 * In legacy mode with no CSUM/GSO features negotiated, all fields are zero.
 */
typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    /* num_buffers field only present when VIRTIO_NET_F_MRG_RXBUF is negotiated */
} virtio_net_hdr_t;

#define NET_HDR_SZ  sizeof(virtio_net_hdr_t)   /* 10 bytes */

/* Max Ethernet frame: 14 header + 1500 payload */
#define MAX_FRAME   1514u
/* Buffer size for RX: net header + max frame */
#define RX_BUF_SZ   (NET_HDR_SZ + MAX_FRAME)

/*
 * Number of RX buffers to keep posted.
 * Each occupies 2 descriptors (header + data).
 * Keep 4 — enough for bursts in QEMU.
 */
#define RX_BUFS     4u

/* ── Per-queue state ─────────────────────────────────────────────────────── */
typedef struct {
    vring_desc_t  *desc;
    vring_avail_t *avail;
    vring_used_t  *used;
    uint16_t       avail_idx;   /* shadow of avail->idx (next to produce) */
    uint16_t       last_used;   /* next used->idx we haven't consumed yet */
    uint16_t       free_head;   /* head of free descriptor list */
} vq_t;

/* ── Driver state ─────────────────────────────────────────────────────────── */
static bool     g_present = false;
static uint16_t g_iobase  = 0;
static uint8_t  g_mac[6]  = {0};

static vq_t g_rx, g_tx;

/*
 * RX buffer pool — each RX buffer is one PMM page split into:
 *   [0..9]    virtio_net_hdr_t  (10 bytes, written by device)
 *   [10..1523] Ethernet frame    (up to 1514 bytes, written by device)
 */
static uint64_t g_rx_phys[RX_BUFS];   /* physical addresses */
static uint8_t *g_rx_virt[RX_BUFS];   /* virtual addresses */
static uint16_t g_rx_desc[RX_BUFS];   /* which descriptor heads these use */

/* TX scratch: one page shared for header + frame.  TX is synchronous. */
static uint64_t g_tx_phys = 0;
static uint8_t *g_tx_virt = 0;

/* ── I/O helpers ──────────────────────────────────────────────────────────── */
static inline uint32_t vio_r32(uint16_t off) { return inl((uint16_t)(g_iobase + off)); }
static inline uint16_t vio_r16(uint16_t off) { return inw((uint16_t)(g_iobase + off)); }
static inline uint8_t  vio_r8 (uint16_t off) { return inb((uint16_t)(g_iobase + off)); }
static inline void vio_w32(uint16_t off, uint32_t v) { outl((uint16_t)(g_iobase + off), v); }
static inline void vio_w16(uint16_t off, uint16_t v) { outw((uint16_t)(g_iobase + off), v); }
static inline void vio_w8 (uint16_t off, uint8_t  v) { outb((uint16_t)(g_iobase + off), v); }

#define vio_mb() __asm__ __volatile__("mfence" ::: "memory")

/* ── Virtqueue setup ─────────────────────────────────────────────────────── */
static bool vq_setup(vq_t *vq, uint16_t qidx) {
    vio_w16(VREG_QUEUE_SEL, qidx);
    uint16_t qsz = vio_r16(VREG_QUEUE_SZ);
    if (qsz == 0) {
        kprintf("[virtio-net] queue %u size=0\n", (unsigned)qidx);
        return false;
    }
    if (qsz != VQ_SIZE) {
        kprintf("[virtio-net] WARNING: queue %u device size %u != VQ_SIZE %u — ring layout mismatch\n",
                (unsigned)qidx, (unsigned)qsz, (unsigned)VQ_SIZE);
        /* Mismatch is fatal: continue anyway, but TX/RX likely broken */
    }

    uint64_t phys = pmm_alloc_pages(VQ_PAGES);
    if (!phys) return false;

    uint8_t *virt = (uint8_t *)pmm_phys_to_virt(phys);
    for (size_t i = 0; i < VQ_PAGES * PAGE_SIZE; i++) virt[i] = 0;

    vq->desc      = (vring_desc_t  *)virt;
    vq->avail     = (vring_avail_t *)(virt + AVAIL_RING_OFF);
    vq->used      = (vring_used_t  *)(virt + USED_RING_OFF);
    vq->avail_idx = 0;
    vq->last_used = 0;
    vq->free_head = 0;

    /* Build free descriptor chain */
    for (uint16_t i = 0; i < VQ_SIZE - 1; i++) {
        vq->desc[i].flags = VRING_DESC_F_NEXT;
        vq->desc[i].next  = i + 1;
    }
    vq->desc[VQ_SIZE - 1].flags = 0;
    vq->desc[VQ_SIZE - 1].next  = 0;

    /* Register with device */
    vio_w32(VREG_QUEUE_PFN, (uint32_t)(phys / PAGE_SIZE));
    return true;
}

/* Alloc 'n' descriptors from a queue's free list. Returns head index. */
static uint16_t vq_alloc(vq_t *vq, uint16_t n) {
    uint16_t head = vq->free_head;
    uint16_t cur  = head;
    for (uint16_t i = 0; i < n - 1; i++) {
        cur = vq->desc[cur].next;
    }
    vq->free_head = vq->desc[cur].next;
    vq->desc[cur].flags &= ~(uint16_t)VRING_DESC_F_NEXT;  /* tail */
    return head;
}

/* Return descriptor chain starting at 'head' back to free list. */
static void vq_free(vq_t *vq, uint16_t head) {
    uint16_t cur = head;
    while (vq->desc[cur].flags & VRING_DESC_F_NEXT)
        cur = vq->desc[cur].next;
    vq->desc[cur].next  = vq->free_head;
    vq->desc[cur].flags |= VRING_DESC_F_NEXT;
    vq->free_head = head;
}

/* Post descriptor chain to available ring and notify device. */
static void vq_submit(vq_t *vq, uint16_t head, uint16_t qidx) {
    uint16_t slot = vq->avail_idx & (VQ_SIZE - 1);
    vq->avail->ring[slot] = head;
    vio_mb();
    vq->avail->idx = ++vq->avail_idx;
    vio_mb();
    vio_w16(VREG_QUEUE_NOT, qidx);
}

/* ── RX buffer management ────────────────────────────────────────────────── */

/* Post one RX buffer (buf index i) into the RX queue. */
static void rx_post(uint32_t i) {
    /* 2 descriptors: one for net header, one for frame data.
     * Both are device-writable. We put them in a single contiguous page
     * so one descriptor covers the whole thing — simpler and equally valid. */
    uint16_t d = vq_alloc(&g_rx, 1);

    g_rx.desc[d].addr  = g_rx_phys[i];
    g_rx.desc[d].len   = (uint32_t)RX_BUF_SZ;
    g_rx.desc[d].flags = VRING_DESC_F_WRITE;
    g_rx.desc[d].next  = 0;

    g_rx_desc[i] = d;
    vq_submit(&g_rx, d, VQ_RX);
}

/* ── init ─────────────────────────────────────────────────────────────────── */
bool virtio_net_init(void) {
    uint8_t bus, dev, fn;
    if (!pci_find(VIRTIO_VENDOR, VIRTIO_DEV_NET, &bus, &dev, &fn)) {
        kprintf("[virtio-net] no device found\n");
        return false;
    }
    kprintf("[virtio-net] found at PCI %u:%u.%u\n",
            (unsigned)bus, (unsigned)dev, (unsigned)fn);

    pci_enable(bus, dev, fn);

    bool is_io = false;
    uint32_t bar0 = pci_bar_base(bus, dev, fn, 0, &is_io);
    if (!is_io || bar0 == 0) {
        kprintf("[virtio-net] BAR0 is not I/O\n");
        return false;
    }
    g_iobase = (uint16_t)bar0;
    kprintf("[virtio-net] I/O base=0x%x\n", (unsigned)g_iobase);

    /* 1. Reset */
    vio_w8(VREG_STATUS, 0);

    /* 2. ACK + DRIVER */
    vio_w8(VREG_STATUS, VSTAT_ACK | VSTAT_DRIVER);

    /* 3. Read features, negotiate: only ask for MAC address */
    uint32_t dev_feat = vio_r32(VREG_DEV_FEAT);
    uint32_t drv_feat = 0;
    if (dev_feat & VNET_F_MAC) drv_feat |= VNET_F_MAC;
    vio_w32(VREG_DRV_FEAT, drv_feat);
    kprintf("[virtio-net] dev_feat=0x%x negotiated=0x%x\n",
            (unsigned)dev_feat, (unsigned)drv_feat);

    /* 4. Set up RX and TX queues */
    if (!vq_setup(&g_rx, VQ_RX)) return false;
    if (!vq_setup(&g_tx, VQ_TX)) return false;

    /* 5. Allocate TX scratch page */
    g_tx_phys = pmm_alloc_page();
    if (!g_tx_phys) return false;
    g_tx_virt = (uint8_t *)pmm_phys_to_virt(g_tx_phys);
    for (size_t i = 0; i < PAGE_SIZE; i++) g_tx_virt[i] = 0;

    /* 6. Allocate RX buffers and post them */
    for (uint32_t i = 0; i < RX_BUFS; i++) {
        g_rx_phys[i] = pmm_alloc_page();
        if (!g_rx_phys[i]) return false;
        g_rx_virt[i] = (uint8_t *)pmm_phys_to_virt(g_rx_phys[i]);
        for (size_t j = 0; j < PAGE_SIZE; j++) g_rx_virt[i][j] = 0;
        rx_post(i);
    }

    /* 7. DRIVER_OK */
    vio_w8(VREG_STATUS, VSTAT_ACK | VSTAT_DRIVER | VSTAT_DRIVER_OK);

    /* 8. Read MAC from config (bytes 0-5 at VREG_CFG_BASE) */
    if (drv_feat & VNET_F_MAC) {
        for (int i = 0; i < 6; i++)
            g_mac[i] = vio_r8((uint16_t)(VREG_CFG_BASE + i));
    }
    kprintf("[virtio-net] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            (unsigned)g_mac[0], (unsigned)g_mac[1], (unsigned)g_mac[2],
            (unsigned)g_mac[3], (unsigned)g_mac[4], (unsigned)g_mac[5]);

    g_present = true;
    return true;
}

/* ── send ─────────────────────────────────────────────────────────────────── */
bool virtio_net_send(const void *frame, size_t len) {
    if (!g_present) return false;
    if (len == 0 || len > MAX_FRAME) return false;

    /*
     * TX buffer layout in g_tx_virt:
     *   [0 .. NET_HDR_SZ-1]  virtio_net_hdr_t (all zeros — no csum/gso)
     *   [NET_HDR_SZ .. end]  Ethernet frame
     *
     * Use a single descriptor covering the whole thing.
     */
    for (size_t i = 0; i < NET_HDR_SZ; i++) g_tx_virt[i] = 0;
    const uint8_t *src = (const uint8_t *)frame;
    for (size_t i = 0; i < len; i++) g_tx_virt[NET_HDR_SZ + i] = src[i];

    uint16_t d = vq_alloc(&g_tx, 1);
    g_tx.desc[d].addr  = g_tx_phys;
    g_tx.desc[d].len   = (uint32_t)(NET_HDR_SZ + len);
    g_tx.desc[d].flags = 0;   /* device reads, not writes */
    g_tx.desc[d].next  = 0;

    vq_submit(&g_tx, d, VQ_TX);

    /* Wait for TX completion (device returns descriptor to used ring) */
    uint32_t spin = 0;
    while (g_tx.used->idx == g_tx.last_used) {
        vio_mb();
        if (++spin > 5000000u) {
            kprintf("[virtio-net] TX timeout\n");
            vq_free(&g_tx, d);
            return false;
        }
    }
    g_tx.last_used++;
    vq_free(&g_tx, d);
    return true;
}

/* ── recv ─────────────────────────────────────────────────────────────────── */
size_t virtio_net_recv(void *buf, size_t buf_len) {
    if (!g_present) return 0;

    vio_mb();
    if (g_rx.used->idx == g_rx.last_used) return 0;   /* nothing ready */

    /* Consume one used entry */
    uint16_t slot = g_rx.last_used & (VQ_SIZE - 1);
    vring_used_elem_t ue = g_rx.used->ring[slot];
    g_rx.last_used++;

    /* Figure out which RX buffer this was */
    uint16_t d_head = (uint16_t)ue.id;
    uint32_t buf_idx = (uint32_t)RX_BUFS;
    for (uint32_t i = 0; i < RX_BUFS; i++) {
        if (g_rx_desc[i] == d_head) { buf_idx = i; break; }
    }
    if (buf_idx >= RX_BUFS) {
        /* Unexpected descriptor — re-post something and bail */
        rx_post(0);
        return 0;
    }

    /* ue.len includes the virtio-net header; strip it */
    uint32_t total = ue.len;
    size_t frame_len = (total > NET_HDR_SZ) ? total - NET_HDR_SZ : 0;
    if (frame_len > buf_len) frame_len = buf_len;

    /* Copy frame out (skip the 10-byte virtio header) */
    const uint8_t *src = g_rx_virt[buf_idx] + NET_HDR_SZ;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < frame_len; i++) dst[i] = src[i];

    /* Re-post this buffer so the device can use it again */
    rx_post(buf_idx);

    return frame_len;
}

/* ── accessors ───────────────────────────────────────────────────────────── */
void virtio_net_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = g_mac[i];
}

bool virtio_net_present(void) { return g_present; }
