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

/* sys_write: write to fd (1 = stdout/console).  Same call sites as before. */
static inline long sys_write(const void *buf, uint64_t len) {
    return sys_call3(SYS_WRITE, 1L, (long)(uintptr_t)buf, (long)len);
}

/* sys_fdwrite: write to an arbitrary fd (e.g., a pipe write end). */
static inline long sys_fdwrite(int fd, const void *buf, uint64_t len) {
    return sys_call3(SYS_WRITE, (long)fd, (long)(uintptr_t)buf, (long)len);
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

static inline long sys_readfile(const char *path, void *out_buf, uint64_t cap) {
    return sys_call3(SYS_READFILE,
                     (long)(uintptr_t)path,
                     (long)(uintptr_t)out_buf,
                     (long)cap);
}


/* exec(path): replace this process with the ELF at path (no arguments). */
static inline long sys_exec(const char *path) {
    return sys_call2(SYS_EXEC, (long)(uintptr_t)path, 0L);
}

/* execv(path, argv): replace this process; argv is a NULL-terminated array
 * of string pointers (argv[0] is conventionally the program name). */
static inline long sys_execv(const char *path, const char *const *argv) {
    return sys_call2(SYS_EXEC, (long)(uintptr_t)path, (long)(uintptr_t)argv);
}

static inline long sys_fork(void) {
    return sys_call0(SYS_FORK);
}

/* brk(0) returns current break; brk(addr) sets break, returns new break.
 * On failure returns the unchanged old break. */
static inline unsigned long sys_brk(unsigned long addr) {
    return (unsigned long)sys_call1(SYS_BRK, (long)addr);
}

/* listfiles(buf, cap): fill buf with "name\n"-separated VFS file list.
 * Returns bytes written, or -1 on error. cap should be at most 4096. */
static inline long sys_listfiles(char *buf, unsigned long cap) {
    return sys_call2(SYS_LISTFILES, (long)(uintptr_t)buf, (long)cap);
}

/* getchar(): blocking read of one character from the console keyboard.
 * Returns ASCII value (or KEY_* special code). Never returns -1. */
static inline int sys_getchar(void) {
    return (int)sys_call0(SYS_GETCHAR);
}

/* waitpid(child_tid, &exit_code): wait for child to exit, collect status.
 * child_tid == (uint32_t)-1 waits for any child.
 * Returns reaped TID on success, -1 on error/timeout/no child. */
static inline long sys_waitpid(unsigned long child_tid, int *exit_code) {
    return sys_call2(SYS_WAITPID, (long)child_tid, (long)(uintptr_t)exit_code);
}

/* pipe(pipefd): create a pipe.  pipefd[0] = read end, pipefd[1] = write end.
 * Returns 0 on success, -1 on failure. */
static inline long sys_pipe(int pipefd[2]) {
    return sys_call1(SYS_PIPE, (long)(uintptr_t)pipefd);
}

/* dup2(old_fd, new_fd): make new_fd refer to the same resource as old_fd.
 * Returns new_fd on success, -1 on failure. */
static inline long sys_dup2(int old_fd, int new_fd) {
    return sys_call2(SYS_DUP2, (long)old_fd, (long)new_fd);
}

/* creat(path): create or truncate a file for writing.
 * Returns a write-only fd, or -1 on failure. */
static inline long sys_creat(const char *path) {
    return sys_call1(SYS_CREAT, (long)(uintptr_t)path);
}

/* sbrk(n): grow heap by n bytes, return pointer to start of new region.
 * Returns (void*)-1 on failure. */
static inline void *sys_sbrk(unsigned long n) {
    unsigned long cur = sys_brk(0);
    if (!cur) return (void*)-1UL;
    unsigned long new_brk = sys_brk(cur + n);
    if (new_brk != cur + n && new_brk == cur) return (void*)-1UL;
    return (void*)cur;
}
