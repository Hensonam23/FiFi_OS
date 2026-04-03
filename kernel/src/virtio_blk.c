/*
 * virtio_blk.c — Legacy VirtIO 0.9.5 block device driver
 *
 * Uses the transitional PCI I/O BAR interface (vendor 0x1AF4, device 0x1001).
 * Queue depth = 64 descriptors.  Polling only (no MSI/interrupt).
 *
 * Legacy register map (I/O at BAR0):
 *   +0x00  DEVICE_FEATURES   (r)
 *   +0x04  DRIVER_FEATURES   (w)
 *   +0x08  QUEUE_PFN         (r/w)  — queue phys addr >> 12
 *   +0x0C  QUEUE_SIZE        (r)
 *   +0x0E  QUEUE_SELECT      (w)
 *   +0x10  QUEUE_NOTIFY      (w)
 *   +0x12  DEVICE_STATUS     (r/w)
 *   +0x13  ISR_STATUS        (r, clears on read)
 *   +0x14  device-specific config (blk_size, capacity, ...)
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "virtio_blk.h"
#include "pci.h"
#include "pmm.h"
#include "kprintf.h"
#include "io.h"

/* ── VirtIO PCI IDs ───────────────────────────────────────────────────────── */
#define VIRTIO_VENDOR   0x1AF4u
#define VIRTIO_DEV_BLK  0x1001u   /* legacy block device */

/* ── Legacy I/O register offsets ─────────────────────────────────────────── */
#define VREG_DEV_FEAT   0x00
#define VREG_DRV_FEAT   0x04
#define VREG_QUEUE_PFN  0x08
#define VREG_QUEUE_SZ   0x0C
#define VREG_QUEUE_SEL  0x0E
#define VREG_QUEUE_NOT  0x10
#define VREG_STATUS     0x12
#define VREG_ISR        0x13
/* device-specific config starts at 0x14 (legacy) or 0x18 (with MSI) */
#define VREG_CFG_BASE   0x14

/* Device status bits */
#define VSTAT_ACK       0x01
#define VSTAT_DRIVER    0x02
#define VSTAT_DRIVER_OK 0x04
#define VSTAT_FAILED    0x80

/* Feature bits we care about (none required for basic block I/O) */
#define VFEAT_BLK_SIZE  (1u << 6)

/* ── Virtqueue geometry ───────────────────────────────────────────────────── */
#define VQ_SIZE     256u     /* number of descriptors — MUST match device's queue size */
#define PAGE_SIZE   4096u

/*
 * Virtqueue memory layout for VQ_SIZE=256 (one contiguous phys allocation):
 *
 *   offset 0:              descriptor table  — 256*16 = 4096 bytes
 *   offset 4096:           available ring    — 6 + 256*2 = 518 bytes
 *   <pad to next page>     (4096+518=4614, next page at 8192)
 *   offset 8192:           used ring         — 6 + 256*8 = 2054 bytes
 *
 * Total: 8192 + 2054 = 10246 bytes → 3 pages (12 KiB).
 *
 * IMPORTANT: AVAIL_RING_OFF = DESC_TABLE_SZ = VQ_SIZE*16.
 * The device computes: avail_ring = queue_pfn*4096 + queue_size*16.
 * VQ_SIZE must equal the value the device reports via QUEUE_SIZE register.
 */
#define DESC_TABLE_SZ   (VQ_SIZE * 16u)
#define AVAIL_RING_SZ   (6u + VQ_SIZE * 2u)
#define AVAIL_RING_OFF  DESC_TABLE_SZ                /* = VQ_SIZE * 16 = 4096 */
#define USED_RING_OFF   (2u * PAGE_SIZE)             /* starts on page 2 */
#define USED_RING_SZ    (6u + VQ_SIZE * 8u)
#define VQ_PAGES        3u                           /* 12 KiB total */

/* ── Virtqueue descriptor (16 bytes) ─────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t addr;   /* physical address of buffer */
    uint32_t len;    /* length in bytes */
    uint16_t flags;  /* VRING_DESC_F_* */
    uint16_t next;   /* next descriptor index (if F_NEXT) */
} vring_desc_t;

#define VRING_DESC_F_NEXT    1u
#define VRING_DESC_F_WRITE   2u   /* device writes to this buffer */

