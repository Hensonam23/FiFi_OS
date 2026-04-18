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


/* execve(path, argv, envp): replace this process; argv and envp are
 * NULL-terminated pointer arrays (pass NULL for either if not needed). */
static inline long sys_execve(const char *path,
                               const char *const *argv,
                               const char *const *envp) {
    return sys_call3(SYS_EXEC, (long)(uintptr_t)path,
                     (long)(uintptr_t)argv, (long)(uintptr_t)envp);
}

/* Convenience wrappers */
static inline long sys_exec(const char *path) {
    return sys_execve(path, (void*)0, (void*)0);
}
static inline long sys_execv(const char *path, const char *const *argv) {
    return sys_execve(path, argv, (void*)0);
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

/* unlink(path): delete a file.  Returns 0 on success, -1 on failure. */
static inline long sys_unlink(const char *path) {
    return sys_call1(SYS_UNLINK, (long)(uintptr_t)path);
}

/* openw(path): open a file for append-write (pre-loads existing content).
 * Returns a write-only fd, or -1 on failure. */
static inline long sys_openw(const char *path) {
    return sys_call1(SYS_OPENW, (long)(uintptr_t)path);
}

/* filesize(path): returns file size in bytes, or -1 if not found. */
static inline long sys_filesize(const char *path) {
    return sys_call1(SYS_FILESIZE, (long)(uintptr_t)path);
}

/* mkdir(path): create a directory.  Returns 0 on success, -1 on failure. */
static inline long sys_mkdir(const char *path) {
    return sys_call1(SYS_MKDIR, (long)(uintptr_t)path);
}

/* getcwd(buf, size): copy cwd into buf.  Returns length, or -1 on error. */
static inline long sys_getcwd(char *buf, unsigned long size) {
    return sys_call2(SYS_GETCWD, (long)(uintptr_t)buf, (long)size);
}

/* chdir(path): change working directory.  Returns 0 on success, -1 on error. */
static inline long sys_chdir(const char *path) {
    return sys_call1(SYS_CHDIR, (long)(uintptr_t)path);
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

/* ── signal constants ────────────────────────────────────────────────────── */
#define SIG_DFL  ((void*)0UL)   /* default action */
#define SIG_IGN  ((void*)1UL)   /* ignore signal */
#define SIGINT   2
#define SIGQUIT  3
#define SIGKILL  9
#define SIGTERM  15
#define SIGCONT  18
#define SIGTSTP  19

/* ── wait flags ──────────────────────────────────────────────────────────── */
#define WUNTRACED 1   /* also return if child stopped */
#define WNOHANG   2   /* return immediately if no child changed state */

/* POSIX-like status inspection */
#define WIFSTOPPED(s)  (((s) & 0xFF) == 0x7F)
#define WSTOPSIG(s)    (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s) (!WIFSTOPPED(s) && (s) >= 128)
#define WTERMSIG(s)    ((s) - 128)
#define WIFEXITED(s)   (!WIFSTOPPED(s) && (s) < 128)
#define WEXITSTATUS(s) (s)

/* ── mmap prot flags ─────────────────────────────────────────────────────── */
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

/* kill(tid, sig): send signal to thread. Returns 0 on success, -1 on error. */
static inline int sys_kill(unsigned long tid, int sig) {
    return (int)sys_call2(SYS_KILL, (long)tid, (long)sig);
}

/* signal(sig, handler): install user-space signal handler.
 * handler: SIG_DFL(NULL) = default, SIG_IGN((void*)1) = ignore, else user VA.
 * Returns previous handler. */
static inline void *sys_signal(int sig, void *handler) {
    return (void *)(uintptr_t)sys_call2(SYS_SIGNAL, (long)sig, (long)(uintptr_t)handler);
}

/* mmap(addr, length, prot): map anonymous pages. addr=0 lets kernel choose.
 * Returns mapped virtual address or (void*)-1 on failure. */
static inline void *sys_mmap(void *addr, unsigned long length, int prot) {
    return (void *)(uintptr_t)sys_call3(SYS_MMAP,
                                         (long)(uintptr_t)addr, (long)length, (long)prot);
}

/* munmap(addr, length): unmap previously mapped region. Returns 0 on success. */
static inline int sys_munmap(void *addr, unsigned long length) {
    return (int)sys_call2(SYS_MUNMAP, (long)(uintptr_t)addr, (long)length);
}

/* setpgid(tid, pgid): set process group. 0 for either = self/tid. */
static inline int sys_setpgid(unsigned long tid, unsigned long pgid) {
    return (int)sys_call2(SYS_SETPGID, (long)tid, (long)pgid);
}

/* listdir(path, buf, cap): list directory entries as "name\n" lines.
 * Returns bytes written, or -1 on error. */
static inline long sys_listdir(const char *path, char *buf, unsigned long cap) {
    return sys_call3(SYS_LISTDIR, (long)(uintptr_t)path, (long)(uintptr_t)buf, (long)cap);
}

/* waitpid_flags(child_tid, &status, flags): extended waitpid.
 * flags: WUNTRACED=1, WNOHANG=2
 * Returns child TID on state change, 0 on WNOHANG with no change, -1 on error. */
static inline long sys_waitpid_flags(unsigned long child_tid, int *status, int flags) {
    return sys_call3(SYS_WAITFLAGS, (long)child_tid, (long)(uintptr_t)status, (long)flags);
}

/* getpid(): return current process group ID (or TID if not in a group). */
static inline unsigned long sys_getpid(void) {
    return (unsigned long)sys_call0(SYS_GETPID);
}

/* getppid(): return parent thread ID, or 0 if no parent. */
static inline unsigned long sys_getppid(void) {
    return (unsigned long)sys_call0(SYS_GETPPID);
}

/* rename(old, new): rename/move a file. Returns 0 on success, -1 on failure. */
static inline long sys_rename(const char *old, const char *new) {
    return sys_call2(SYS_RENAME, (long)(uintptr_t)old, (long)(uintptr_t)new);
}

/* stat(path, st): fill fifi_stat with file info. Returns 0 on success, -1 if not found. */
static inline long sys_stat(const char *path, struct fifi_stat *st) {
    return sys_call2(SYS_STAT, (long)(uintptr_t)path, (long)(uintptr_t)st);
}

/* fstat(fd, st): fill fifi_stat from open fd. Returns 0 on success, -1 on failure. */
static inline long sys_fstat(int fd, struct fifi_stat *st) {
    return sys_call2(SYS_FSTAT, (long)fd, (long)(uintptr_t)st);
}

/* time(): seconds since boot. */
static inline unsigned long sys_time(void) {
    return (unsigned long)sys_call0(SYS_TIME);
}

/* dup(fd): duplicate fd to lowest available fd. Returns new fd, or -1 on failure. */
static inline long sys_dup(int fd) {
    return sys_call1(SYS_DUP, (long)fd);
}

/* uname(buf): fill fifi_utsname with OS info. Returns 0 on success. */
static inline long sys_uname(struct fifi_utsname *buf) {
    return sys_call1(SYS_UNAME, (long)(uintptr_t)buf);
}
