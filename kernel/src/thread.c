#include "thread.h"
#include "kprintf.h"
#include "heap.h"
#include "gdt.h"
#include "vmm.h"

// forward decl (used by scheduler)
static inline uint64_t read_rsp_u64(void) {
    uint64_t v;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(v));
    return v;
}


#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "pit.h"

extern void ctx_switch(uint64_t *old_rsp, uint64_t *new_rsp);

#define THREAD_MAX        32
#define THREAD_NAME_MAX   24
#define THREAD_STACK_SIZE (16 * 1024)

typedef enum {
    T_UNUSED = 0,
    T_READY,
    T_RUNNING,
    T_SLEEPING,
    T_DEAD,
    T_ZOMBIE,   /* exited; waiting for parent to collect exit status */
    T_STOPPED,  /* suspended by SIGTSTP; resumes on SIGCONT */
} thread_state_t;

#define THREAD_USER_MAP_MAX 8

typedef struct {
    uint64_t base;
    uint64_t size;
    uint8_t in_use;
} thread_user_map_t;

typedef struct {
    uint64_t rsp;
    uint64_t kstack_top;  // top of this thread’s kernel stack (for TSS.rsp0)
    thread_state_t state;
    uint8_t prio;
    uint32_t tid;
    uint32_t wait_ticks;
    uint64_t wake_tick;
    uint64_t cpu_ticks;
    uint64_t last_start_tick;

    char name[THREAD_NAME_MAX];

    thread_entry_t entry;
    void *arg;

    void *stack_base;
    uint8_t is_user;
    uint64_t cr3;          /* physical addr of this task's PML4, 0 = use kernel CR3 */
    uint64_t user_brk;     /* current program break (heap top), 0 = not set */
    uint32_t parent_tid;   /* TID of parent (0 = no parent, exit goes T_DEAD) */
    volatile int exit_code;/* stored before thread_exit; read by waitpid */
    volatile int sig_pending; /* signal number pending (0=none, 2=SIGINT, 19=SIGTSTP…) */
    char cwd[256];            /* current working directory, absolute path */
    thread_user_map_t user_maps[THREAD_USER_MAP_MAX];
    uint32_t pgid;            /* process group ID (0 = same as tid) */
    uint8_t  stop_reported;   /* 1 = T_STOPPED already returned by waitflags */
    uint8_t  _pad[3];
    uint64_t sig_handlers[32];/* user-space handler VAs (0=SIG_DFL, 1=SIG_IGN, else VA) */
    uint64_t mmap_next;       /* next anon mmap VA for this thread */
} thread_t;

static thread_t g_threads[THREAD_MAX];
static thread_t *g_cur = 0;


// Kernel stack top for the currently running thread (for TSS.rsp0)
uint64_t thread_current_kstack_top(void) {
    // Prefer tracked per-thread value if present
    if (g_cur && g_cur->kstack_top) return g_cur->kstack_top;

    // Fallback: compute from allocated stack_base
    if (g_cur && g_cur->stack_base) {
        return (uint64_t)(uintptr_t)g_cur->stack_base + (uint64_t)THREAD_STACK_SIZE;
    }

    return 0;
}


// Current thread id (tid)
uint32_t thread_current_tid(void) {
    return g_cur ? g_cur->tid : 0;
}

static int g_cur_idx = 0;
static volatile int g_need_resched = 0;
static uint64_t g_boot_tick = 0;
static uint32_t g_next_tid = 1;

static volatile int g_preempt_enabled = 1;
static volatile int g_aging_enabled = 1;
static volatile int g_timeslice_ticks = 3; // 100Hz ticks (~30ms)
static volatile int g_slice_left = 3;
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

static void thread_user_maps_zero(thread_t *t) {
    if (!t) return;
    for (int i = 0; i < THREAD_USER_MAP_MAX; i++) {
        t->user_maps[i].base = 0;
        t->user_maps[i].size = 0;
        t->user_maps[i].in_use = 0;
    }
}

static void thread_user_cleanup(thread_t *t) {
    if (!t) return;

    for (int i = 0; i < THREAD_USER_MAP_MAX; i++) {
        if (!t->user_maps[i].in_use) continue;
        (void)vmm_unmap_range_and_free(t->user_maps[i].base, (size_t)t->user_maps[i].size);
        t->user_maps[i].base = 0;
        t->user_maps[i].size = 0;
        t->user_maps[i].in_use = 0;
    }

    t->is_user = 0;
}

