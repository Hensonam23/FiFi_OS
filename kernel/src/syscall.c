#include "syscall.h"
#include "fork.h"
#include "kprintf.h"
#include "timer.h"
#include "thread.h"
#include "vmm.h"
#include "usermode.h"
#include "vfs.h"
#include "exec.h"

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

static int copyin_bytes(uint8_t *dst, size_t cap, uint64_t uaddr, size_t n) {
    if (!dst || cap == 0) return -1;
    if (n > cap) n = cap;
    if (n == 0) return 0;
    if (uaddr == 0) return -1;

    // Basic overflow checks
    uint64_t end = uaddr + (uint64_t)n;
    if (end < uaddr) return -1;

    // Must stay in user range
    if (uaddr >= (uint64_t)FIFI_USER_TOP) return -1;
    if (end > (uint64_t)FIFI_USER_TOP) return -1;

    // Validate mapping once for the whole range (page-walk)
    if (!vmm_user_accessible(uaddr, n, false)) return -1;

    for (size_t i = 0; i < n; i++) {
        dst[i] = *(volatile uint8_t*)(uintptr_t)(uaddr + (uint64_t)i);
    }
    return (int)n;
}


static int copyout_bytes(uint64_t uaddr, const uint8_t *src, size_t n) {
    if (n == 0) return 0;
    if (!src) return -1;
    if (uaddr == 0) return -1;

    uint64_t end = uaddr + (uint64_t)n;
    if (end < uaddr) return -1;

    if (uaddr >= (uint64_t)FIFI_USER_TOP) return -1;
    if (end > (uint64_t)FIFI_USER_TOP) return -1;

    if (!vmm_user_accessible(uaddr, n, true)) return -1;

    for (size_t i = 0; i < n; i++) {
        *(volatile uint8_t*)(uintptr_t)(uaddr + (uint64_t)i) = src[i];
    }
    return (int)n;
}

#define SYS_FD_MAX 16

typedef struct {
    int used;
    uint64_t owner_tid;
    const uint8_t *data;
    uint64_t size;
    uint64_t off;
} sys_fd_t;

static sys_fd_t g_fds[SYS_FD_MAX];

static int fd_alloc(uint64_t tid, const uint8_t *data, uint64_t size) {
    for (int i = 0; i < SYS_FD_MAX; i++) {
        if (!g_fds[i].used) {
            g_fds[i].used = 1;
            g_fds[i].owner_tid = tid;
            g_fds[i].data = data;
            g_fds[i].size = size;
            g_fds[i].off = 0;
            return i;
        }
    }
    return -1;
}

static sys_fd_t *fd_get(uint64_t tid, int fd) {
    if (fd < 0 || fd >= SYS_FD_MAX) return 0;
    if (!g_fds[fd].used) return 0;
    if (g_fds[fd].owner_tid != tid) return 0;
    return &g_fds[fd];
}

static void fd_close(uint64_t tid, int fd) {
    sys_fd_t *f = fd_get(tid, fd);
    if (!f) return;
    f->used = 0;
    f->owner_tid = 0;
    f->data = 0;
    f->size = 0;
    f->off = 0;
}


