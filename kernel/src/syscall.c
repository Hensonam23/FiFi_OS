#include "syscall.h"
#include "kprintf.h"
#include "timer.h"
#include "thread.h"
#include "vmm.h"
#include "usermode.h"

static int copyin_str(char *dst, size_t dst_cap, uint64_t uaddr) {
    if (!dst || dst_cap == 0) return -1;
    if (uaddr == 0) return -1;

    // Must stay in lower-half user space
    if (uaddr >= (uint64_t)FIFI_USER_TOP) return -1;

    size_t i = 0;
    for (; i + 1 < dst_cap; i++) {
        uint64_t a = uaddr + (uint64_t)i;
        if (a >= (uint64_t)FIFI_USER_TOP) return -1;
        if (!vmm_user_accessible(a, 1, false)) return -1;

        char c = *(volatile char*)(uintptr_t)a;
        dst[i] = c;
        if (c == 0) return (int)i;
    }
    dst[dst_cap - 1] = 0;
    return (int)(dst_cap - 1);
}

void syscall_dispatch(isr_ctx_t *ctx) {
    uint64_t n = ctx->rax;

    switch (n) {
        case SYS_NOP:
            ctx->rax = 0;
            return;

        case SYS_LOG: {
            uint64_t ptr = (uint64_t)ctx->rdi;
            int from_user = ((ctx->cs & 3ULL) == 3ULL);

            if (from_user) {
                char buf[256];
                if (copyin_str(buf, sizeof(buf), ptr) < 0) {
                    kprintf("[syscall] SYS_LOG bad user ptr=%p\n", (void*)ptr);
                    ctx->rax = (uint64_t)-1;
                    return;
                }
                kprintf("[syscall] %s\n", buf);
                ctx->rax = 0;
                return;
            }

            // Ring0 callers (kernel shell tests) can pass kernel pointers directly.
            const char *msg = (const char*)(uintptr_t)ptr;
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
        case SYS_EXIT: {
            unsigned long long code = (unsigned long long)ctx->rdi;
            kprintf("[syscall] exit code=%p\n", (void*)code);
            thread_exit();
        }

        default:
            kprintf("[syscall] unknown n=%p (ctx->rax=%p)\n", (void*)n, (void*)ctx->rax);
            ctx->rax = (uint64_t)-1;
            return;

    }
}