void g_cur_set_cr3(uint64_t cr3_phys) {
    if (g_cur) g_cur->cr3 = cr3_phys;
}

uint64_t g_cur_cr3(void) {
    return g_cur ? g_cur->cr3 : 0;
}

void thread_user_maps_zero_current(void) {
    if (g_cur) thread_user_maps_zero(g_cur);
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
        g_threads[i].kstack_top = 0;
        g_threads[i].tid = 0;
        g_threads[i].prio = 0;
        g_threads[i].wait_ticks = 0;
        g_threads[i].wake_tick = 0;
        g_threads[i].cpu_ticks = 0;
        g_threads[i].last_start_tick = 0;
        g_threads[i].entry = 0;
        g_threads[i].arg = 0;
        g_threads[i].stack_base = 0;
        g_threads[i].is_user = 0;
        g_threads[i].name[0] = 0;
        g_threads[i].wake_tick = 0;
        g_threads[i].rsp = 0;
        g_threads[i].entry = 0;
        g_threads[i].arg = 0;
        g_threads[i].stack_base = 0;
        g_threads[i].is_user = 0;
        g_threads[i].kstack_top = 0;
        g_threads[i].tid = 0;
        g_threads[i].prio = 0;
        g_threads[i].wait_ticks = 0;
        g_threads[i].cpu_ticks = 0;
        g_threads[i].last_start_tick = 0;
        g_threads[i].name[0] = 0;
        g_threads[i].user_brk = 0;
        g_threads[i].parent_tid = 0;
        g_threads[i].exit_code = 0;
        g_threads[i].pgid = 0;
        g_threads[i].stop_reported = 0;
        for (int j = 0; j < 32; j++) g_threads[i].sig_handlers[j] = 0;
        g_threads[i].mmap_next = 0x0000600000000000ULL;
        thread_user_maps_zero(&g_threads[i]);
    }

    /* slot 0 = "main" thread (current context) */
    g_cur_idx = 0;
    g_cur = &g_threads[0];


    g_cur->state = T_RUNNING;
    g_cur->prio = 1;
    g_cur->cwd[0] = '/'; g_cur->cwd[1] = '\0';
    name_copy(g_cur->name, "main");

    g_boot_tick = pit_get_ticks();
    g_cur->cpu_ticks = 0;
    g_cur->last_start_tick = g_boot_tick;
    g_cur->tid = g_next_tid++;
    g_cur->wait_ticks = 0;
    g_slice_left = g_timeslice_ticks;
    kprintf("FiFi OS: thread_init OK (main thread)\n");

    // FiFi OS: bootstrap thread (slot 0) kernel stack top for TSS.rsp0
    // This is a safe fallback until boot stack is modeled explicitly.
    g_threads[0].kstack_top = read_rsp_u64();
    gdt_tss_set_rsp0(g_threads[0].kstack_top);

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
    // reclaim DEAD slots before allocating a new thread slot
    thread_reap_dead();
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

    t->kstack_top = (uint64_t)top; /* TSS.rsp0 top-of-kstack */

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
    t->wait_ticks = 0;
    t->prio = 1;
    t->entry = entry;
    t->arg = arg;
    t->stack_base = stack;
    t->is_user = 0;
    t->user_brk = 0;
    t->parent_tid = 0;
    t->exit_code = 0;
    t->pgid = 0;
    t->stop_reported = 0;
    for (int j = 0; j < 32; j++) t->sig_handlers[j] = 0;
    t->mmap_next = 0x0000600000000000ULL;
    t->cwd[0] = '/'; t->cwd[1] = '\0';
    thread_user_maps_zero(t);
    t->kstack_top = (uint64_t)(uintptr_t)stack + (uint64_t)THREAD_STACK_SIZE;

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
static int eff_prio(thread_t *t);



static int highest_ready_eff_prio(void) {
    int best = -1;
    for (int i = 0; i < THREAD_MAX; i++) {
        if (g_threads[i].state == T_READY) {
            int ep = eff_prio(&g_threads[i]);
            if (ep > best) best = ep;
        }
    }
    return best;
}


