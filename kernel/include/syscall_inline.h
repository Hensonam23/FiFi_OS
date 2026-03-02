#pragma once
#include <stdint.h>
#include "syscall_numbers.h"

// Kernel side inline helpers for invoking the syscall gate.
// Useful for shell debug commands (ring0 calling int 0x80).

static inline long sys_call0(long n) {
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(n)
                      : "memory");
    return ret;
}

static inline long sys_call1(long n, long a1) {
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(n), "D"(a1)
                      : "memory");
    return ret;
}

static inline long sys_call2(long n, long a1, long a2) {
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(n), "D"(a1), "S"(a2)
                      : "memory");
    return ret;
}

static inline long sys_call3(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                      : "memory");
    return ret;
}
