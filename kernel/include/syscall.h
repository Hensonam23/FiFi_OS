#pragma once

#include <stdint.h>
#include "isr.h"

/*
  Syscall numbers (temporary ABI for bring-up)
*/
enum {
    SYS_NOP    = 0,
    SYS_LOG    = 1,
    SYS_UPTIME = 2,
    SYS_YIELD  = 3,
};

/*
  Kernel-side wrappers for testing int 0x80 from ring0 shell code.
  Later, ring3 uses the same numbers but different calling context.
*/
static inline long sys_call0(long n) {
    long ret;
    __asm__ volatile (
        "mov %1, %%rax\n"
        "int $0x80\n"
        "mov %%rax, %0\n"
        : "=r"(ret)
        : "r"(n)
        : "rax", "memory"
    );
    return ret;
}

static inline long sys_call1(long n, long a1) {
    long ret;
    __asm__ volatile (
        "mov %1, %%rax\n"
        "mov %2, %%rdi\n"
        "int $0x80\n"
        "mov %%rax, %0\n"
        : "=r"(ret)
        : "r"(n), "r"(a1)
        : "rax", "rdi", "memory"
    );
    return ret;
}

/*
  Kernel dispatcher called by ISR for vec 0x80.
  Uses ctx->rax as syscall number, returns value in ctx->rax.
*/
void syscall_dispatch(isr_ctx_t *ctx);
