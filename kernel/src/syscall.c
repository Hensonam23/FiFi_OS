#include "syscall.h"
#include "fork.h"
#include "kprintf.h"
#include "timer.h"
#include "thread.h"
#include "vmm.h"
#include "pmm.h"
#include "usermode.h"
#include "vfs.h"
#include "exec.h"
#include "keyboard.h"

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

/* ── pipes ──────────────────────────────────────────────────────────────────
 * Fixed-size ring-buffer pipes.  No dynamic allocation — just a static pool.
 * The pipe is ref-counted: ropen = live read-end fds, wopen = live write-end fds.
 */
#define PIPE_BUF  4096
#define PIPE_MAX  8

typedef struct {
    uint8_t  buf[PIPE_BUF];
    uint32_t head;          /* index of next byte to read */
    uint32_t nread;         /* bytes currently in buffer */
    volatile int ropen;     /* number of live read-end fds */
    volatile int wopen;     /* number of live write-end fds */
} pipe_t;

static pipe_t g_pipes[PIPE_MAX];

static pipe_t *pipe_alloc(void) {
    for (int i = 0; i < PIPE_MAX; i++) {
        pipe_t *p = &g_pipes[i];
        if (p->ropen == 0 && p->wopen == 0) {
            p->head  = 0;
            p->nread = 0;
            p->ropen = 1;
            p->wopen = 1;
            return p;
        }
    }
    return (pipe_t*)0;
}

/* Write up to n bytes into the ring buffer.  Returns bytes actually written. */
static uint32_t pipe_ring_write(pipe_t *p, const uint8_t *src, uint32_t n) {
    uint32_t free_space = (uint32_t)PIPE_BUF - p->nread;
    if (n > free_space) n = free_space;
    if (n == 0) return 0;
    uint32_t tail  = (p->head + p->nread) % (uint32_t)PIPE_BUF;
    uint32_t chunk = (uint32_t)PIPE_BUF - tail;  /* contiguous space to end */
    if (chunk > n) chunk = n;
    for (uint32_t i = 0; i < chunk; i++) p->buf[tail + i] = src[i];
    if (chunk < n)
        for (uint32_t i = 0; i < n - chunk; i++) p->buf[i] = src[chunk + i];
    p->nread += n;
    return n;
}

/* Read up to n bytes from the ring buffer.  Returns bytes actually read. */
static uint32_t pipe_ring_read(pipe_t *p, uint8_t *dst, uint32_t n) {
    if (n > p->nread) n = p->nread;
    if (n == 0) return 0;
    uint32_t chunk = (uint32_t)PIPE_BUF - p->head;
    if (chunk > n) chunk = n;
    for (uint32_t i = 0; i < chunk; i++) dst[i] = p->buf[p->head + i];
    if (chunk < n)
        for (uint32_t i = 0; i < n - chunk; i++) dst[chunk + i] = p->buf[i];
    p->head  = (p->head + n) % (uint32_t)PIPE_BUF;
    p->nread -= n;
    return n;
}

/* ── file-descriptor table ────────────────────────────────────────────────
 * Each entry holds a (owner_tid, fd_num) pair so that two threads can each
 * own fd 3 simultaneously (needed for fork).  Linear search is fine for 32
 * entries and O(1)-ish thread counts.
 */
#define FD_TYPE_FILE   0
#define FD_TYPE_PIPE_R 1
#define FD_TYPE_PIPE_W 2

#define SYS_FD_MAX 32

typedef struct {
    int      used;
    int      fd_num;        /* logical fd as seen by userspace */
    uint64_t owner_tid;
    uint8_t  type;          /* FD_TYPE_* */
    /* file: */
    const uint8_t *data;
    uint64_t size;
    uint64_t off;
    /* pipe: */
    pipe_t  *pipe;
} sys_fd_t;

static sys_fd_t g_fds[SYS_FD_MAX];

/* Find the entry for (tid, fd_num), or NULL. */
static sys_fd_t *fd_get(uint64_t tid, int fd_num) {
    for (int i = 0; i < SYS_FD_MAX; i++) {
        sys_fd_t *f = &g_fds[i];
        if (f->used && f->owner_tid == tid && f->fd_num == fd_num)
            return f;
    }
    return (sys_fd_t*)0;
}

/* Return the lowest fd_num ≥ 3 not already used by tid. */
static int fd_next_num(uint64_t tid) {
    for (int n = 3; n < 256; n++)
        if (!fd_get(tid, n)) return n;
    return -1;
}

