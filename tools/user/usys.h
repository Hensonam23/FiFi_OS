#pragma once
#include <stdint.h>
#include <stddef.h>

// ONLY syscall numbers (no kernel ISR/types)
#include "../../kernel/include/syscall_numbers.h"

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

static inline long sys_log(const char *s) {
    return sys_call1(SYS_LOG, (long)(uintptr_t)s);
}

static inline long sys_yield(void) {
    return sys_call0(SYS_YIELD);
}

static inline uint64_t sys_uptime(void) {
    return (uint64_t)sys_call0(SYS_UPTIME);
}

static inline long sys_sleep_ms(uint64_t ms) {
    return sys_call1(SYS_SLEEP_MS, (long)ms);
}

static inline __attribute__((noreturn)) void sys_exit(int code) {
    (void)sys_call1(SYS_EXIT, (long)code);
    for (;;) { }
}

static inline uint64_t sys_gettid(void) {
    return (uint64_t)sys_call0(SYS_GETTID);
}

static inline long sys_write(const void *buf, uint64_t len) {
    return sys_call2(SYS_WRITE, (long)(uintptr_t)buf, (long)len);
}

static inline long sys_open(const char *path) {
    return sys_call1(SYS_OPEN, (long)(uintptr_t)path);
}

static inline long sys_read(long fd, void *buf, uint64_t len) {
    return sys_call3(SYS_READ, (long)fd, (long)(uintptr_t)buf, (long)len);
}

static inline long sys_close(long fd) {
    return sys_call1(SYS_CLOSE, (long)fd);
}