/* ── Available ring ───────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[256];
    uint16_t used_event;  /* optional, ignored */
} vring_avail_t;

/* ── Used ring ────────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t id;  /* descriptor chain head index */
    uint32_t len; /* bytes written by device */
} vring_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t         flags;
    uint16_t         idx;
    vring_used_elem_t ring[256];
    uint16_t         avail_event; /* optional, ignored */
} vring_used_t;

/* ── VirtIO block request header ──────────────────────────────────────────── */
#define VIRTIO_BLK_T_IN    0u   /* read */
#define VIRTIO_BLK_T_OUT   1u   /* write */

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} virtio_blk_req_hdr_t;

/* Status byte written by the device */
#define VIRTIO_BLK_S_OK    0u
#define VIRTIO_BLK_S_IOERR 1u
#define VIRTIO_BLK_S_UNSUPP 2u

/* ── Driver state ─────────────────────────────────────────────────────────── */
static bool     g_present   = false;
static uint16_t g_iobase    = 0;
static uint64_t g_capacity  = 0;   /* total sectors */

/* Virtual queue pointers into the phys-alloc region */
static vring_desc_t  *g_desc  = 0;
static vring_avail_t *g_avail = 0;
static vring_used_t  *g_used  = 0;

/* Free descriptor ring (trivial: we only ever use 3 at a time) */
static uint16_t g_free_head = 0;   /* next free descriptor index */
static uint16_t g_avail_idx = 0;   /* shadow of avail->idx */
static uint16_t g_last_used = 0;   /* shadow of used->idx we've consumed */

/*
 * Request scratch pages — allocated from PMM so we have known physical
 * addresses.  Kernel .data/.bss variables sit at 0xFFFFFFFF80... (kernel
 * image VA) and pmm_virt_to_phys() cannot translate them (it only works for
 * HHDM addresses at 0xFFFF800...).
 *
 * Page layout (1 page each):
 *   g_req_page:  [0..11]  virtio_blk_req_hdr_t
 *                [12]     status byte (written by device)
 *   g_data_page: 4 KiB bounce buffer — up to 8 sectors per request
 */
#define MAX_SECTORS_PER_REQ 8u

static uint64_t g_req_phys  = 0;   /* physical address of request page */
static uint64_t g_data_phys = 0;   /* physical address of data page */

/* ── I/O helpers ──────────────────────────────────────────────────────────── */
static inline uint32_t vio_r32(uint16_t off) { return inl((uint16_t)(g_iobase + off)); }
static inline uint16_t vio_r16(uint16_t off) { return inw((uint16_t)(g_iobase + off)); }
static inline __attribute__((unused))
uint8_t vio_r8(uint16_t off) { return inb((uint16_t)(g_iobase + off)); }
static inline void vio_w32(uint16_t off, uint32_t v) { outl((uint16_t)(g_iobase + off), v); }
static inline void vio_w16(uint16_t off, uint16_t v) { outw((uint16_t)(g_iobase + off), v); }
static inline void vio_w8 (uint16_t off, uint8_t  v) { outb((uint16_t)(g_iobase + off), v); }

/* Memory barrier — prevents compiler/CPU reordering across virtqueue ops */
#define vio_mb() __asm__ __volatile__("mfence" ::: "memory")

