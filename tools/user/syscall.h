#pragma once
#include <stdint.h>

// Keep these in sync with kernel/include/syscall.h
#define SYS_LOG   1
#define SYS_EXIT  4

static inline long sys_call1(long n, long a1) {
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(n), "D"(a1)
                      : "memory");
    return ret;
}

static inline __attribute__((noreturn)) void sys_exit(int code) {
    (void)sys_call1(SYS_EXIT, (long)code);
    for (;;) { }
}

static inline long sys_log(const char *s) {
    return sys_call1(SYS_LOG, (long)(uintptr_t)s);
}
