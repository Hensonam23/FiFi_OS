#include <stdint.h>
#include "usermode.h"

#include "isr.h"
#include "kprintf.h"
#include "panic.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "thread.h"
#include "syscall.h"
#include "serial.h"
#include "vmm.h"
#include "acpi.h"


/* ---- exception debug helpers (auto-generated) ---- */
static inline unsigned long long read_cr2_ull(void) {
    unsigned long long v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

static void isr_print_exception_dump(unsigned int vec, const void *ctx_void) {
    const isr_ctx_t *ctx = (const isr_ctx_t*)ctx_void;

    /* Your last run detected fields: rip, cs, rflags. ctx->error/rsp/ss may not exist in your ctx. */
    unsigned long long rip    = (unsigned long long)ctx->rip;
    unsigned long long cs     = (unsigned long long)ctx->cs;
    unsigned long long rflags = (unsigned long long)ctx->rflags;

    serial_write("\n[EXC] vec=");    print_hex_u64((uint64_t)vec);
    serial_write(" ctx->error=");          print_hex_u64((uint64_t)ctx->error);
    serial_write(" rip=");          print_hex_u64((uint64_t)rip);
    serial_write(" cs=");           print_hex_u64((uint64_t)cs);
    serial_write(" rflags=");       print_hex_u64((uint64_t)rflags);
    serial_write("\n");

    if (vec == 14) {
        serial_write("[EXC] cr2="); print_hex_u64((uint64_t)read_cr2_ull());
        serial_write("\n");
    }
}
/* ---- end helpers ---- */


/* Per-IRQ event counters for diagnostics */
static volatile uint32_t irq_counters[16] = {0};

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
    
    if (ctx->vector == 14) {
        int cpl = (int)(ctx->cs & 3);
        if (cpl == 3) {
            uint64_t handler = thread_get_sig_handler(11); /* SIGSEGV */
            if (handler > 1) {
                uint64_t *ursp_ptr = &((uint64_t *)(ctx + 1))[0];
                uint64_t ursp = *ursp_ptr - 8;
                if (vmm_user_accessible(ursp, 8, true)) {
                    *(uint64_t *)(uintptr_t)ursp = ctx->rip;
                    *ursp_ptr  = ursp;
                    ctx->rdi   = 11;
                    ctx->rip   = handler;
                    return;
                }
            }
            kprintf("Segmentation fault\n");
            ctx->rip = (uint64_t)FIFI_USER_TRAMPOLINE_VA;
            ctx->rax = (uint64_t)SYS_EXIT;
            ctx->rdi = 139;
            return;
        }
        uint64_t cr2 = 0;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        kprintf("\n=== KERNEL PAGE FAULT ===\n");
        kprintf("CR2=%p RIP=%p err=%p\n",
                (void*)cr2, (void*)ctx->rip, (void*)ctx->error);
        panic("kernel page fault");
    }






    uint64_t vec = ctx->vector;
    // Syscall vector (int 0x80)
      if (vec == 0x80) {
          syscall_dispatch(ctx);
          return;
      }



    /* IRQs after PIC remap live at vectors 32-47 */
    if (vec >= 32 && vec < 48) {
        uint8_t irq = (uint8_t)(vec - 32);
        if (irq < 16) irq_counters[irq]++;
        if (irq == 1) keyboard_irq_handler();

        /* Timer tick (IRQ0) */
        if (irq == 0) {
            pit_on_irq0();
            thread_request_resched();
            pit_on_tick();
        }

        /* ACPI SCI (IRQ9) — EC keyboard data delivery */
        if (irq == 9) acpi_sci_handler();

        pic_send_eoi(irq);
        thread_check_resched();
        return;
    }

    /* CPU exceptions */
    const char *name = "(unknown)";
    if (vec < 32) {

        isr_print_exception_dump(vec, ctx);
        name = exc_names[(uint32_t)vec];
    }

    kprintf("\n--- FiFi OS EXCEPTION ---\n");
    kprintf("vector=%p name=%s\n", (void*)vec, name);
    kprintf("error=%p\n", (void*)ctx->error);
    kprintf("RIP=%p CS=%p RFLAGS=%p\n",
            (void*)ctx->rip, (void*)ctx->cs, (void*)ctx->rflags);

  if (fifi_cs_is_user(ctx->cs)) {
    kprintf("Segmentation fault\n");
    ctx->rip = (uint64_t)FIFI_USER_TRAMPOLINE_VA;
    ctx->rax = (uint64_t)SYS_EXIT;
    ctx->rdi = 139;
    return;
  }

  panic("Unhandled CPU exception");
}

uint32_t isr_get_irq_count(uint8_t irq) {
    return (irq < 16) ? irq_counters[irq] : 0;
}

void isr_reset_irq_counts(void) {
    for (int i = 0; i < 16; i++) irq_counters[i] = 0;
}
