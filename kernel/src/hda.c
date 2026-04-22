/*
 * hda.c — Intel High Definition Audio controller driver.
 *
 * Supports QEMU intel-hda (ICH6, PCI 8086:2668) and generic HDA hardware
 * (PCI class 0x04 / subclass 0x03).
 *
 * Features:
 *   - Immediate Command Interface (ICI) for codec setup
 *   - Single PCM output stream: 48 kHz, 16-bit, stereo
 *   - 4-entry BDL ring (4 KB each = 16 KB, ~85 ms) that loops continuously
 *   - Integer sine generator (Bhaskara I approximation, no FPU)
 *   - hda_play_tone(freq, ms): fill ring with tone, stop after duration
 *   - hda_set_volume(0-100): codec output amp gain
 *   - hda_poll(): called at 100 Hz from pit_on_tick to expire tones
 */

#include <stdint.h>
#include <stdbool.h>
#include "hda.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "pit.h"
#include "kprintf.h"

/* ── HDA MMIO register offsets ────────────────────────────────────────────── */
#define R_GCAP      0x00u   /* 16-bit: global capabilities */
#define R_GCTL      0x08u   /* 32-bit: global control */
#define R_STATESTS  0x0Eu   /* 16-bit: codec wake / state-change status */
#define R_IC        0x60u   /* 32-bit: immediate command (write) */
#define R_IR        0x64u   /* 32-bit: immediate result (read) */
#define R_IRS       0x68u   /* 16-bit: immediate command status */
#define IRS_ICB     (1u << 0)   /* immediate command busy */
#define IRS_IRV     (1u << 1)   /* immediate result valid */

/* Stream descriptor register offsets (relative to stream base) */
#define SD_CTL      0x00u   /* 32-bit: stream control */
#define SD_STS      0x03u   /* 8-bit:  stream status (within CTL word) */
#define SD_LPIB     0x04u   /* 32-bit: link position in buffer */
#define SD_CBL      0x08u   /* 32-bit: cyclic buffer length */
#define SD_LVI      0x0Cu   /* 16-bit: last valid BDL index */
#define SD_FMT      0x12u   /* 16-bit: stream PCM format */
#define SD_BDPL     0x18u   /* 32-bit: BDL physical address (lower 32 bits) */
#define SD_BDPU     0x1Cu   /* 32-bit: BDL physical address (upper 32 bits) */
#define SD_CTL_SRST (1u << 0)   /* stream reset */
#define SD_CTL_RUN  (1u << 1)   /* DMA run */
#define SD_CTL_IOCE (1u << 2)   /* IOC interrupt enable */

/* PCM format word: 48 kHz base, /1 divider, 16-bit, stereo (2 ch) */
#define PCM_FMT     0x0011u
#define PCM_SR      48000
#define PCM_AMPL    28000   /* sine amplitude (< 32767) */

/* BDL ring: 4 entries × 4096 bytes = 16384 bytes = 4096 stereo samples */
#define BDL_N       4u
#define BDL_BYTES   4096u
#define BDL_SAMPS   (BDL_BYTES / 4u)   /* stereo 16-bit = 4 bytes/sample */

/* Virtual address window reserved for HDA MMIO */
#define HDA_MMIO_VIRT 0xFFFFFF0050000000ULL

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint32_t ioc;
} hda_bdl_t;

/* ── Module state ──────────────────────────────────────────────────────────── */
static uint64_t   s_mmio   = 0;
static bool       s_ready  = false;
static uint32_t   s_sdbase = 0x80u;   /* first output stream offset */
static uint32_t   s_cad    = 0;       /* codec address */

static uint64_t   s_bdl_phys = 0;
static hda_bdl_t *s_bdl      = NULL;

static uint64_t s_pcm_phys[BDL_N];
static int16_t *s_pcm_virt[BDL_N];

static volatile int      s_tone_active   = 0;
static volatile uint64_t s_tone_stop_tick = 0;
static volatile int      s_volume        = 50;

/* ── MMIO helpers ──────────────────────────────────────────────────────────── */
static uint8_t  rd8 (uint32_t o) { return *(volatile uint8_t *)(s_mmio+o); }
static uint16_t rd16(uint32_t o) { return *(volatile uint16_t*)(s_mmio+o); }
static uint32_t rd32(uint32_t o) { return *(volatile uint32_t*)(s_mmio+o); }
static void     wr8 (uint32_t o, uint8_t  v){ *(volatile uint8_t *)(s_mmio+o)=v; }
static void     wr16(uint32_t o, uint16_t v){ *(volatile uint16_t*)(s_mmio+o)=v; }
static void     wr32(uint32_t o, uint32_t v){ *(volatile uint32_t*)(s_mmio+o)=v; }

static uint8_t  sdrd8 (uint32_t r){ return rd8 (s_sdbase+r); }
static void     sdwr8 (uint32_t r, uint8_t  v){ wr8 (s_sdbase+r, v); }
static void     sdwr16(uint32_t r, uint16_t v){ wr16(s_sdbase+r, v); }
static void     sdwr32(uint32_t r, uint32_t v){ wr32(s_sdbase+r, v); }