/* ── init ─────────────────────────────────────────────────────────────────── */
bool virtio_blk_init(void) {
    uint8_t bus, dev, fn;
    if (!pci_find(VIRTIO_VENDOR, VIRTIO_DEV_BLK, &bus, &dev, &fn)) {
        kprintf("[virtio-blk] no device found\n");
        return false;
    }
    kprintf("[virtio-blk] found at PCI %u:%u.%u\n", bus, dev, fn);

    pci_enable(bus, dev, fn);

    bool is_io = false;
    uint32_t bar0 = pci_bar_base(bus, dev, fn, 0, &is_io);
    if (!is_io || bar0 == 0) {
        kprintf("[virtio-blk] BAR0 is not I/O or zero (bar0=%p)\n", (void*)(uintptr_t)bar0);
        return false;
    }
    g_iobase = (uint16_t)bar0;
    kprintf("[virtio-blk] I/O base=0x%x\n", (unsigned)g_iobase);

    /* 1. Reset device */
    vio_w8(VREG_STATUS, 0);

    /* 2. Acknowledge + Driver */
    vio_w8(VREG_STATUS, VSTAT_ACK | VSTAT_DRIVER);

    /* 3. Read and accept device features (we don't require any) */
    uint32_t dev_feat = vio_r32(VREG_DEV_FEAT);
    (void)dev_feat;
    vio_w32(VREG_DRV_FEAT, 0);  /* negotiate no optional features */

    /* 4. Set up virtqueue 0 */
    vio_w16(VREG_QUEUE_SEL, 0);
    uint16_t qsz = vio_r16(VREG_QUEUE_SZ);
    if (qsz == 0) {
        kprintf("[virtio-blk] queue size 0\n");
        return false;
    }
    kprintf("[virtio-blk] queue size=%u\n", (unsigned)qsz);
    if (qsz != VQ_SIZE) {
        kprintf("[virtio-blk] WARNING: device queue size %u != VQ_SIZE %u; "
                "avail ring offset will mismatch — update VQ_SIZE\n",
                (unsigned)qsz, (unsigned)VQ_SIZE);
        /* Continue anyway; will likely fail, but at least we'll know why */
    }

    /* Allocate physically contiguous pages for the virtqueue */
    uint64_t vq_phys = pmm_alloc_pages(VQ_PAGES);
    if (!vq_phys) {
        kprintf("[virtio-blk] vq alloc failed\n");
        return false;
    }
    /* Zero the entire region */
    uint8_t *vq_virt = (uint8_t*)pmm_phys_to_virt(vq_phys);
    for (size_t i = 0; i < VQ_PAGES * PAGE_SIZE; i++) vq_virt[i] = 0;

    g_desc  = (vring_desc_t*)vq_virt;
    g_avail = (vring_avail_t*)(vq_virt + AVAIL_RING_OFF);
    g_used  = (vring_used_t *)(vq_virt + USED_RING_OFF);

    /* Tell device the queue's physical address (in page units) */
    vio_w32(VREG_QUEUE_PFN, (uint32_t)(vq_phys / PAGE_SIZE));

    /* 5. Initialise free descriptor chain (simple: 0→1→2→...→VQ_SIZE-1) */
    for (uint16_t i = 0; i < VQ_SIZE - 1; i++) {
        g_desc[i].flags = VRING_DESC_F_NEXT;
        g_desc[i].next  = i + 1;
    }
    g_desc[VQ_SIZE - 1].flags = 0;
    g_desc[VQ_SIZE - 1].next  = 0;
    g_free_head = 0;

    /* 5b. Allocate request scratch pages (must be in HHDM for physical translation) */
    g_req_phys = pmm_alloc_page();
    g_data_phys = pmm_alloc_page();
    if (!g_req_phys || !g_data_phys) {
        kprintf("[virtio-blk] scratch page alloc failed\n");
        return false;
    }
    /* Zero both pages */
    uint8_t *rp = (uint8_t*)pmm_phys_to_virt(g_req_phys);
    uint8_t *dp = (uint8_t*)pmm_phys_to_virt(g_data_phys);
    for (size_t i = 0; i < PAGE_SIZE; i++) { rp[i] = 0; dp[i] = 0; }

    /* 6. Driver OK */
    vio_w8(VREG_STATUS, VSTAT_ACK | VSTAT_DRIVER | VSTAT_DRIVER_OK);

    /* 7. Read disk capacity from device config */
    /* Legacy config: capacity is at offset 0x14, 64-bit LE */
    uint32_t cap_lo = vio_r32(VREG_CFG_BASE + 0);
    uint32_t cap_hi = vio_r32(VREG_CFG_BASE + 4);
    g_capacity = ((uint64_t)cap_hi << 32) | cap_lo;
    kprintf("[virtio-blk] capacity=%p sectors (%p MiB)\n",
            (void*)g_capacity,
            (void*)(g_capacity / 2048));

    g_present = true;
    return true;
}

/* ── submit one request (polling, synchronous) ───────────────────────────── */
/*
 * data_buf: caller's kernel virtual address (any mapping).
 * data_len: bytes (must be multiple of 512, max MAX_SECTORS_PER_REQ*512).
 * For reads the device writes into the bounce page, then we copy out.
 * For writes we copy in first, then the device reads the bounce page.
 */