static void fd_close_all(uint64_t tid) {
    for (int i = 0; i < SYS_FD_MAX; i++) {
        if (!g_fds[i].used) continue;
        if (g_fds[i].owner_tid != tid) continue;
        g_fds[i].used = 0;
        g_fds[i].owner_tid = 0;
        g_fds[i].data = 0;
        g_fds[i].size = 0;
        g_fds[i].off = 0;
    }
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

        case SYS_SLEEP_MS: {
            // rdi = ms
            uint64_t ms = (uint64_t)ctx->rdi;
            // clamp a bit just to avoid insane values
            if (ms > 60000) ms = 60000;
            thread_sleep_ms((uint32_t)ms);
            ctx->rax = 0;
            return;
        }

        case SYS_EXIT: {
            unsigned long long code = (unsigned long long)ctx->rdi;
            uint64_t tid = thread_current_tid();

            fd_close_all(tid);
            thread_user_map_cleanup_current();

            kprintf("[syscall] exit code=%p\n", (void*)code);
            thread_exit();
        }

        case SYS_WRITE: {
            // rdi = user ptr, rsi = len
            uint64_t ptr = (uint64_t)ctx->rdi;
            uint64_t len = (uint64_t)ctx->rsi;

            // keep it sane
            if (len > 1024) len = 1024;

            int from_user = ((ctx->cs & 3ULL) == 3ULL);

            if (from_user) {
                uint8_t buf[1024];
                int n = copyin_bytes(buf, sizeof(buf), ptr, (size_t)len);
                if (n < 0) {
                    kprintf("[syscall] SYS_WRITE bad user ptr=%p len=%p\n", (void*)ptr, (void*)len);
                    ctx->rax = (uint64_t)-1;
                    return;
                }

                for (int i = 0; i < n; i++) {
                    kprintf("%c", (int)buf[i]);
                }

                ctx->rax = (uint64_t)n;
                return;
            }

            // ring0 caller: treat as kernel pointer
            const uint8_t *k = (const uint8_t*)(uintptr_t)ptr;
            for (uint64_t i = 0; i < len; i++) {
                kprintf("%c", (int)k[i]);
            }
            ctx->rax = len;
            return;
        }

        case SYS_GETTID: {
            ctx->rax = (uint64_t)thread_current_tid();
            return;
        }

        case SYS_OPEN: {
            // rdi = user ptr to path string
            uint64_t upath = (uint64_t)ctx->rdi;
            int from_user = ((ctx->cs & 3ULL) == 3ULL);

            // only allow from ring3
            if (!from_user) { ctx->rax = (uint64_t)-1; return; }

            char path[128];
            if (copyin_str(path, sizeof(path), upath) < 0) {
                ctx->rax = (uint64_t)-1;
                return;
            }

            const void *data = 0;
            uint64_t size = 0;
            int rc = vfs_read(path, &data, &size);
            if (rc < 0 || !data) {
                ctx->rax = (uint64_t)-1;
                return;
            }

            uint64_t tid = thread_current_tid();
            int fd = fd_alloc(tid, (const uint8_t*)data, size);
            ctx->rax = (fd >= 0) ? (uint64_t)fd : (uint64_t)-1;
            return;
        }

        case SYS_READ: {
            // rdi = fd, rsi = user buf ptr, rdx = len
            int fd = (int)ctx->rdi;
            uint64_t ubuf = (uint64_t)ctx->rsi;
            uint64_t len  = (uint64_t)ctx->rdx;

            int from_user = ((ctx->cs & 3ULL) == 3ULL);
            if (!from_user) { ctx->rax = (uint64_t)-1; return; }

            if (len > 4096) len = 4096;

            uint64_t tid = thread_current_tid();
            sys_fd_t *f = fd_get(tid, fd);
            if (!f) { ctx->rax = (uint64_t)-1; return; }

            if (f->off >= f->size) {
                ctx->rax = 0; // EOF
                return;
            }

            uint64_t remain = f->size - f->off;
            uint64_t n = len;
            if (n > remain) n = remain;

            if (copyout_bytes(ubuf, f->data + f->off, (size_t)n) < 0) {
                ctx->rax = (uint64_t)-1;
                return;
            }

            f->off += n;
            ctx->rax = n;
            return;
        }

        case SYS_CLOSE: {
            // rdi = fd
            int fd = (int)ctx->rdi;
            int from_user = ((ctx->cs & 3ULL) == 3ULL);
            if (!from_user) { ctx->rax = (uint64_t)-1; return; }

            uint64_t tid = thread_current_tid();
            fd_close(tid, fd);
            ctx->rax = 0;
            return;
        }

        case SYS_READFILE: {
            // rdi = user path pointer
            // rsi = user out buffer (optional)
            // rdx = capacity (optional)
            uint64_t upath = (uint64_t)ctx->rdi;
            uint64_t ubuf  = (uint64_t)ctx->rsi;
            uint64_t cap   = (uint64_t)ctx->rdx;

            int from_user = ((ctx->cs & 3ULL) == 3ULL);

            const char *kpath = 0;
            char path[128];

            if (from_user) {
                if (copyin_str(path, sizeof(path), upath) < 0) {
                    kprintf("[syscall] SYS_READFILE bad path ptr=%p\n", (void*)upath);
                    ctx->rax = (uint64_t)-1;
                    return;
                }
                kpath = path;
            } else {
                kpath = (const char*)(uintptr_t)upath;
            }

            const void *data = 0;
            uint64_t size = 0;

            if (vfs_read(kpath, &data, &size) < 0 || !data) {
                ctx->rax = (uint64_t)-1;
                return;
            }

            // size query mode
            if (ubuf == 0 || cap == 0) {
                ctx->rax = (uint64_t)size;
                return;
            }

            // clamp to cap
            uint64_t n = size;
            if (n > cap) n = cap;

            if (from_user) {
                int rc = copyout_bytes(ubuf, (const uint8_t*)data, (size_t)n);
                if (rc < 0) {
                    ctx->rax = (uint64_t)-1;
                    return;
                }
                ctx->rax = (uint64_t)rc;
                return;
            }

            // ring0 caller: treat ubuf as kernel pointer
            uint8_t *kdst = (uint8_t*)(uintptr_t)ubuf;
            for (uint64_t i = 0; i < n; i++) kdst[i] = ((const uint8_t*)data)[i];

            ctx->rax = (uint64_t)n;
            return;
        }





        case SYS_EXEC: {
            uint64_t upath = ctx->rdi;
            char path[256];
            if (copyin_str(path, sizeof(path), upath) < 0) {
                ctx->rax = (uint64_t)-1;
                break;
            }
            int r = exec_load(ctx, path);
            if (r < 0) ctx->rax = (uint64_t)-1;
            /* on success exec_load modified ctx for iretq to new program */
            break;
        }

        case SYS_FORK: {
            ctx->rax = (uint64_t)do_fork(ctx);
            break;
        }

        default:
            kprintf("[syscall] unknown n=%p (ctx->rax=%p)\n", (void*)n, (void*)ctx->rax);
            ctx->rax = (uint64_t)-1;
            return;

    }
}
