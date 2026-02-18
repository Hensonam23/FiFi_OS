#include <stdint.h>

#include "isr.h"
#include "kprintf.h"
#include "panic.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static const char *exc_names[32] = {
    "Divide-by-zero",
    "Debug",
    "Non-maskable interrupt",
    "Breakpoint",
    "Overflow",
    "Bound range exceeded",
    "Invalid opcode",
    "Device not available",
    "Double fault",
    "Coprocessor segment overrun",
    "Invalid TSS",
    "Segment not present",
    "Stack-segment fault",
    "General protection fault",
    "Page fault",
    "Reserved",
    "x87 floating-point exception",
    "Alignment check",
    "Machine check",
    "SIMD floating-point exception",
    "Virtualization exception",
    "Control protection exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor injection exception",
    "VMM communication exception",
    "Security exception",
    "Reserved"
};

void isr_common_handler(isr_ctx_t *ctx) {
    /* Step 8: Page fault (#PF) decoder */
    if (ctx->vector == 14) {
        uint64_t cr2 = 0;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

        uint64_t err = ctx->error;

        kprintf("\nFiFi OS: PAGE FAULT\n");
        kprintf("CR2=%p err=%p\n", (void*)cr2, (void*)err);
        kprintf("cause: %s %s %s %s %s\n",
            (err & 1) ? "PROT"  : "NP",
            (err & 2) ? "WRITE" : "READ",
            (err & 4) ? "USER"  : "KERN",
            (err & 8) ? "RSVD"  : "-",
            (err & 16)? "INSTR" : "-"
        );

        panic("page fault");
    }


    uint64_t vec = ctx->vector;

    /* IRQs after PIC remap live at vectors 32-47 */
    if (vec >= 32 && vec < 48) {
        uint8_t irq = (uint8_t)(vec - 32);

        /* Timer tick (IRQ0) */
        if (irq == 0) {
            pit_on_tick();
        }

        /* Keyboard (IRQ1) */
        if (irq == 1) {
            uint8_t sc = inb(0x60);
            keyboard_on_scancode(sc);
        }

        pic_send_eoi(irq);
        return;
    }

    /* CPU exceptions */
    const char *name = "(unknown)";
    if (vec < 32) {
        name = exc_names[(uint32_t)vec];
    }

    kprintf("\n--- FiFi OS EXCEPTION ---\n");
    kprintf("vector=%p name=%s\n", (void*)vec, name);
    kprintf("error=%p\n", (void*)ctx->error);
    kprintf("RIP=%p CS=%p RFLAGS=%p\n",
            (void*)ctx->rip, (void*)ctx->cs, (void*)ctx->rflags);

    panic("Unhandled CPU exception");
}