static bool vq_do_request(uint32_t type, uint64_t sector,
                          void *data_buf, size_t data_len) {
    if (!g_present) return false;
    if (data_len == 0 || data_len % 512 != 0) return false;
    if (data_len > MAX_SECTORS_PER_REQ * 512u) return false;

    /* Scratch page virtual pointers (in HHDM, so physical addr = virt - hhdm) */
    virtio_blk_req_hdr_t *hdr    = (virtio_blk_req_hdr_t*)pmm_phys_to_virt(g_req_phys);
    volatile uint8_t     *status = (volatile uint8_t*)pmm_phys_to_virt(g_req_phys)
                                    + sizeof(virtio_blk_req_hdr_t);
    uint8_t              *dbuf   = (uint8_t*)pmm_phys_to_virt(g_data_phys);

    hdr->type     = type;
    hdr->reserved = 0;
    hdr->sector   = sector;
    *status       = 0xFF;   /* sentinel */

    /* For writes, copy caller data into bounce buffer first */
    if (type == VIRTIO_BLK_T_OUT) {
        const uint8_t *src = (const uint8_t*)data_buf;
        for (size_t i = 0; i < data_len; i++) dbuf[i] = src[i];
    }

    /* Allocate 3 descriptors from the free list */
    uint16_t d_hdr    = g_free_head;
    uint16_t d_data   = g_desc[d_hdr].next;
    uint16_t d_status = g_desc[d_data].next;
    g_free_head       = g_desc[d_status].next;

    /* Physical addresses — all in HHDM, so translation is straightforward */
    uint64_t hdr_phys    = g_req_phys;
    uint64_t status_phys = g_req_phys + sizeof(virtio_blk_req_hdr_t);
    uint64_t data_phys   = g_data_phys;

    /* Header descriptor — device reads it */
    g_desc[d_hdr].addr  = hdr_phys;
    g_desc[d_hdr].len   = sizeof(virtio_blk_req_hdr_t);
    g_desc[d_hdr].flags = VRING_DESC_F_NEXT;
    g_desc[d_hdr].next  = d_data;

    /* Data descriptor */
    g_desc[d_data].addr  = data_phys;
    g_desc[d_data].len   = (uint32_t)data_len;
    g_desc[d_data].flags = VRING_DESC_F_NEXT |
                           (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
    g_desc[d_data].next  = d_status;

    /* Status descriptor — device writes 1 byte */
    g_desc[d_status].addr  = status_phys;
    g_desc[d_status].len   = 1;
    g_desc[d_status].flags = VRING_DESC_F_WRITE;
    g_desc[d_status].next  = 0;

    /* Publish to available ring */
    uint16_t avail_slot = g_avail_idx & (VQ_SIZE - 1);
    g_avail->ring[avail_slot] = d_hdr;
    vio_mb();
    g_avail->idx = ++g_avail_idx;
    vio_mb();

    /* Kick the device (queue 0) */
    vio_w16(VREG_QUEUE_NOT, 0);

    /* Poll used ring until the device completes */
    uint32_t spin = 0;
    while (g_used->idx == g_last_used) {
        vio_mb();
        if (++spin > 10000000u) {
            kprintf("[virtio-blk] timeout waiting for completion\n");
            return false;
        }
    }
    g_last_used++;

    /* Return descriptors to free list */
    g_desc[d_status].next = g_free_head;
    g_free_head           = d_hdr;

    if (*status != VIRTIO_BLK_S_OK) {
        kprintf("[virtio-blk] device returned status=%u\n", (unsigned)*status);
        return false;
    }

    /* For reads, copy bounce buffer out to caller */
    if (type == VIRTIO_BLK_T_IN) {
        uint8_t *dst = (uint8_t*)data_buf;
        for (size_t i = 0; i < data_len; i++) dst[i] = dbuf[i];
    }

    return true;
}

/* ── public API ───────────────────────────────────────────────────────────── */

bool virtio_blk_read(uint64_t sector, void *buf, size_t count) {
    return vq_do_request(VIRTIO_BLK_T_IN, sector, buf, count * 512);
}

bool virtio_blk_write(uint64_t sector, const void *buf, size_t count) {
    return vq_do_request(VIRTIO_BLK_T_OUT, sector, (void*)buf, count * 512);
}

uint64_t virtio_blk_sector_count(void) { return g_capacity; }
bool     virtio_blk_present(void)      { return g_present;  }
