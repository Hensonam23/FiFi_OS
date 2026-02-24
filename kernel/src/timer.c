#include <stdint.h>
#include <stdbool.h>
#include "timer.h"
#include "pit.h"
#include "workqueue.h"
#include "kprintf.h"

#define TIMER_MAX 32

typedef struct {
    bool active;
    uint64_t due_ticks;
    work_fn_t fn;
    void *arg;
} timer_ev_t;

static timer_ev_t g_timers[TIMER_MAX];
static uint64_t g_hz = 100;

void timer_init(uint64_t pit_hz) {
    if (pit_hz) g_hz = pit_hz;
    for (int i = 0; i < TIMER_MAX; i++) {
        g_timers[i].active = false;
        g_timers[i].due_ticks = 0;
        g_timers[i].fn = 0;
        g_timers[i].arg = 0;
    }
}

uint64_t timer_hz(void) { return g_hz; }
uint64_t timer_ticks(void) { return pit_get_ticks(); }

static uint64_t ms_to_ticks(uint64_t ms) {
    // ceil(ms * hz / 1000)
    uint64_t num = ms * g_hz + 999;
    return num / 1000;
}

int timer_call_in_ms(uint64_t ms, work_fn_t fn, void *arg) {
    if (!fn) return -1;

    uint64_t now = pit_get_ticks();
    uint64_t due = now + ms_to_ticks(ms);

    for (int i = 0; i < TIMER_MAX; i++) {
        if (!g_timers[i].active) {
            g_timers[i].active = true;
            g_timers[i].due_ticks = due;
            g_timers[i].fn = fn;
            g_timers[i].arg = arg;
            return i;
        }
    }
    return -1; // full
}

void timer_poll(void) {
    uint64_t now = pit_get_ticks();

    for (int i = 0; i < TIMER_MAX; i++) {
        if (!g_timers[i].active) continue;
        if (now < g_timers[i].due_ticks) continue;

        // fire once
        work_fn_t fn = g_timers[i].fn;
        void *arg = g_timers[i].arg;

        g_timers[i].active = false;
        g_timers[i].fn = 0;
        g_timers[i].arg = 0;

        workqueue_push(fn, arg);
    }
}

void timer_sleep_ms(uint64_t ms) {
    if (ms == 0) return;

    uint64_t hz = timer_hz();
    if (hz == 0) hz = 100; // fallback

    // Convert ms -> ticks, rounding up
    uint64_t ticks = (ms * hz + 999) / 1000;
    if (ticks == 0) ticks = 1;

    uint64_t start = timer_ticks();
    uint64_t target = start + ticks;

    #ifdef FIFI_TIMER_DEBUG


    kprintf("sleepdbg: ms=%p hz=%p ticks=%p start=%p target=%p
",


            (void*)ms, (void*)hz, (void*)ticks, (void*)start, (void*)target);


    #endif
    while (timer_ticks() < target) {
        timer_poll(); // ok even if IRQ-driven; cheap
        __asm__ __volatile__("hlt");
    }
}