/* Find a free slot; fill it with type=FILE.  Returns fd_num or -1. */
static int fd_alloc(uint64_t tid, const uint8_t *data, uint64_t size) {
    int fd_num = fd_next_num(tid);
    if (fd_num < 0) return -1;
    for (int i = 0; i < SYS_FD_MAX; i++) {
        if (!g_fds[i].used) {
            g_fds[i].used      = 1;
            g_fds[i].fd_num    = fd_num;
            g_fds[i].owner_tid = tid;
            g_fds[i].type      = FD_TYPE_FILE;
            g_fds[i].data      = data;
            g_fds[i].size      = size;
            g_fds[i].off       = 0;
            g_fds[i].pipe      = (pipe_t*)0;
            return fd_num;
        }
    }
    return -1;
}

/* Decrement pipe refcounts; zero the entry. */
static void fd_close_entry(sys_fd_t *f) {
    if (f->type == FD_TYPE_PIPE_R && f->pipe) { f->pipe->ropen--; }
    if (f->type == FD_TYPE_PIPE_W && f->pipe) { f->pipe->wopen--; }
    f->used = 0; f->fd_num = 0; f->owner_tid = 0;
    f->type = 0; f->data = 0; f->size = 0; f->off = 0; f->pipe = 0;
}

static void fd_close(uint64_t tid, int fd_num) {
    sys_fd_t *f = fd_get(tid, fd_num);
    if (f) fd_close_entry(f);
}

static void fd_close_all(uint64_t tid) {
    for (int i = 0; i < SYS_FD_MAX; i++) {
        if (g_fds[i].used && g_fds[i].owner_tid == tid)
            fd_close_entry(&g_fds[i]);
    }
}

/* Called from SYS_FORK: give the child copies of all parent fds. */
static void fd_fork_inherit(uint64_t parent_tid, uint64_t child_tid) {
    for (int i = 0; i < SYS_FD_MAX; i++) {
        sys_fd_t *p = &g_fds[i];
        if (!p->used || p->owner_tid != parent_tid) continue;
        /* Find a free slot for child's copy */
        for (int j = 0; j < SYS_FD_MAX; j++) {
            if (!g_fds[j].used) {
                g_fds[j] = *p;
                g_fds[j].owner_tid = child_tid;
                if (p->type == FD_TYPE_PIPE_R && p->pipe) p->pipe->ropen++;
                if (p->type == FD_TYPE_PIPE_W && p->pipe) p->pipe->wopen++;
                break;
            }
        }
    }
}

/* dup2(old_fd, new_fd): close new_fd if open, then duplicate old_fd as new_fd. */
static int fd_dup2(uint64_t tid, int old_fd_num, int new_fd_num) {
    sys_fd_t *old = fd_get(tid, old_fd_num);
    if (!old) return -1;
    fd_close(tid, new_fd_num);      /* close new_fd if it was open */
    for (int i = 0; i < SYS_FD_MAX; i++) {
        if (!g_fds[i].used) {
            g_fds[i] = *old;
            g_fds[i].fd_num = new_fd_num;
            if (old->type == FD_TYPE_PIPE_R && old->pipe) old->pipe->ropen++;
            if (old->type == FD_TYPE_PIPE_W && old->pipe) old->pipe->wopen++;
            return new_fd_num;
        }
    }
    return -1;
}

