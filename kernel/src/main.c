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
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "pmm.h"
#include "heap.h"
#include "vmm.h"
#include "initrd.h"
#include "vfs.h"

static void shell_run(void);

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
        uint64_t off = hhdm_request.response->offset;
        kprintf("HHDM offset: %p\n", (void*)(uintptr_t)off);
    }

    /* FiFi OS: init PMM (simple bump allocator) */
    uint64_t hhdm_off = 0;
    if (hhdm_request.response) {
        hhdm_off = hhdm_request.response->offset;
    }
    pmm_init(memmap_request.response, hhdm_off);

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

    serial_write("FiFi OS: enabling interrupts (sti)\n");
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


// -------------------- Simple FiFi shell --------------------
int keyboard_try_getchar(void); // from keyboard.c

static inline void cpu_hlt(void) {
    __asm__ __volatile__("hlt");
}

static inline void cpu_cli(void) {
    __asm__ __volatile__("cli");
}


static void shell_print_prompt(void) {
    kprintf("FiFi> ");
}

static int streq_simple(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == 0 && *b == 0);
}

static void shell_help(void) {
    kprintf("\nCommands:\n");
    kprintf("  help             show this list\n");
    kprintf("  clear            clear screen (prints newlines for now)\n");
    kprintf("  ls               list initrd files\n");
    kprintf("  cat <file>       print an initrd file\n");
    kprintf("  motd             show motd.txt from initrd\n");
    kprintf("  uptime           show uptime (PIT ticks)\n");
    kprintf("  mem              show memory stats (PMM + heap)\n");
    kprintf("  ai               placeholder for local AI agent\n");
    kprintf("  modules          list limine modules (includes initrd)\n");
    kprintf("  reboot           reboot (port 0x64)\n");
    kprintf("  halt             stop CPU\n");
    kprintf("\n");
}



static void shell_exec(char *line) {
    // trim leading spaces
    while (*line == ' ' || *line == '\t') line++;
    if (*line == 0) return;

    // tokenize by spaces/tabs
    char *argv[8];
    int argc = 0;

    char *p = line;
    while (*p && argc < 8) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = 0; p++; }
    }

    if (argc == 0) return;

    if (streq_simple(argv[0], "help")) {
        shell_help();
        return;
    }

    if (streq_simple(argv[0], "clear")) {
        console_clear();
        return;
    }

    if (streq_simple(argv[0], "modules")) {
        initrd_dump_modules();
        
    vfs_init();
return;
    }

    if (streq_simple(argv[0], "ls")) {
        initrd_ls();
        return;
    }

    if (streq_simple(argv[0], "cat")) {
        if (argc < 2) {
            kprintf("Usage: cat <file>\n");
            return;
        }
        initrd_cat(argv[1]);
        return;
    }

    if (streq_simple(argv[0], "motd")) {
        initrd_cat("motd.txt");
        return;
    }


    if (streq_simple(argv[0], "uptime")) {
        uint64_t ticks = pit_get_ticks();
        uint32_t hz = pit_get_hz();
        uint64_t sec = (hz ? (ticks / hz) : 0);
        kprintf("Uptime: ticks=%p hz=%d sec=%p\n", (void*)ticks, (int)hz, (void*)sec);
        return;
    }

    if (streq_simple(argv[0], "ai")) {
        kprintf("AI agent: not installed yet.\n");
        kprintf("Plan: docs/ai-agent-plan.md\n");
        return;
    }


    if (streq_simple(argv[0], "mem")) {
        uint64_t total = pmm_get_total_pages();
        uint64_t freep = pmm_get_free_pages();
        uint64_t used  = pmm_get_used_pages();

        void *hpage = heap_get_cur_page();
        uint64_t hoff = heap_get_offset();

        kprintf("PMM: total=%p free=%p used=%p pages (4KiB)\n",
                (void*)total, (void*)freep, (void*)used);

        if (total) {
            uint64_t total_kib = total * 4;
            uint64_t free_kib  = freep * 4;
            uint64_t used_kib  = used * 4;
            kprintf("PMM: total=%p KiB free=%p KiB used=%p KiB\n",
                    (void*)total_kib, (void*)free_kib, (void*)used_kib);
        }

        kprintf("Heap: cur_page=%p offset=%p bytes\n", hpage, (void*)hoff);
        return;
    }

    if (streq_simple(argv[0], "reboot")) {
        kprintf("FiFi OS: rebooting...\n");
        outb(0x64, 0xFE);
        cpu_cli();
        for (;;) cpu_hlt();
    }

    if (streq_simple(argv[0], "halt")) {
        kprintf("FiFi OS: halted.\n");
        cpu_cli();
        for (;;) cpu_hlt();
    }

    kprintf("Unknown command: %s\n", argv[0]);
    kprintf("Type: help\n");
}



static void shell_run(void) {
    char line[128];
    unsigned long len = 0;

    kprintf("\nFiFi OS shell online. Type 'help'.\n");
    shell_print_prompt();

    for (;;) {
        int ch = keyboard_try_getchar();
        if (ch < 0) {
            cpu_hlt();
            continue;
        }

        char c = (char)ch;

        if (c == '\r') continue;

        // echo typed chars so it feels normal
        if (c != '\n' && c != '\b' && c != 127) {
            kprintf("%c", c);
        }

        if (c == '\n') {
            kprintf("\n");
            line[len] = 0;
            shell_exec(line);
            len = 0;
            shell_print_prompt();
            continue;
        }

        if (c == '\b' || c == 127) {
            if (len > 0) {
                len--;
                // simple visual backspace: move left, overwrite, move left
                kprintf("\b \b");
            }
            continue;
        }

        if (len < (sizeof(line) - 1)) {
            line[len++] = c;
        }
    }
}
