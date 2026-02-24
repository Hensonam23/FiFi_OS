#include "thread.h"
#include "kprintf.h"
#include "heap.h"

#include <stdint.h>
#include <stdbool.h>

extern void ctx_switch(uint64_t *old_rsp, uint64_t *new_rsp);

#define THREAD_MAX        16
#define THREAD_NAME_MAX   24
#define THREAD_STACK_SIZE (16 * 1024)

typedef enum {
    T_UNUSED = 0,
    T_READY,
    T_RUNNING,
    T_DEAD
} thread_state_t;

typedef struct {
    uint64_t rsp;
    thread_state_t state;

    char name[THREAD_NAME_MAX];

    thread_entry_t entry;
    void *arg;

    void *stack_base;
} thread_t;

static thread_t g_threads[THREAD_MAX];
static int g_cur_idx = 0;
static volatile int g_need_resched = 0;

static thread_t *g_cur = 0;

static inline void k_cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void k_sti(void) { __asm__ volatile("sti" ::: "memory"); }

/* Very small strncpy */
static void name_copy(char *dst, const char *src) {
    if (!dst) return;
    if (!src) { dst[0] = 0; return; }
    for (int i = 0; i < THREAD_NAME_MAX - 1; i++) {
        char c = src[i];
        dst[i] = c;
        if (!c) return;
    }
    dst[THREAD_NAME_MAX - 1] = 0;
}

/* This runs when a brand new thread is first switched to. */
__attribute__((noreturn))
static void thread_trampoline(void) {
    /* Important: yield() disables IRQs before switching.
       A brand new thread enters here via RET, not via the yield() callsite,
       so we must re-enable IRQs here. */
    k_sti();

    thread_entry_t fn = g_cur->entry;
    void *arg = g_cur->arg;

    fn(arg);

    thread_exit();
}

void thread_init(void) {
    for (int i = 0; i < THREAD_MAX; i++) {
        g_threads[i].state = T_UNUSED;
        g_threads[i].rsp = 0;
        g_threads[i].entry = 0;
        g_threads[i].arg = 0;
        g_threads[i].stack_base = 0;
        g_threads[i].name[0] = 0;
    }

    /* slot 0 = "main" thread (current context) */
    g_cur_idx = 0;
    g_cur = &g_threads[0];
    g_cur->state = T_RUNNING;
    name_copy(g_cur->name, "main");

    kprintf("FiFi OS: thread_init OK (main thread)\n");
}

static int find_free_slot(void) {
    for (int i = 1; i < THREAD_MAX; i++) {
        if (g_threads[i].state == T_UNUSED || g_threads[i].state == T_DEAD) {
            return i;
        }
    }
    return -1;
}

int thread_create(const char *name, thread_entry_t entry, void *arg) {
    if (!entry) return -1;

    int idx = find_free_slot();
    if (idx < 0) return -1;

    thread_t *t = &g_threads[idx];
    void *stack = kmalloc_aligned(THREAD_STACK_SIZE, 16);
    if (!stack) return -1;

    /* build initial stack frame to match ctx_switch pop order:
       pop r15,r14,r13,r12,rbx,rbp,ret */
    uintptr_t top = (uintptr_t)stack + THREAD_STACK_SIZE;
    top &= ~(uintptr_t)0xF; /* align */

    uint64_t *sp = (uint64_t *)top;

    *(--sp) = (uint64_t)thread_trampoline; /* ret address */

    *(--sp) = 0; /* rbp */
    *(--sp) = 0; /* rbx */
    *(--sp) = 0; /* r12 */
    *(--sp) = 0; /* r13 */
    *(--sp) = 0; /* r14 */
    *(--sp) = 0; /* r15 */

    t->rsp = (uint64_t)(uintptr_t)sp;
    t->state = T_READY;
    t->entry = entry;
    t->arg = arg;
    t->stack_base = stack;
    name_copy(t->name, name);

    return idx;
}

static int pick_next_ready(void) {
    if (!g_cur) return 0;

    for (int step = 1; step <= THREAD_MAX; step++) {
        int i = (g_cur_idx + step) % THREAD_MAX;
        if (g_threads[i].state == T_READY) return i;
    }
    return -1;
}

void thread_yield(void) {
    if (!g_cur) return;

    k_cli();

    int next_idx = pick_next_ready();
    if (next_idx < 0) {
        k_sti();
        return; /* nobody else ready */
    }

    thread_t *prev = g_cur;
    thread_t *next = &g_threads[next_idx];

    if (prev->state == T_RUNNING) prev->state = T_READY;
    next->state = T_RUNNING;

    g_cur_idx = next_idx;
    g_cur = next;

    ctx_switch(&prev->rsp, &next->rsp);

    /* For existing threads, ctx_switch returns to the yield() callsite.
       For new threads, it RETs into thread_trampoline which does its own STI. */
    k_sti();
}

void thread_request_resched(void) {
    g_need_resched = 1;
}

void thread_check_resched(void) {
    if (!g_need_resched) return;
    g_need_resched = 0;
    thread_yield();
}


__attribute__((noreturn))
void thread_exit(void) {
    k_cli();

    if (g_cur) {
        g_cur->state = T_DEAD;
        kprintf("\n[thread] exit: %s\n", g_cur->name);
    }

    /* switch away forever */
    for (;;) {
        thread_yield();
        __asm__ volatile("hlt");
    }
}

static const char *state_str(thread_state_t s) {
    switch (s) {
        case T_UNUSED:  return "UNUSED";
        case T_READY:   return "READY";
        case T_RUNNING: return "RUNNING";
        case T_DEAD:    return "DEAD";
        default:        return "?";
    }
}

void thread_dump(void) {
    kprintf("\n[threads]\n");
    for (int i = 0; i < THREAD_MAX; i++) {
        if (g_threads[i].state == T_UNUSED) continue;
        kprintf("  %d: %-7s  %-10s  rsp=%p\n",
            i,
            state_str(g_threads[i].state),
            g_threads[i].name,
            (void*)(uintptr_t)g_threads[i].rsp);
    }
}

/* ===== demo thread so we can prove switching works ===== */

static void demo_fn(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        kprintf("\n[demo] i=%d\n", i);
        /* cheap delay so it doesn't spam instantly */
        for (volatile uint64_t j = 0; j < 8000000; j++) { }
        thread_yield();
    }
    kprintf("\n[demo] done\n");
}

int thread_spawn_demo(void) {
    int id = thread_create("demo", demo_fn, 0);
    if (id >= 0) {
        kprintf("\n[thread] spawned demo as id=%d\n", id);
    } else {
        kprintf("\n[thread] spawn demo FAILED\n");
    }
    return id;
}