void syscall_dispatch(isr_ctx_t *ctx) {
    /* Deliver any pending signal before handling the syscall.
     * This catches user threads that were signaled while running user code. */
    thread_check_signal();

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
            int code = (int)(unsigned int)ctx->rdi;
            uint64_t tid = thread_current_tid();

            fd_close_all(tid);
            thread_user_map_cleanup_current();
            thread_set_exit_code(code);

            thread_exit();
        }

        case SYS_WRITE: {
            /* rdi = fd (1 = stdout/console), rsi = user buf ptr, rdx = len */
            int      wr_fd = (int)ctx->rdi;
            uint64_t ptr   = ctx->rsi;
            uint64_t len   = ctx->rdx;

            if (len > 4096) len = 4096;

            int from_user = ((ctx->cs & 3ULL) == 3ULL);

            /* Copyin the data first (or use kernel ptr for ring0) */
            uint8_t wr_buf[4096];
            int n;
            if (from_user) {
                n = copyin_bytes(wr_buf, sizeof(wr_buf), ptr, (size_t)len);
                if (n < 0) { ctx->rax = (uint64_t)-1; return; }
            } else {
                n = (int)len;
                if (n > (int)sizeof(wr_buf)) n = (int)sizeof(wr_buf);
                const uint8_t *k = (const uint8_t*)(uintptr_t)ptr;
                for (int i = 0; i < n; i++) wr_buf[i] = k[i];
            }

            /* Check if this fd is a pipe write end */
            uint64_t tid = thread_current_tid();
            sys_fd_t *wf = fd_get(tid, wr_fd);

            if (wf && wf->type == FD_TYPE_PIPE_W) {
                /* Write to pipe — block if full, bail if read end closed */
                uint32_t written = 0;
                while (written < (uint32_t)n) {
                    if (wf->pipe->ropen == 0) { ctx->rax = (uint64_t)-1; return; }
                    uint32_t w = pipe_ring_write(wf->pipe, wr_buf + written,
                                                  (uint32_t)n - written);
                    written += w;
                    if (written < (uint32_t)n) { thread_sleep_ms(1); thread_check_signal(); }
                }
                ctx->rax = (uint64_t)written;
                return;
            }

            /* Default: write to console (fd=1 with no pipe redirect, or ring0) */
            for (int i = 0; i < n; i++) kprintf("%c", (int)wr_buf[i]);
            ctx->rax = (uint64_t)n;
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
            /* rdi = fd, rsi = user buf ptr, rdx = len */
            int      rd_fd = (int)ctx->rdi;
            uint64_t ubuf  = ctx->rsi;
            uint64_t len   = ctx->rdx;

            if (!((ctx->cs & 3ULL) == 3ULL)) { ctx->rax = (uint64_t)-1; return; }
            if (len > 4096) len = 4096;

            uint64_t tid = thread_current_tid();
            sys_fd_t *f = fd_get(tid, rd_fd);
            if (!f) { ctx->rax = (uint64_t)-1; return; }

            if (f->type == FD_TYPE_PIPE_R) {
                /* Blocking pipe read — wait until data available or write end closed */
                uint8_t rd_buf[4096];
                for (;;) {
                    uint32_t got = pipe_ring_read(f->pipe, rd_buf, (uint32_t)len);
                    if (got > 0) {
                        if (copyout_bytes(ubuf, rd_buf, (size_t)got) < 0) {
                            ctx->rax = (uint64_t)-1; return;
                        }
                        ctx->rax = (uint64_t)got;
                        return;
                    }
                    /* Buffer empty */
                    if (f->pipe->wopen == 0) {
                        ctx->rax = 0;   /* EOF — writer is gone */
                        return;
                    }
                    thread_sleep_ms(2);
                    thread_check_signal();
                }
            }

            /* Regular file fd */
            if (f->off >= f->size) { ctx->rax = 0; return; }
            uint64_t remain = f->size - f->off;
            uint64_t n = (len < remain) ? len : remain;
            if (copyout_bytes(ubuf, f->data + (size_t)f->off, (size_t)n) < 0) {
                ctx->rax = (uint64_t)-1; return;
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
            /* rdi = path, rsi = user argv ptr (NULL-terminated array, or 0) */
            uint64_t upath = ctx->rdi;
            uint64_t uargv = ctx->rsi;
            char path[256];
            if (copyin_str(path, sizeof(path), upath) < 0) {
                ctx->rax = (uint64_t)-1;
                break;
            }

            /* Copy argv strings from user space onto the kernel stack */
            #define EXEC_KMAX_ARGS 8
            #define EXEC_KARG_LEN  128
            char arg_store[EXEC_KMAX_ARGS][EXEC_KARG_LEN];
            const char *argv_k[EXEC_KMAX_ARGS + 1];
            int argc_k = 0;

            if (uargv != 0) {
                for (int i = 0; i < EXEC_KMAX_ARGS; i++) {
                    uint64_t uptr_addr = uargv + (uint64_t)i * 8;
                    if (!vmm_user_accessible(uptr_addr, 8, false)) break;
                    uint64_t ustr = *(volatile uint64_t*)(uintptr_t)uptr_addr;
                    if (ustr == 0) break;
                    if (copyin_str(arg_store[argc_k], EXEC_KARG_LEN, ustr) < 0) break;
                    argv_k[argc_k] = arg_store[argc_k];
                    argc_k++;
                }
            }
            argv_k[argc_k] = (const char*)0;

            int r = exec_load(ctx, path, argc_k, argv_k);
            if (r < 0) ctx->rax = (uint64_t)-1;
            /* on success exec_load modified ctx for iretq to new program */
            break;
        }

        case SYS_FORK: {
            uint64_t fork_parent = thread_current_tid();
            long fork_child = do_fork(ctx);
            if (fork_child > 0) {
                fd_fork_inherit(fork_parent, (uint64_t)fork_child);
            }
            ctx->rax = (uint64_t)fork_child;
            break;
        }

        case SYS_WAITPID: {
            /* rdi = child TID (-1 = any), rsi = user int* for exit code */
            uint32_t child_tid = (uint32_t)(uint64_t)ctx->rdi;
            uint64_t ucode     = ctx->rsi;
            uint32_t me        = thread_current_tid();

            /* Poll up to ~5 s (500 × 10 ms) for child to become zombie */
            for (int attempt = 0; attempt < 500; attempt++) {
                int code = 0;
                long reaped = thread_reap_zombie_child(me, child_tid, &code);
                if (reaped > 0) {
                    if (ucode) copyout_bytes(ucode, (const uint8_t*)&code, sizeof(int));
                    ctx->rax = (uint64_t)(uint32_t)reaped;
                    return;
                }
                if (reaped == -2L) {
                    ctx->rax = (uint64_t)-1; /* no matching child */
                    return;
                }
                /* child alive but not zombie yet — yield and retry */
                thread_sleep_ms(10);
                thread_check_signal();
            }
            ctx->rax = (uint64_t)-1; /* timeout */
            return;
        }

        case SYS_BRK: {
            uint64_t new_brk = ctx->rdi;
            uint64_t cur_brk = thread_get_brk();

            /* brk(0) = query current break */
            if (new_brk == 0) {
                ctx->rax = cur_brk;
                break;
            }

            /* Clamp: must stay in user space and be page-aligned or we round up */
            const uint64_t PAGE = 0x1000ULL;
            new_brk = (new_brk + PAGE - 1) & ~(PAGE - 1);

            if (new_brk >= (uint64_t)FIFI_USER_TOP || new_brk < PAGE) {
                ctx->rax = cur_brk;  /* reject — return old break */
                break;
            }

            if (new_brk > cur_brk) {
                /* Grow: map new pages */
                uint64_t v = cur_brk;
                for (; v < new_brk; v += PAGE) {
                    uint64_t phys = pmm_alloc_page();
                    if (!phys) goto brk_oom;
                    if (!vmm_map_page(v, phys, VMM_USER | VMM_WRITE | VMM_NX)) {
                        pmm_free_page(phys);
                        goto brk_oom;
                    }
                    /* zero via direct virtual access (heap pages start clean) */
                    volatile uint8_t *p = (volatile uint8_t*)(uintptr_t)v;
                    for (uint64_t b = 0; b < PAGE; b++) p[b] = 0;
                }
                thread_user_map_add(cur_brk, new_brk - cur_brk);
                thread_set_brk(new_brk);
                ctx->rax = new_brk;
                break;

            brk_oom:
                /* Partial grow failed — unmap what we managed to map */
                vmm_unmap_range_and_free(cur_brk, v - cur_brk);
                ctx->rax = cur_brk;  /* return old break on failure */
                break;

            } else if (new_brk < cur_brk) {
                /* Shrink: unmap pages */
                vmm_unmap_range_and_free(new_brk, cur_brk - new_brk);
                thread_set_brk(new_brk);
                ctx->rax = new_brk;
                break;

            } else {
                /* No change */
                ctx->rax = cur_brk;
                break;
            }
        }

        case SYS_LISTFILES: {
            /* rdi = user buf ptr, rsi = capacity (max 4096) */
            uint64_t ubuf = ctx->rdi;
            uint64_t cap  = ctx->rsi;
            if (!ubuf || cap == 0) { ctx->rax = (uint64_t)-1; return; }
            if (cap > 4096) cap = 4096;

            static char ls_kbuf[4096];
            size_t ls_n = vfs_list(ls_kbuf, (size_t)cap);
            if (copyout_bytes(ubuf, (const uint8_t*)ls_kbuf, ls_n) < 0) {
                ctx->rax = (uint64_t)-1; return;
            }
            ctx->rax = (uint64_t)ls_n;
            return;
        }

        case SYS_GETCHAR: {
            /* Block until a key is available; returns ASCII/KEY_* value */
            for (;;) {
                int c = keyboard_try_getchar();
                if (c >= 0) {
                    ctx->rax = (uint64_t)(unsigned int)c;
                    return;
                }
                thread_sleep_ms(10);
                thread_check_signal();
            }
        }

        case SYS_PIPE: {
            /* rdi = user int[2] to receive [read_fd, write_fd] */
            uint64_t ufd_arr = ctx->rdi;
            if (!((ctx->cs & 3ULL) == 3ULL)) { ctx->rax = (uint64_t)-1; return; }

            pipe_t *pp = pipe_alloc();
            if (!pp) { ctx->rax = (uint64_t)-1; return; }

            uint64_t tid = thread_current_tid();
            /* Allocate read end first, then write end, at next free fd numbers */
            int rfd_n = fd_next_num(tid);
            if (rfd_n < 0) { pp->ropen = pp->wopen = 0; ctx->rax = (uint64_t)-1; return; }
            /* Temporarily mark it used so wfd_n gets a different number */
            int rfd_slot = -1;
            for (int i = 0; i < SYS_FD_MAX; i++) {
                if (!g_fds[i].used) { rfd_slot = i; break; }
            }
            if (rfd_slot < 0) { pp->ropen = pp->wopen = 0; ctx->rax = (uint64_t)-1; return; }
            g_fds[rfd_slot].used = 1;  /* reserve slot */
            g_fds[rfd_slot].fd_num = rfd_n;
            g_fds[rfd_slot].owner_tid = tid;

            int wfd_n = fd_next_num(tid);
            if (wfd_n < 0) {
                g_fds[rfd_slot].used = 0;
                pp->ropen = pp->wopen = 0;
                ctx->rax = (uint64_t)-1; return;
            }
            /* Finish populating rfd_slot and find wfd_slot */
            g_fds[rfd_slot].type = FD_TYPE_PIPE_R;
            g_fds[rfd_slot].data = (const uint8_t*)0;
            g_fds[rfd_slot].size = g_fds[rfd_slot].off = 0;
            g_fds[rfd_slot].pipe = pp;
            /* ropen was pre-set to 1 by pipe_alloc; bump again since we also count this fd */
            /* Actually pipe_alloc already set ropen=1 and wopen=1 so we're good as is.
             * But fd_alloc_pipe_at would have done ropen++ again — avoid double-count. */

            int wfd_slot = -1;
            for (int i = 0; i < SYS_FD_MAX; i++) {
                if (!g_fds[i].used) { wfd_slot = i; break; }
            }
            if (wfd_slot < 0) {
                fd_close_entry(&g_fds[rfd_slot]);
                ctx->rax = (uint64_t)-1; return;
            }
            g_fds[wfd_slot].used      = 1;
            g_fds[wfd_slot].fd_num    = wfd_n;
            g_fds[wfd_slot].owner_tid = tid;
            g_fds[wfd_slot].type      = FD_TYPE_PIPE_W;
            g_fds[wfd_slot].data      = (const uint8_t*)0;
            g_fds[wfd_slot].size      = g_fds[wfd_slot].off = 0;
            g_fds[wfd_slot].pipe      = pp;
            /* wopen already 1 from pipe_alloc */

            /* Write the two fd numbers into user memory */
            int fds_out[2];
            fds_out[0] = rfd_n;
            fds_out[1] = wfd_n;
            if (copyout_bytes(ufd_arr, (const uint8_t*)fds_out, sizeof(fds_out)) < 0) {
                fd_close_entry(&g_fds[rfd_slot]);
                fd_close_entry(&g_fds[wfd_slot]);
                ctx->rax = (uint64_t)-1; return;
            }
            ctx->rax = 0;
            return;
        }

        case SYS_DUP2: {
            /* rdi = old_fd, rsi = new_fd */
            if (!((ctx->cs & 3ULL) == 3ULL)) { ctx->rax = (uint64_t)-1; return; }
            int old_fd = (int)ctx->rdi;
            int new_fd = (int)ctx->rsi;
            uint64_t tid = thread_current_tid();
            int r = fd_dup2(tid, old_fd, new_fd);
            ctx->rax = (r >= 0) ? (uint64_t)r : (uint64_t)-1;
            return;
        }

        default:
            kprintf("[syscall] unknown n=%p (ctx->rax=%p)\n", (void*)n, (void*)ctx->rax);
            ctx->rax = (uint64_t)-1;
            return;

    }
}
