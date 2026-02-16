#include <stdint.h>

#include "isr.h"
#include "kprintf.h"
#include "panic.h"
#include "pic.h"
#include "pit.h"
#include "serial.h"

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
    uint64_t vec = ctx->vector;

    /* IRQs after PIC remap live at vectors 32-47 */
    if (vec >= 32 && vec < 48) {
        uint8_t irq = (uint8_t)(vec - 32);

        /* Timer tick (IRQ0) */
        if (irq == 0) {
            pit_on_tick();
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
    kprintf("RIP=%p CS=%p RFLAGS=%p\n", (void*)ctx->rip, (void*)ctx->cs, (void*)ctx->rflags);

    panic("Unhandled CPU exception");
}
