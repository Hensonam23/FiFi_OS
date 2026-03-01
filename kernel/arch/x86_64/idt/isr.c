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

                  if (cpl == 3) {
              kprintf("[EXC] user page fault -> redirect to SYS_EXIT\n");
              ctx->rip = (uint64_t)FIFI_USER_TRAMPOLINE_VA;
              ctx->rax = (uint64_t)SYS_EXIT;
              ctx->rdi = (((uint64_t)ctx->vector) << 32) | (uint64_t)ctx->error;
              return;
          }

          panic("page fault");
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

        isr_print_exception_dump(vec, ctx);
        name = exc_names[(uint32_t)vec];
    }

    kprintf("\n--- FiFi OS EXCEPTION ---\n");
    kprintf("vector=%p name=%s\n", (void*)vec, name);
    kprintf("error=%p\n", (void*)ctx->error);
    kprintf("RIP=%p CS=%p RFLAGS=%p\n",
            (void*)ctx->rip, (void*)ctx->cs, (void*)ctx->rflags);

    
  // ---- FiFi OS: usermode-safe exceptions ----
  // If the fault happened in ring3 (CS RPL=3), do NOT panic the kernel.
  // Instead redirect RIP to a mapped user trampoline page that will do SYS_EXIT.
  if (fifi_cs_is_user(ctx->cs)) {
    // Keep your existing dump printing (it should run before this panic in your code).
    // Then force the current user task to exit via a known-mapped trampoline.
    ctx->rip = (uint64_t)FIFI_USER_TRAMPOLINE_VA;

    // Syscall ABI: rax = syscall number, rdi = arg0
    // We assume your dispatcher already uses these (SYS_LOG works).
    // NOTE: if your syscall number constant is named differently, fix it in syscall header.    // We won't reference SYS_EXIT symbol directly here (to avoid link issues).
    // Instead we set a number in rax AFTER you add SYS_EXIT in your syscall dispatcher step.
    // For now, set rax to 0; we'll patch this value in the syscall step if needed.
    ctx->rax = (uint64_t)SYS_EXIT;

    // Put something useful as exit code: (vec << 32) | ctx->error
    ctx->rdi = (((uint64_t)vec) << 32) | (uint64_t)ctx->error;

    return;
  }
  // ------------------------------------------

  panic("Unhandled CPU exception");
}
