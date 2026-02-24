#include "thread.h"
#include "kprintf.h"
#include "heap.h"

#include <stdint.h>
#include <stdbool.h>
#include "pit.h"

extern void ctx_switch(uint64_t *old_rsp, uint64_t *new_rsp);

#define THREAD_MAX        16
#define THREAD_NAME_MAX   24
#define THREAD_STACK_SIZE (16 * 1024)

typedef enum {
    T_UNUSED = 0,
    T_READY,
    T_RUNNING,
    T_SLEEPING,
    T_DEAD
} thread_state_t;

typedef struct {
    uint64_t rsp;
    thread_state_t state;
    uint8_t prio;
    uint32_t tid;
    uint64_t wake_tick;
    uint64_t cpu_ticks;
    uint64_t last_start_tick;

    char name[THREAD_NAME_MAX];

    thread_entry_t entry;
    void *arg;

    void *stack_base;
} thread_t;

static thread_t g_threads[THREAD_MAX];
static int g_cur_idx = 0;
static volatile int g_need_resched = 0;
static uint64_t g_boot_tick = 0;
static uint32_t g_next_tid = 1;

static volatile int g_preempt_enabled = 1;

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
        g_threads[i].wake_tick = 0;
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
    g_cur->prio = 1;
    name_copy(g_cur->name, "main");

    g_boot_tick = pit_get_ticks();
    g_cur->cpu_ticks = 0;
    g_cur->last_start_tick = g_boot_tick;
    g_cur->tid = g_next_tid++;
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
    t->tid = g_next_tid++;
    t->prio = 1;
    t->entry = entry;
    t->arg = arg;
    t->stack_base = stack;
    t->cpu_ticks = 0;
    t->last_start_tick = 0;
    name_copy(t->name, name);

    return idx;
}


static int thread_wake_ready(void) {
    int woke = 0;
    uint64_t now = pit_get_ticks();
    for (int i = 0; i < THREAD_MAX; i++) {
        if (g_threads[i].state == T_SLEEPING && g_threads[i].wake_tick <= now) {
            g_threads[i].state = T_READY;
            woke = 1;
        }
    }
    return woke;
}


static int highest_ready_prio(void) {
    int best = -1;
    for (int i = 0; i < THREAD_MAX; i++) {
        if (g_threads[i].state == T_READY) {
            int p = (int)g_threads[i].prio;
            if (p > best) best = p;
        }
    }
    return best;
}