static int pick_next_ready(void) {
    if (!g_cur) return 0;

    int best = highest_ready_eff_prio();
    if (best < 0) return -1;

    for (int step = 1; step <= THREAD_MAX; step++) {
        int i = (g_cur_idx + step) % THREAD_MAX;
        if (g_threads[i].state == T_READY && eff_prio(&g_threads[i]) == best) {
            return i;
        }
    }
    return -1;
}



void thread_yield(void) {
    if (!g_cur) return;

    k_cli();

    // reclaim dead threads so slots can be reused
    thread_reap_dead();


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
    next->wait_ticks = 0;
    g_slice_left = g_timeslice_ticks;
    // FiFi OS: per-thread kernel stack for ring3->ring0 transitions
    // Update TSS.rsp0 so ring3->ring0 transitions use the NEXT thread's kernel stack
    uint64_t ktop = next->kstack_top;
    if (!ktop && next->stack_base) ktop = (uint64_t)(uintptr_t)next->stack_base + (uint64_t)THREAD_STACK_SIZE;
    if (ktop) gdt_tss_set_rsp0(ktop);

    /* Load per-process page map if the next thread has one */
    if (next->cr3) {
        vmm_switch_to(next->cr3);
    } else {
        vmm_switch_to(vmm_get_kernel_cr3());
    }

    ctx_switch(&prev->rsp, &next->rsp);

    /* For existing threads, ctx_switch returns to the yield() callsite.
       For new threads, it RETs into thread_trampoline which does its own STI. */
    k_sti();
}


void thread_request_resched(void) {
    // called from IRQ0 (timer tick). Do NOT print here.

    // Aging: increment wait time for READY threads
    for (int i = 0; i < THREAD_MAX; i++) {
        if (g_threads[i].state == T_READY) {
            if (g_threads[i].wait_ticks < 1000000u) g_threads[i].wait_ticks++;
        }
    }

    // Timeslice: count down current running slice
    if (g_slice_left > 0) g_slice_left--;
    if (g_slice_left <= 0) {
        g_slice_left = g_timeslice_ticks;
        if (g_preempt_enabled) {
            g_need_resched = 1;
        }
    }
}


void thread_check_resched(void) {
    thread_reap_dead();
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

        /* If this thread owns a per-process page map, switch back to the
         * kernel map first, then destroy the user map entirely. */
        if (g_cur->cr3) {
            uint64_t dying_cr3 = g_cur->cr3;
            g_cur->cr3 = 0;
            vmm_switch_to(vmm_get_kernel_cr3());
            vmm_destroy_user_pagemap(dying_cr3);
            thread_user_maps_zero(g_cur); /* tracking already freed — just zero */
        } else {
            thread_user_cleanup(g_cur);
        }

        /* Go T_ZOMBIE if our parent is still alive so it can collect our
         * exit status via waitpid; otherwise go straight to T_DEAD. */
        int go_zombie = 0;
        if (g_cur->parent_tid != 0) {
            for (int _i = 0; _i < THREAD_MAX; _i++) {
                if (g_threads[_i].tid == g_cur->parent_tid &&
                    g_threads[_i].state != T_UNUSED &&
                    g_threads[_i].state != T_DEAD &&
                    g_threads[_i].state != T_ZOMBIE) {
                    go_zombie = 1;
                    break;
                }
            }
        }
        g_cur->state = go_zombie ? T_ZOMBIE : T_DEAD;
    }

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


static int eff_prio(thread_t *t) {
    int p = (int)t->prio;
    if (!g_aging_enabled) return p;

    // Boost based on how long it's been READY
    const int AGING_QUANTUM = 10; // ticks (~100ms)
    const int MAX_BOOST = 3;

    int boost = (int)(t->wait_ticks / (uint32_t)AGING_QUANTUM);
    if (boost > MAX_BOOST) boost = MAX_BOOST;
    return p + boost;
}

