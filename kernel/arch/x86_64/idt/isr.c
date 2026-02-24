#include <stdint.h>

#include "isr.h"
#include "kprintf.h"
#include "panic.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "thread.h"

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
    
    /* Step 9: Page fault (#PF) diagnostics */
    if (ctx->vector == 14) {
        uint64_t cr2 = 0;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

        uint64_t err = ctx->error;

        int p    = (int)((err >> 0) & 1); /* present (1=protection, 0=non-present) */
        int wr   = (int)((err >> 1) & 1); /* 1=write, 0=read */
        int us   = (int)((err >> 2) & 1); /* 1=user, 0=kernel */
        int rsvd = (int)((err >> 3) & 1); /* reserved-bit violation */
        int id   = (int)((err >> 4) & 1); /* instruction fetch */
        int cpl  = (int)(ctx->cs & 3);

        kprintf("\n=== FiFi OS PAGE FAULT (#PF) ===\n");
        kprintf("CR2=%p\n", (void*)cr2);
        kprintf("RIP=%p  CS=%p (CPL=%d)  RFLAGS=%p\n",
                (void*)ctx->rip, (void*)ctx->cs, cpl, (void*)ctx->rflags);

        kprintf("ERR=%p  bits: P=%d WR=%d US=%d RSVD=%d ID=%d\n",
                (void*)err, p, wr, us, rsvd, id);

        kprintf("meaning: %s %s %s %s %s\n",
            p    ? "PROT"  : "NP",
            wr   ? "WRITE" : "READ",
            us   ? "USER"  : "KERN",
            rsvd ? "RSVD"  : "-",
            id   ? "INSTR" : "-"
        );

        panic("page fault");
    }






    uint64_t vec = ctx->vector;

    /* IRQs after PIC remap live at vectors 32-47 */
    if (vec >= 32 && vec < 48) {
        uint8_t irq = (uint8_t)(vec - 32);
        if (irq == 1) keyboard_irq_handler();

        /* Timer tick (IRQ0) */
        if (irq == 0) {
            pit_on_irq0();
            thread_request_resched();
            pit_on_tick();
        }

        /* Keyboard (IRQ1) */
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