static int pick_next_ready(void) {
    if (!g_cur) return 0;

    int best = highest_ready_prio();
    if (best < 0) return -1;

    // round-robin among threads at the best priority
    for (int step = 1; step <= THREAD_MAX; step++) {
        int i = (g_cur_idx + step) % THREAD_MAX;
        if (g_threads[i].state == T_READY && (int)g_threads[i].prio == best) {
            return i;
        }
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

    uint64_t now = pit_get_ticks();
    if (prev->last_start_tick != 0 && now >= prev->last_start_tick) {
        prev->cpu_ticks += (now - prev->last_start_tick);
    }
    next->last_start_tick = now;
    ctx_switch(&prev->rsp, &next->rsp);

    /* For existing threads, ctx_switch returns to the yield() callsite.
       For new threads, it RETs into thread_trampoline which does its own STI. */
    k_sti();
}

void thread_request_resched(void) {
    if (!g_preempt_enabled) return;
    g_need_resched = 1;
}

void thread_check_resched(void) {
    int woke_any = thread_wake_ready();
    if (woke_any) g_need_resched = 1;
    if (!g_need_resched) return;
    g_need_resched = 0;
    thread_yield();
}


__attribute__((noreturn))
void thread_exit(void) {
    k_cli();

    if (g_cur) {
        uint64_t now = pit_get_ticks();
        if (g_cur->last_start_tick != 0 && now >= g_cur->last_start_tick) {
            g_cur->cpu_ticks += (now - g_cur->last_start_tick);
        }
        g_cur->state = T_DEAD;
        kprintf("\n[thread] exit: %s\n", g_cur->name);
    }

    /* switch away forever */
    for (;;) {
        thread_yield();
        __asm__ volatile("hlt");
    }
}


static uint64_t thread_effective_cpu_ticks(thread_t *t) {
    uint64_t base = t->cpu_ticks;
    if (t->state == T_RUNNING) {
        uint64_t now = pit_get_ticks();
        if (t->last_start_tick != 0 && now >= t->last_start_tick) {
            base += (now - t->last_start_tick);
        }
    }
    return base;
}

static const char *state_str(thread_state_t s) {
    switch (s) {
        case T_UNUSED:  return "UNUSED";
        case T_READY:   return "READY";
        case T_RUNNING: return "RUNNING";
        case T_SLEEPING: return "SLEEPING";
        case T_DEAD:    return "DEAD";
        default:        return "?";
    }
}




void thread_dump(void) {
    uint64_t now = pit_get_ticks();
    uint64_t total = (now >= g_boot_tick) ? (now - g_boot_tick) : 0;

    kprintf("\n[threads]\n");
    for (int i = 0; i < THREAD_MAX; i++) {
        if (g_threads[i].state == T_UNUSED) continue;

        uint64_t ct = thread_effective_cpu_ticks(&g_threads[i]);
        unsigned long long cpu = (unsigned long long)ct;
        unsigned pct = 0;
        if (total != 0) pct = (unsigned)((ct * 100) / total);

        kprintf("  slot=%d tid=%u %-8s %-10s p=%u cpu=%llu pct=%u\n",
            i,
            (unsigned)g_threads[i].tid,
            state_str(g_threads[i].state),
            g_threads[i].name,
            (unsigned)g_threads[i].prio,
            cpu,
            pct);
    }
}





void thread_sleep_ms(uint64_t ms) {
    /* PIT is 100Hz right now (10ms/tick). ms->ticks rounding up */
    uint64_t ticks = (ms + 9) / 10;
    if (ticks == 0) ticks = 1;

    k_cli();

    uint64_t now = pit_get_ticks();
    if (g_cur->last_start_tick != 0 && now >= g_cur->last_start_tick) {
        g_cur->cpu_ticks += (now - g_cur->last_start_tick);
    }

    g_cur->wake_tick = now + ticks;
    g_cur->state = T_SLEEPING;

    /* request resched and switch */
    g_need_resched = 1;
    k_sti();
    thread_yield();
}



int thread_resched_pending(void) {
    return g_need_resched ? 1 : 0;
}

int thread_preempt_get(void) {
    return g_preempt_enabled ? 1 : 0;
}

void thread_preempt_set(int on) {
    g_preempt_enabled = on ? 1 : 0;
}


void thread_cpu_reset(void) {
    k_cli();
    uint64_t now = pit_get_ticks();
    g_boot_tick = now;
    for (int i = 0; i < THREAD_MAX; i++) {
        if (g_threads[i].state == T_UNUSED) continue;
        g_threads[i].cpu_ticks = 0;
        if (g_threads[i].state == T_RUNNING) g_threads[i].last_start_tick = now;
        else g_threads[i].last_start_tick = 0;
    }
    k_sti();
    kprintf("cpu: reset\n");
}



void thread_top(void) {
    uint64_t now = pit_get_ticks();
    uint64_t total = (now >= g_boot_tick) ? (now - g_boot_tick) : 0;

    int idx[THREAD_MAX];
    int n = 0;
    for (int i = 0; i < THREAD_MAX; i++) {
        if (g_threads[i].state != T_UNUSED) idx[n++] = i;
    }

    for (int a = 0; a < n; a++) {
        for (int b = 0; b + 1 < n; b++) {
            uint64_t ca = thread_effective_cpu_ticks(&g_threads[idx[b]]);
            uint64_t cb = thread_effective_cpu_ticks(&g_threads[idx[b+1]]);
            if (cb > ca) {
                int tmp = idx[b];
                idx[b] = idx[b+1];
                idx[b+1] = tmp;
            }
        }
    }

    kprintf("\n[top] total_ticks=%u\n", (unsigned)(total & 0xffffffffu));
    for (int k = 0; k < n; k++) {
        int i = idx[k];
        uint64_t ct = thread_effective_cpu_ticks(&g_threads[i]);
        unsigned long long cpu = (unsigned long long)ct;
        unsigned pct = 0;
        if (total != 0) pct = (unsigned)((ct * 100) / total);

        kprintf("  slot=%d tid=%u %-8s %-10s p=%u cpu=%llu pct=%u\n",
            i,
            (unsigned)g_threads[i].tid,
            state_str(g_threads[i].state),
            g_threads[i].name,
            (unsigned)g_threads[i].prio,
            cpu,
            pct);
    }
}




int thread_set_prio(int id, int prio) {
    if (prio < 0) return -1;
    if (prio > 3) return -1;
    if (id < 0 || id >= THREAD_MAX) return -1;
    if (g_threads[id].state == T_UNUSED) return -1;
    g_threads[id].prio = (uint8_t)(prio & 0xff);
    return 0;
}

int thread_get_prio(int id) {
    if (id < 0 || id >= THREAD_MAX) return -1;
    if (g_threads[id].state == T_UNUSED) return -1;
    return (int)g_threads[id].prio;
}


static void spin_fn(void *arg) {
    (void)arg;
    for (;;) {
        // burn a little CPU, then yield cooperatively
        for (volatile uint64_t j = 0; j < 3000000; j++) { }
        thread_yield();
    }
}

int thread_spawn_spin(void) {
    int id = thread_create("spin", spin_fn, 0);
    if (id >= 0) {
        kprintf("\n[thread] spawned spin as id=%d\n", id);
    } else {
        kprintf("\n[thread] spawn spin FAILED\n");
    }
    return id;
}

/* ===== demo thread so we can prove switching works ===== */

static void demo_fn(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        kprintf("\n[demo] i=%d\n", i);
        /* cheap delay so it doesn't spam instantly */
        thread_sleep_ms(250);thread_yield();
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
