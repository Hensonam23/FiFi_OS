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
/* Base revision */
__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(4);

/* Request a framebuffer */
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

/* Required request markers */
__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

/* Minimal libc bits (temporary; we will move these into their own file soon) */
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
    initrd_init();
    initrd_cat("motd.txt");
struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    console_init(fb);

    /* FiFi OS: memmap summary (after console_init so kprintf works) */
    console_write("\nFiFi OS: memmap summary...\n");
    if (!memmap_request.response) {
        console_write("FiFi OS: memmap missing\n");
    } else {
        uint64_t usable = 0;
        uint64_t count = memmap_request.response->entry_count;

        for (uint64_t i = 0; i < count; i++) {
            struct limine_memmap_entry *e = memmap_request.response->entries[i];
            if (e->type == LIMINE_MEMMAP_USABLE) {
                usable += e->length;
            }
        }

        kprintf("Memory usable: %p MiB (entries=%p)\n",
                (void*)(usable / (1024 * 1024)),
                (void*)count);
    }

    kprintf("kprintf test: num=%d hex=%x ptr=%p str=%s char=%c\n",
        -123, 0xBEEF, (void*)0xFFFFFFFF80000000ULL, "ok", '!');
    console_write("FiFi OS framebuffer console online\n");
    console_write("Real font enabled. Scrolling enabled.\n\n");

    /* FiFi OS: hhdm offset */
    serial_write("FiFi OS: hhdm offset...\n");
    if (!hhdm_request.response) {
        serial_write("FiFi OS: hhdm missing\n");
    } else {

        kprintf("FiFi OS: hhdm offset=0x%llx\n", (unsigned long long)hhdm_request.response->offset);
    }

    /* FiFi OS: init PMM (simple bump allocator) */
    uint64_t hhdm_off = 0;
    if (hhdm_request.response) {
        hhdm_off = hhdm_request.response->offset;
    }
    pmm_init(memmap_request.response, hhdm_off);
        kprintf("FiFi OS: ACPI autoinit disabled (boot is stable).\n");
        kprintf("FiFi OS: (Re-enable later after ACPI uses HHDM-safe pointers.)\n");
heap_init();
    vmm_init(hhdm_off);
    /* VMM map test */
    uint64_t test_phys = pmm_alloc_page();
    uint64_t test_virt = 0xFFFF900000000000ULL;
    if (!test_phys) {
        serial_write("FiFi OS: VMM test alloc failed\n");
    } else {
        bool ok = vmm_map_page(test_virt, test_phys, (1ULL<<1)); /* RW */
        if (!ok) {
            serial_write("FiFi OS: VMM map failed\n");
        } else {
            volatile uint64_t *x = (volatile uint64_t*)test_virt;
            *x = 0x1122334455667788ULL;
            kprintf("VMM test: virt=%p phys=%p val=%p\n", (void*)test_virt, (void*)test_phys, (void*)(*x));
            uint64_t back = vmm_virt_to_phys(test_virt);
            kprintf("VMM translate: %p -> %p\n", (void*)test_virt, (void*)back);
        }
    }

    void *a = kmalloc(32);
    void *b = kmalloc_aligned(64, 64);
    void *c = kzalloc(128);
    kprintf("Heap test: a=%p b=%p c=%p\n", a, b, c);

    uint64_t page = pmm_alloc_page();
    void *vpage = pmm_phys_to_virt(page);
    kprintf("PMM alloc: phys=%p virt=%p\n", (void*)page, vpage);
    if (vpage) {
        *(volatile uint64_t*)vpage = 0x1122334455667788ULL;
        kprintf("PMM write test: %p\n", (void*)(uintptr_t)*(volatile uint64_t*)vpage);
    }

    serial_write("FiFi OS: calling idt_init...\n");
    idt_init();
    serial_write("FiFi OS: remapping PIC...\n");
    pic_remap(0x20, 0x28);

    /* Mask everything, then allow only IRQ0 (timer) */
    pic_disable();
    pic_clear_mask(0);
    pic_clear_mask(1);

    serial_write("FiFi OS: init PIT 100Hz...\n");
    pit_init(100);

    timer_init(100);

    serial_write("FiFi OS: enabling interrupts (sti)\n");
    pic_unmask_irq(0);
    pic_unmask_irq(1);
    kprintf("FiFi OS: PIC unmasked IRQ1 (keyboard)\n");
    __asm__ volatile ("sti");


    serial_write("FiFi OS: IDT loaded\n");
    console_write("IDT loaded. Exceptions will panic cleanly.\n\n");

#ifdef FIFI_PF_TEST
    kprintf("PF test enabled: intentionally reading from NULL...\n");
    volatile uint64_t *p = (volatile uint64_t*)0x0;
    volatile uint64_t x = *p;
    (void)x;
#endif

    initrd_init();
    initrd_cat("motd.txt");
shell_run();

    serial_write("FiFi OS: entering idle loop (hlt)\n");

    uint64_t start = pit_ticks();
    while ((pit_ticks() - start) < 100) {        }
        __asm__ volatile ("hlt");
    
    serial_write("FiFi OS: timer confirmed\n");
    serial_write("FiFi OS: entering idle loop (hlt)\n");
    for (;;) {
        __asm__ volatile ("hlt");
        (void)pit_ticks();
    }
}
