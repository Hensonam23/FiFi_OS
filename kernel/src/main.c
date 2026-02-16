#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>

#include "io.h"
#include "serial.h"
#include "panic.h"
#include "idt.h"

/* Base revision */
__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(4);

/* Request a framebuffer */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

/* Required request markers */
__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

/* Minimal libc bits */
void *memcpy(void *restrict dest, const void *restrict src, size_t n) {
    uint8_t *restrict d = (uint8_t *)dest;
    const uint8_t *restrict s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}
void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}
void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (s > d) for (size_t i = 0; i < n; i++) d[i] = s[i];
    else if (s < d) for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    return dest;
}
int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = (const uint8_t *)s1;
    const uint8_t *b = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

__attribute__((noreturn))
static void hcf(void) { for (;;) __asm__ __volatile__("hlt"); }

static void pic_mask_all(void) {
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

__attribute__((noreturn))
void kmain(void) {
    __asm__ __volatile__("cli");

    serial_init();
    serial_write("FiFi OS: serial online\n");
    serial_write("FiFi OS: interrupts disabled (cli)\n");

    pic_mask_all();
    serial_write("FiFi OS: PIC masked (no hardware IRQs)\n");

    serial_write("FiFi OS: checking limine base revision...\n");
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        panic("limine base revision not supported");
    }
    serial_write("FiFi OS: base revision OK\n");

    serial_write("FiFi OS: checking framebuffer...\n");
    if (!framebuffer_request.response ||
        framebuffer_request.response->framebuffer_count < 1) {
        panic("no framebuffer available");
    }
    serial_write("FiFi OS: framebuffer OK\n");

    serial_write("FiFi OS: calling idt_init...\n");
    idt_init();
    serial_write("FiFi OS: IDT loaded\n");

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    volatile uint32_t *pix = (volatile uint32_t *)fb->address;
    uint64_t pitch32 = fb->pitch / 4;
    uint64_t w = fb->width;
    uint64_t h = fb->height;

    for (uint64_t y = 0; y < h; y++)
        for (uint64_t x = 0; x < w; x++)
            pix[y * pitch32 + x] = 0x00202020;

    uint64_t box = 240;
    uint64_t sx = (w > box) ? (w - box) / 2 : 0;
    uint64_t sy = (h > box) ? (h - box) / 2 : 0;

    for (uint64_t y = 0; y < box && (sy + y) < h; y++)
        for (uint64_t x = 0; x < box && (sx + x) < w; x++)
            pix[(sy + y) * pitch32 + (sx + x)] = 0x00FF00FF;

    serial_write("FiFi OS: framebuffer test drawn\n");
    hcf();
}
