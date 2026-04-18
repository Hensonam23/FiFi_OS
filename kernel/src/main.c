#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include "kprintf.h"
#include "io.h"
#include "serial.h"
#include "panic.h"
#include "idt.h"
#include "console.h"
#include "timer.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "pmm.h"
#include "heap.h"
#include "vmm.h"
#include "initrd.h"
#include "vfs.h"
#include "elf.h"
#include "acpi.h"
#include "shell.h"
#include "thread.h"
#include "gdt.h"
#include "virtio_blk.h"
#include "virtio_net.h"
#include "rtl8168.h"
#include "net.h"
#include "ext2.h"
#include "xhci.h"
#include "isr.h"
#include "pci.h"
#include "statusbar.h"
#include "dhcp.h"

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(4);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

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

static void pic_mask_all(void) {
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

void kmain(void) {
    uint64_t hhdm_off = 0;

    __asm__ __volatile__("cli");

    serial_init();
    serial_write("FiFi OS: serial online\n");
    pic_mask_all();

    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision))
        panic("limine base revision not supported");

    if (!framebuffer_request.response ||
        framebuffer_request.response->framebuffer_count < 1)
        panic("no framebuffer available");

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    console_init(fb);
    statusbar_init(fb->width);

    if (!hhdm_request.response)
        panic("HHDM response missing — cannot map physical memory");
    hhdm_off = hhdm_request.response->offset;

    if (memmap_request.response) {
        uint64_t usable = 0;
        for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
            struct limine_memmap_entry *e = memmap_request.response->entries[i];
            if (e->type == LIMINE_MEMMAP_USABLE) usable += e->length;
        }
        serial_write("FiFi OS: memory usable: ");
        /* inline MiB print to serial without kprintf */
        uint64_t mib = usable / (1024 * 1024);
        char mbuf[24]; int mi = 0;
        if (!mib) { mbuf[mi++] = '0'; } else {
            char tmp[20]; int ti = 0;
            while (mib) { tmp[ti++] = (char)('0' + mib % 10); mib /= 10; }
            while (ti > 0) mbuf[mi++] = tmp[--ti];
        }
        mbuf[mi++] = ' '; mbuf[mi++] = 'M'; mbuf[mi++] = 'i';
        mbuf[mi++] = 'B'; mbuf[mi++] = '\n'; mbuf[mi] = '\0';
        serial_write(mbuf);
    }

    pmm_init(memmap_request.response, hhdm_off);
    heap_init();
    vmm_init(hhdm_off);

    idt_init();
    pic_remap(0x20, 0x28);
    pic_disable();
    pic_clear_mask(0);
    pic_clear_mask(1);
    pit_init(100);
    timer_init(100);
    pic_unmask_irq(0);
    pic_unmask_irq(1);
    pic_unmask_irq(2);
    pic_unmask_irq(9);
    __asm__ volatile ("sti");
    keyboard_ps2_init();

    gdt_init();

#ifdef FIFI_PF_TEST
    kprintf("PF test enabled: intentionally reading from NULL...\n");
    volatile uint64_t *p = (volatile uint64_t*)0x0;
    volatile uint64_t xv = *p;
    (void)xv;
#endif

    initrd_init();
    virtio_blk_init();
    virtio_net_init();
    rtl8168_init();
    net_init();
    if (net_nic_present()) dhcp_request();
    ext2_init();
    xhci_init();
    acpi_init();
    thread_init();

    {
        uint64_t t = pmm_get_total_pages(), f = pmm_get_free_pages(), u = pmm_get_used_pages();
        serial_write("FiFi OS: PMM pages total=");
        char pbuf[32]; int pi = 0;
        char tmp[20]; int ti;
        ti = 0; while (t) { tmp[ti++] = (char)('0' + t % 10); t /= 10; } if (!ti) tmp[ti++]='0';
        while (ti > 0) pbuf[pi++] = tmp[--ti];
        pbuf[pi++] = ' '; pbuf[pi++] = 'f'; pbuf[pi++] = 'r'; pbuf[pi++] = 'e'; pbuf[pi++] = 'e'; pbuf[pi++] = '=';
        ti = 0; while (f) { tmp[ti++] = (char)('0' + f % 10); f /= 10; } if (!ti) tmp[ti++]='0';
        while (ti > 0) pbuf[pi++] = tmp[--ti];
        pbuf[pi++] = ' '; pbuf[pi++] = 'u'; pbuf[pi++] = 's'; pbuf[pi++] = 'e'; pbuf[pi++] = 'd'; pbuf[pi++] = '=';
        ti = 0; while (u) { tmp[ti++] = (char)('0' + u % 10); u /= 10; } if (!ti) tmp[ti++]='0';
        while (ti > 0) pbuf[pi++] = tmp[--ti];
        pbuf[pi++] = '\n'; pbuf[pi] = '\0';
        serial_write(pbuf);
    }

    shell_run();
}
