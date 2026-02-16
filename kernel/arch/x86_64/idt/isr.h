#pragma once
#include <stdint.h>

typedef struct isr_ctx {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;

    uint64_t vector;
    uint64_t error;

    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
} isr_ctx_t;

void isr_common_handler(isr_ctx_t *ctx);
extern void *isr_stub_table[];