static const char *state_str(thread_state_t s) {
    switch (s) {
        case T_UNUSED:   return "UNUSED";
        case T_READY:    return "READY";
        case T_RUNNING:  return "RUNNING";
        case T_SLEEPING: return "SLEEPING";
        case T_DEAD:     return "DEAD";
        case T_ZOMBIE:   return "ZOMBIE";
        case T_STOPPED:  return "STOPPED";
        default:         return "?";
    }
}





void thread_dump(void) {
    uint64_t now = pit_get_ticks();
    uint64_t total = (now >= g_boot_tick) ? (now - g_boot_tick) : 0;

    kprintf("\n[threads]\n");
    for (int i = 0; i < THREAD_MAX; i++) {
        if (g_threads[i].state == T_UNUSED) continue;

        uint64_t ct = thread_effective_cpu_ticks(&g_threads[i]);
        int cpu = (int)ct;  // PIT ticks are small; keep it simple for our kprintf
        int pct = 0;
        if (total != 0) pct = (int)((ct * 100) / total);

        kprintf("slot=%d tid=%d state=%s name=%s prio=%d cpu=%d pct=%d\n",
            i,
            (int)g_threads[i].tid,
            state_str(g_threads[i].state),
            g_threads[i].name,
            (int)g_threads[i].prio,
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


int thread_aging_get(void) {
    return g_aging_enabled ? 1 : 0;
}

void thread_aging_set(int on) {
    g_aging_enabled = on ? 1 : 0;
}

int thread_timeslice_get(void) {
    return g_timeslice_ticks;
}

void thread_timeslice_set(int ticks) {
    if (ticks < 1) ticks = 1;
    if (ticks > 50) ticks = 50; // keep sane
    g_timeslice_ticks = ticks;
    if (g_slice_left > g_timeslice_ticks) g_slice_left = g_timeslice_ticks;
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

    kprintf("\n[top] total_ticks=%d\n", (int)total);
    for (int k = 0; k < n; k++) {
        int i = idx[k];
        uint64_t ct = thread_effective_cpu_ticks(&g_threads[i]);
        int cpu = (int)ct;
        int pct = 0;
        if (total != 0) pct = (int)((ct * 100) / total);

        kprintf("slot=%d tid=%d state=%s name=%s prio=%d cpu=%d pct=%d\n",
            i,
            (int)g_threads[i].tid,
            state_str(g_threads[i].state),
            g_threads[i].name,
            (int)g_threads[i].prio,
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



static int thread_find_by_name(const char *name) {
    for (int i = 0; i < THREAD_MAX; i++) {
        if (g_threads[i].state == T_UNUSED) continue;
        if (g_threads[i].name[0] == 0) continue;
        // exact match
        const char *a = g_threads[i].name;
        const char *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return i;
    }
    return -1;
}

int thread_stop_talk(void) {
    int slot = thread_find_by_name("talk");
    if (slot < 0) return -1;
    return thread_kill(slot);
}

static void talk_fn(void *arg) {
    uint64_t packed = (uint64_t)(uintptr_t)arg;
    uint32_t ms = (uint32_t)(packed & 0xffffffffu);
    uint32_t count = (uint32_t)((packed >> 32) & 0xffffffffu);

    if (ms < 10) ms = 250;

    // count==0 means "run forever" (backwards compatible)
    if (count == 0) {
        int i = 0;
        for (;;) {
            kprintf("\n[talk] i=%d\n", i++);
            thread_sleep_ms(ms);
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        kprintf("\n[talk] i=%d\n", (int)i);
        thread_sleep_ms(ms);
    }

    kprintf("\n[talk] done\n");
    thread_exit();
}



int thread_spawn_talk(uint64_t period_ms, uint32_t count) {
    // If a previous talk exists, stop it first
    thread_stop_talk();
    uint64_t packed = ((uint64_t)count << 32) | ((uint64_t)(uint32_t)period_ms);

    int id = thread_create("talk", talk_fn, (void*)(uintptr_t)packed);
    if (id >= 0) {
        kprintf("\n[thread] spawned talk as id=%d\n", id);
    } else {
        kprintf("\n[thread] spawn talk FAILED\n");
    }
    return id;
}





int thread_kill(int slot) {
    if (slot <= 0 || slot >= THREAD_MAX) return -1; // don't kill main (slot 0)
    if (g_threads[slot].state == T_UNUSED) return -1;
    if (g_threads[slot].state == T_DEAD) return 0;

    k_cli();
    g_threads[slot].state = T_DEAD;

    // If we're killing the currently running thread, switch away now.
    if (&g_threads[slot] == g_cur) {
        k_sti();
        thread_yield();
        return 0;
    }

    // Ask scheduler to run soon so the dead thread won't run again.
    g_need_resched = 1;
    k_sti();
    return 0;
}




void thread_reap_dead(void) {
    for (int i = 1; i < THREAD_MAX; i++) {
        if (g_threads[i].state != T_DEAD) continue;
        if (&g_threads[i] == g_cur) continue;

        /* Destroy per-process page map if thread was killed before exiting */
        if (g_threads[i].cr3) {
            uint64_t dying_cr3 = g_threads[i].cr3;
            g_threads[i].cr3 = 0;
            if (dying_cr3 != vmm_get_kernel_cr3()) {
                vmm_destroy_user_pagemap(dying_cr3);
            }
            thread_user_maps_zero(&g_threads[i]);
        } else {
            thread_user_cleanup(&g_threads[i]);
        }

        if (g_threads[i].stack_base) {
            (void)vmm_unmap_range_and_free(
                (uint64_t)(uintptr_t)g_threads[i].stack_base,
                (size_t)THREAD_STACK_SIZE
            );
        }

        g_threads[i].state = T_UNUSED;
        g_threads[i].rsp = 0;
        g_threads[i].kstack_top = 0;
        g_threads[i].tid = 0;
        g_threads[i].prio = 0;
        g_threads[i].wait_ticks = 0;
        g_threads[i].wake_tick = 0;
        g_threads[i].cpu_ticks = 0;
        g_threads[i].last_start_tick = 0;
        g_threads[i].entry = 0;
        g_threads[i].arg = 0;
        g_threads[i].stack_base = 0;
        g_threads[i].is_user = 0;
        g_threads[i].cr3 = 0;
        g_threads[i].name[0] = 0;
        g_threads[i].parent_tid = 0;
        g_threads[i].exit_code = 0;
        thread_user_maps_zero(&g_threads[i]);
    }
}







void thread_jobs(void) {
    kprintf("jobs:\n");

    for (int i = 0; i < THREAD_MAX; i++) {
        thread_t *t = &g_threads[i];

        const char *st = "?";
        switch (t->state) {
            case T_UNUSED:   st = "UNUSED";  break;
            case T_READY:    st = "READY";   break;
            case T_RUNNING:  st = "RUN";     break;
            case T_SLEEPING: st = "SLEEP";   break;
            case T_DEAD:     st = "DEAD";    break;
            case T_ZOMBIE:   st = "ZOMBIE";  break;
            case T_STOPPED:  st = "STOPPED"; break;
        }

        const char *cur = (t == g_cur) ? "*" : " ";

        // Print ALL slots so you always see at least slot 0.
        kprintf(" [%p]%s tid=%p st=%s pr=%p name=%s rsp=%p ktop=%p sb=%p",
                (void*)(uint64_t)i,
                cur,
                (void*)(uint64_t)t->tid,
                st,
                (void*)(uint64_t)t->prio,
                t->name[0] ? t->name : "(none)",
                (void*)t->rsp,
                (void*)t->kstack_top,
                (void*)t->stack_base);

        if (t->state == T_SLEEPING) {
            kprintf(" wake=%p", (void*)t->wake_tick);
        }

        kprintf("\n");
    }
}


/* ===== demo thread so we can prove switching works ===== */

static void demo_fn(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        kprintf("\n[demo] i=%d\n", i);
        /* cheap delay so it doesn't spam instantly */
        thread_sleep_ms(250);
    }
    kprintf("\n[demo] done\n");
}

int thread_spawn_demo(void) {
    int id = thread_create("demo", demo_fn, 0);
    if (id >= 0) {
        kprintf("\n[thread] spawned demo slot=%d\n", id);
    } else {
        kprintf("\n[thread] spawn demo FAILED\n");
    }
    return id;
}



static void demo_bg_fn(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        thread_sleep_ms(250);
    }
}

int thread_spawn_demo_bg(void) {
    int id = thread_create("demo.bg", demo_bg_fn, 0);
    if (id >= 0) {
        kprintf("\n[thread] spawned background demo slot=%d\n", id);
    } else {
        kprintf("\n[thread] spawn background demo FAILED\n");
    }
    return id;
}

// ---- ps: show current thread list (simple view) ----
static const char* thread_state_name(thread_state_t s) {
    switch (s) {
        case T_UNUSED:   return "UNUSED";
        case T_READY:    return "READY";
        case T_RUNNING:  return "RUNNING";
        case T_SLEEPING: return "SLEEP";
        case T_DEAD:     return "DEAD";
        case T_ZOMBIE:   return "ZOMBIE";
        case T_STOPPED:  return "STOPPED";
        default:         return "?";
    }
}


void thread_mark_user_slot(int slot, int on) {
    if (slot < 0 || slot >= THREAD_MAX) return;
    g_threads[slot].is_user = on ? 1 : 0;
}

int thread_user_map_add(uint64_t va, uint64_t size) {
    if (!g_cur || size == 0) return -1;

    for (int i = 0; i < THREAD_USER_MAP_MAX; i++) {
        if (!g_cur->user_maps[i].in_use) continue;
        if (g_cur->user_maps[i].base == va && g_cur->user_maps[i].size == size) {
            g_cur->is_user = 1;
            return 0;
        }
    }

    for (int i = 0; i < THREAD_USER_MAP_MAX; i++) {
        if (g_cur->user_maps[i].in_use) continue;
        g_cur->user_maps[i].base = va;
        g_cur->user_maps[i].size = size;
        g_cur->user_maps[i].in_use = 1;
        g_cur->is_user = 1;

        return 0;
    }
    return -1;
}


void thread_user_map_cleanup_current(void) {
    thread_user_cleanup(g_cur);
}

void thread_user_map_cleanup_slot(int slot) {
    if (slot < 0 || slot >= THREAD_MAX) return;
    thread_user_cleanup(&g_threads[slot]);
}

int thread_user_any_active(void) {
    for (int i = 0; i < THREAD_MAX; i++) {
        if (!g_threads[i].is_user) continue;
        if (g_threads[i].state == T_UNUSED || g_threads[i].state == T_DEAD || g_threads[i].state == T_ZOMBIE) continue;
        return 1;
    }
    return 0;
}

void thread_ps_dump(void) {
    kprintf("\n--- FiFi ps (threads) ---\n");
    kprintf("SLOT CUR  TID   STATE     CPU_TICKS   PTR                 NAME\n");
    kprintf("-------------------------------------------------------------------\n");

    for (int i = 0; i < THREAD_MAX; i++) {
        thread_t *t = &g_threads[i];
        if (t->state == T_UNUSED) continue;

        char cur = (t == g_cur) ? '*' : ' ';

        const char *st = thread_state_name(t->state);
        const char *nm = (t->name[0] != 0) ? t->name : "(noname)";

        kprintf("%d    %c    %p  %s  %p   %p  %s\n",
                i,
                cur,
                (void*)(uintptr_t)t->tid,
                st,
                (void*)(uintptr_t)t->cpu_ticks,
                (void*)t,
                nm);
    }

    kprintf("\n");
}

int thread_wait_slot(int slot) {
    if (slot < 0 || slot >= THREAD_MAX) return -1;

    for (;;) {
        k_cli();
        thread_state_t st = g_threads[slot].state;
        k_sti();

        if (st == T_UNUSED) {
            return 0;
        }

        if (st == T_DEAD) {
            thread_reap_dead();

            k_cli();
            st = g_threads[slot].state;
            k_sti();

            if (st == T_UNUSED || st == T_DEAD) {
                return 0;
            }
        }

        // Keep the scheduler alive while waiting on a foreground thread.
        thread_check_resched();
        __asm__ __volatile__("hlt");
    }
}


int thread_kill_slot(int slot) {
    if (slot < 0 || slot >= THREAD_MAX) return -1;

    k_cli();

    thread_t *t = &g_threads[slot];

    if (t == g_cur) {
        k_sti();
        return -1;
    }

    if (t->state == T_UNUSED || t->state == T_DEAD) {
        k_sti();
        return -1;
    }

    t->state = T_DEAD;
    t->wake_tick = 0;
    t->wait_ticks = 0;

    k_sti();

    thread_user_cleanup(t);
    thread_reap_dead();
    return 0;
}


uint64_t thread_get_brk(void) {
    if (!g_cur) return 0;
    return g_cur->user_brk;
}

void thread_set_brk(uint64_t brk) {
    if (!g_cur) return;
    g_cur->user_brk = brk;
}

/* Working directory accessors */
const char *thread_get_cwd(void) {
    return (g_cur && g_cur->cwd[0]) ? g_cur->cwd : "/";
}

void thread_set_cwd(const char *path) {
    if (!g_cur || !path) return;
    size_t i = 0;
    while (path[i] && i < sizeof(g_cur->cwd) - 1) { g_cur->cwd[i] = path[i]; i++; }
    g_cur->cwd[i] = '\0';
}

void thread_copy_cwd_to_slot(int slot, const char *cwd) {
    if (slot < 0 || slot >= THREAD_MAX || !cwd) return;
    size_t i = 0;
    while (cwd[i] && i < sizeof(g_threads[slot].cwd) - 1) {
        g_threads[slot].cwd[i] = cwd[i]; i++;
    }
    g_threads[slot].cwd[i] = '\0';
}

/* Return the TID assigned to a given thread slot (0 if invalid/unused). */
uint32_t thread_tid_of_slot(int slot) {
    if (slot < 0 || slot >= THREAD_MAX) return 0;
    return g_threads[slot].tid;
}

/* Store exit code in current thread before calling thread_exit. */
void thread_set_exit_code(int code) {
    if (g_cur) g_cur->exit_code = code;
}

/* Set parent TID for a freshly-created thread slot (called by do_fork). */
void thread_set_parent_for_slot(int slot, uint32_t ptid) {
    if (slot < 0 || slot >= THREAD_MAX) return;
    g_threads[slot].parent_tid = ptid;
}

/*
 * Search for a zombie child whose parent_tid == parent_tid.
 * child_tid == (uint32_t)-1 matches any child.
 *
 * Returns:
 *   > 0  : reaped child's TID (zombie → T_DEAD so reaper can collect)
 *   -1   : matching child exists but is still running
 *   -2   : no matching child found at all
 */
long thread_reap_zombie_child(uint32_t par_tid, uint32_t child_tid, int *code_out) {
    int found_alive = 0;
    for (int i = 0; i < THREAD_MAX; i++) {
        if (g_threads[i].parent_tid != par_tid) continue;
        if (child_tid != (uint32_t)-1 && g_threads[i].tid != child_tid) continue;
        if (g_threads[i].state == T_ZOMBIE) {
            uint32_t reaped = g_threads[i].tid;
            if (code_out) *code_out = g_threads[i].exit_code;
            g_threads[i].state = T_DEAD; /* let thread_reap_dead() free the slot */
            return (long)reaped;
        }
        if (g_threads[i].state != T_UNUSED) {
            found_alive = 1;
        }
    }
    return found_alive ? -1L : -2L;
}

/* ── signal delivery ─────────────────────────────────────────────────────── */

/*
 * Send SIGINT to every user thread that was created via fork (parent_tid != 0).
 * Sleeping threads are woken immediately so they reach a signal check quickly.
 * Called from the keyboard IRQ handler on Ctrl-C.
 */
void thread_signal_children(void) {
    for (int i = 0; i < THREAD_MAX; i++) {
        thread_t *t = &g_threads[i];
        if (t->state == T_UNUSED || t->state == T_DEAD || t->state == T_ZOMBIE)
            continue;
        if (!t->is_user) continue;
        if (t->parent_tid == 0) continue;   /* skip ush — no fork parent */
        t->sig_pending = 2; /* SIGINT */
        if (t->state == T_SLEEPING) t->state = T_READY;  /* wake to die */
    }
}

void thread_sigtstp_children(void) {
    for (int i = 0; i < THREAD_MAX; i++) {
        thread_t *t = &g_threads[i];
        if (t->state == T_UNUSED || t->state == T_DEAD || t->state == T_ZOMBIE)
            continue;
        if (!t->is_user) continue;
        if (t->parent_tid == 0) continue;
        if (t->sig_pending == 0) t->sig_pending = 19; /* SIGTSTP */
        if (t->state == T_SLEEPING) t->state = T_READY;
    }
}

/*
 * Check pending signal in blocking kernel loops.
 * SIGINT/SIGTERM → exit; SIGTSTP → stop; others → exit with 128+sig.
 */
void thread_check_signal(void) {
    if (!g_cur || !g_cur->sig_pending) return;
    int sig = g_cur->sig_pending;
    g_cur->sig_pending = 0;
    if (sig == 19 /* SIGTSTP */) {
        thread_do_stop();
        return;
    }
    g_cur->exit_code = 128 + sig;
    thread_exit();
}

/* ── New signal/pgid/mmap functions ─────────────────────────────────────── */

int thread_take_pending_sig(void) {
    if (!g_cur) return 0;
    int sig = g_cur->sig_pending;
    g_cur->sig_pending = 0;
    return sig;
}

uint64_t thread_get_sig_handler(int sig) {
    if (!g_cur || sig < 0 || sig >= 32) return 0;
    return g_cur->sig_handlers[sig];
}

void thread_set_sig_handler(int sig, uint64_t handler_va) {
    if (!g_cur || sig < 0 || sig >= 32) return;
    g_cur->sig_handlers[sig] = handler_va;
}

void thread_kill_by_tid(uint32_t tid, int sig) {
    for (int i = 0; i < THREAD_MAX; i++) {
        thread_t *t = &g_threads[i];
        if (t->tid != tid) continue;
        if (t->state == T_UNUSED || t->state == T_DEAD) continue;
        t->sig_pending = sig;
        if (t->state == T_SLEEPING) t->state = T_READY;
        if (sig == 18 /* SIGCONT */ && t->state == T_STOPPED) {
            t->state = T_READY;
            t->stop_reported = 0;
        }
        return;
    }
}

void thread_do_stop(void) {
    if (!g_cur) return;
    k_cli();
    g_cur->state = T_STOPPED;
    k_sti();
    thread_yield();
    /* execution resumes here after SIGCONT */
}

void thread_cont_by_tid(uint32_t tid) {
    for (int i = 0; i < THREAD_MAX; i++) {
        thread_t *t = &g_threads[i];
        if (t->tid != tid) continue;
        if (t->state == T_STOPPED) {
            t->state = T_READY;
            t->stop_reported = 0;
        }
        /* also clear any pending SIGTSTP */
        if (t->sig_pending == 19) t->sig_pending = 0;
        return;
    }
}

uint32_t thread_get_pgid(uint32_t tid) {
    for (int i = 0; i < THREAD_MAX; i++) {
        if (g_threads[i].tid == tid &&
            g_threads[i].state != T_UNUSED) {
            uint32_t pg = g_threads[i].pgid;
            return pg ? pg : tid;  /* default pgid = tid */
        }
    }
    return 0;
}

void thread_set_pgid(uint32_t tid, uint32_t pgid) {
    for (int i = 0; i < THREAD_MAX; i++) {
        if (g_threads[i].tid == tid &&
            g_threads[i].state != T_UNUSED) {
            g_threads[i].pgid = pgid;
            return;
        }
    }
}

void thread_copy_pgid_to_slot(int slot, uint32_t pgid) {
    if (slot < 0 || slot >= THREAD_MAX) return;
    g_threads[slot].pgid = pgid;
}

uint64_t thread_get_mmap_next(void) {
    return g_cur ? g_cur->mmap_next : 0x0000600000000000ULL;
}

void thread_set_mmap_next(uint64_t addr) {
    if (g_cur) g_cur->mmap_next = addr;
}

long thread_check_stopped_child(uint32_t par_tid, uint32_t child_tid, int *code_out) {
    for (int i = 0; i < THREAD_MAX; i++) {
        thread_t *t = &g_threads[i];
        if (t->parent_tid != par_tid) continue;
        if (child_tid != (uint32_t)-1 && t->tid != child_tid) continue;
        if (t->state == T_STOPPED && !t->stop_reported) {
            t->stop_reported = 1;
            if (code_out) *code_out = (19 << 8) | 0x7F; /* WIFSTOPPED(19) */
            return (long)t->tid;
        }
    }
    return 0;
}
