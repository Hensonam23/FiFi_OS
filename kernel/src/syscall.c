#include "syscall.h"
#include "kprintf.h"
#include "timer.h"
#include "thread.h"

void syscall_dispatch(isr_ctx_t *ctx) {
    uint64_t n = ctx->rax;

    switch (n) {
        case SYS_NOP:
            ctx->rax = 0;
            return;

        case SYS_LOG: {
            const char *msg = (const char*)(uintptr_t)ctx->rdi;
            if (msg) kprintf("[syscall] %s\n", msg);
            else     kprintf("[syscall] (null)\n");
            ctx->rax = 0;
            return;
        }

        case SYS_UPTIME:
            // PIT ticks (monotonic)
            ctx->rax = (uint64_t)timer_ticks();
            return;

        case SYS_YIELD:
            thread_yield();
            ctx->rax = 0;
            return;

        default:
            kprintf("[syscall] unknown n=%llu\n", (unsigned long long)n);
            ctx->rax = (uint64_t)-1;
            return;
    }
}