/* ── Busy-wait (≈ µs granularity) ─────────────────────────────────────────── */
static void hda_udelay(uint32_t us) {
    for (uint32_t i = 0; i < us * 500u; i++)
        __asm__ volatile("pause");
}

/* ── Immediate Command Interface ───────────────────────────────────────────── */
static bool ici_cmd(uint32_t cmd, uint32_t *resp_out) {
    int tries = 100000;
    while ((rd16(R_IRS) & IRS_ICB) && tries-- > 0)
        __asm__ volatile("pause");
    if (rd16(R_IRS) & IRS_ICB) return false;

    wr32(R_IC, cmd);
    wr16(R_IRS, IRS_ICB);

    tries = 100000;
    while (tries-- > 0) {
        uint16_t irs = rd16(R_IRS);
        if (!(irs & IRS_ICB)) {
            if (irs & IRS_IRV) {
                if (resp_out) *resp_out = rd32(R_IR);
                wr16(R_IRS, IRS_IRV);
            }
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}

static void codec_set(uint32_t nid, uint32_t verb) {
    ici_cmd((s_cad << 28) | (nid << 20) | verb, NULL);
}

/* ── Integer sine (Bhaskara I, no FPU) ────────────────────────────────────── */
/* Maps x ∈ [0,N] → sin(πx/N) × ampl; accurate to within 0.2% */
static int16_t bhaskara(int32_t x, int32_t N, int32_t ampl) {
    if (N <= 0 || x <= 0 || x >= N) return 0;
    int64_t num = 16LL * x * (N - x);
    int64_t den = (int64_t)5 * N * N - 4LL * x * (N - x);
    return (int16_t)(num * (int64_t)ampl / den);
}

/* One stereo sample for a tone of given period (in samples) at position pos */
static void gen_sample(int32_t pos, int32_t period, int16_t *l, int16_t *r) {
    if (period < 2) { *l = *r = 0; return; }
    int32_t x = pos % period;
    int32_t N = period >> 1;
    int16_t s = (x < N) ? bhaskara(x, N, PCM_AMPL)
                        : (int16_t)-bhaskara(x - N, N, PCM_AMPL);
    *l = *r = s;
}

/* Fill entire BDL ring with tone (freq_hz=0 → silence) */
static void fill_pcm(int freq_hz) {
    int32_t period = (freq_hz > 0) ? (PCM_SR / freq_hz) : 0;
    for (uint32_t b = 0; b < BDL_N; b++) {
        int16_t *buf = s_pcm_virt[b];
        for (uint32_t i = 0; i < BDL_SAMPS; i++) {
            int32_t pos = (int32_t)(b * BDL_SAMPS + i);
            int16_t li, ri;
            gen_sample(pos, period, &li, &ri);
            buf[i * 2 + 0] = li;
            buf[i * 2 + 1] = ri;
        }
    }
}

/* ── Codec output amp ──────────────────────────────────────────────────────── */
static void apply_volume(int vol) {
    /* Map the slider range to the audible gain window.
     * Gain 0-76 is essentially silent on most HDA hardware; start at 77.
     * Slider 0  → hardware mute (silence without wasting the low gain steps)
     * Slider 1  → gain 77  (minimum audible)
     * Slider 100 → gain 127 (maximum, ~same as old 100%) */
    uint32_t amp;
    if (vol <= 0) {
        amp = 0xB080u;   /* output, L+R, mute bit set */
    } else {
        int gain = 77 + (vol * (127 - 77) / 100);
        if (gain > 0x7F) gain = 0x7F;
        amp = 0xB000u | ((uint32_t)gain & 0x7Fu);
    }
    codec_set(2, (0x3u << 16) | amp);   /* DAC (NID 2) */
    codec_set(3, (0x3u << 16) | amp);   /* Pin (NID 3) */
}

/* ── Stream control ────────────────────────────────────────────────────────── */
static void stream_reset(void) {
    sdwr8(SD_CTL, (uint8_t)(sdrd8(SD_CTL) & ~(uint8_t)SD_CTL_RUN));
    hda_udelay(200);
    sdwr8(SD_CTL, (uint8_t)(sdrd8(SD_CTL) |  (uint8_t)SD_CTL_SRST));
    hda_udelay(200);
    sdwr8(SD_CTL, (uint8_t)(sdrd8(SD_CTL) & ~(uint8_t)SD_CTL_SRST));
    hda_udelay(200);
}

static void stream_start(void) {
    sdwr32(SD_CBL,  BDL_N * BDL_BYTES);
    sdwr16(SD_LVI,  (uint16_t)(BDL_N - 1u));
    sdwr16(SD_FMT,  PCM_FMT);
    sdwr32(SD_BDPL, (uint32_t)(s_bdl_phys & 0xFFFFFFFFu));
    sdwr32(SD_BDPU, (uint32_t)(s_bdl_phys >> 32));
    wr8(s_sdbase + 0x03u, 0x1Cu);   /* clear SD_STS */
    /* stream tag = 1 in bits 23:20 of SD_CTL; set RUN */
    sdwr32(SD_CTL, (1u << 20) | (uint32_t)SD_CTL_RUN);
}

/* ── Public API ────────────────────────────────────────────────────────────── */

bool hda_init(void) {
    uint8_t bus, dev, fn;
    if (!pci_find_class(0x04u, 0x03u, 0x00u, &bus, &dev, &fn)) {
        kprintf("[hda] no HDA controller found\n");
        return false;
    }
    kprintf("[hda] found at PCI %u:%u.%u\n",
            (unsigned)bus, (unsigned)dev, (unsigned)fn);
    pci_enable(bus, dev, fn);

    uint64_t mmio_phys = pci_bar_base64(bus, dev, fn, 0);
    if (!mmio_phys) { kprintf("[hda] bad BAR0\n"); return false; }

    if (!vmm_map_range(HDA_MMIO_VIRT, mmio_phys, 0x4000u,
                       VMM_WRITE | VMM_UNCACHE)) {
        kprintf("[hda] MMIO map failed\n"); return false;
    }
    s_mmio = HDA_MMIO_VIRT;

    /* Controller reset */
    wr32(R_GCTL, 0u);
    hda_udelay(500);
    wr32(R_GCTL, 1u);
    for (int t = 0; t < 50000; t++) {
        if (rd32(R_GCTL) & 1u) break;
        hda_udelay(10);
    }
    if (!(rd32(R_GCTL) & 1u)) { kprintf("[hda] reset timeout\n"); return false; }
    hda_udelay(2000);   /* wait for codec enumeration */

    /* Find first codec */
    uint16_t statests = rd16(R_STATESTS);
    s_cad = 0xFFu;
    for (int i = 0; i < 15; i++) {
        if (statests & (1u << i)) { s_cad = (uint32_t)i; break; }
    }
    if (s_cad == 0xFFu) { kprintf("[hda] no codec detected\n"); return false; }
    kprintf("[hda] codec addr=%u\n", (unsigned)s_cad);

    /* First output stream offset: skip input streams (GCAP bits 11:8) */
    uint16_t gcap = rd16(R_GCAP);
    uint32_t iss  = (gcap >> 8) & 0xFu;
    s_sdbase = 0x80u + iss * 0x20u;
    kprintf("[hda] ISS=%u output stream @ 0x%x\n",
            (unsigned)iss, (unsigned)s_sdbase);

    /* Allocate BDL page */
    s_bdl_phys = pmm_alloc_dma32_page();
    if (!s_bdl_phys) { kprintf("[hda] BDL alloc failed\n"); return false; }
    s_bdl = (hda_bdl_t*)pmm_phys_to_virt(s_bdl_phys);

    /* Allocate PCM buffers */
    for (uint32_t i = 0; i < BDL_N; i++) {
        s_pcm_phys[i] = pmm_alloc_dma32_page();
        if (!s_pcm_phys[i]) { kprintf("[hda] PCM buf alloc failed\n"); return false; }
        s_pcm_virt[i] = (int16_t*)pmm_phys_to_virt(s_pcm_phys[i]);
        s_bdl[i].addr = s_pcm_phys[i];
        s_bdl[i].len  = BDL_BYTES;
        s_bdl[i].ioc  = 1u;
    }

    /* Codec init sequence (QEMU hda-output: NID1=AFG, NID2=DAC, NID3=Pin) */
    codec_set(1u, 0x70500u | 0x00u);        /* AFG: power state D0 */
    hda_udelay(100);
    codec_set(2u, (0x2u << 16) | PCM_FMT); /* DAC: set converter format */
    codec_set(2u, 0x70600u | (1u << 4));    /* DAC: stream tag=1, channel=0 */
    codec_set(3u, 0x70700u | 0x40u);        /* Pin: OUT_EN */
    apply_volume(s_volume);

    /* Fill ring with silence and start DMA */
    fill_pcm(0);
    stream_reset();
    stream_start();

    s_ready = true;
    kprintf("[hda] ready (48kHz/16-bit/stereo)\n");
    return true;
}

void hda_play_tone(int freq_hz, int duration_ms) {
    if (!s_ready) return;
    stream_reset();
    fill_pcm(freq_hz);
    stream_start();
    s_tone_active    = 1;
    s_tone_stop_tick = pit_ticks() + (uint64_t)(duration_ms / 10);
}

void hda_stop(void) {
    if (!s_ready) return;
    s_tone_active = 0;
    stream_reset();
    fill_pcm(0);
    stream_start();
}

void hda_set_volume(int vol) {
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    s_volume = vol;
    if (s_ready) apply_volume(vol);
}

int  hda_get_volume(void)  { return s_volume; }
bool hda_is_ready(void)    { return s_ready;  }

void hda_poll(void) {
    if (!s_ready || !s_tone_active) return;
    if (pit_ticks() >= s_tone_stop_tick) {
        s_tone_active = 0;
        stream_reset();
        fill_pcm(0);
        stream_start();
    }
}
