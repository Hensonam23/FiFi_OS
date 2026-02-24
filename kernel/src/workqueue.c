#include <stdint.h>
#include <stdbool.h>
#include "workqueue.h"

#define WQ_MAX 64

typedef struct {
    work_fn_t fn;
    void *arg;
} wq_item_t;

static volatile uint32_t wq_head = 0;
static volatile uint32_t wq_tail = 0;
static wq_item_t wq[WQ_MAX];

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    if (flags & (1ULL << 9)) {
        __asm__ __volatile__("sti" : : : "memory");
    }
}

int workqueue_push(work_fn_t fn, void *arg) {
    if (!fn) return -1;

    uint64_t f = irq_save();

    uint32_t next = (wq_head + 1) % WQ_MAX;
    if (next == wq_tail) {
        irq_restore(f);
        return -1; // full
    }

    wq[wq_head].fn = fn;
    wq[wq_head].arg = arg;
    wq_head = next;

    irq_restore(f);
    return 0;
}

void workqueue_run(void) {
    for (;;) {
        work_fn_t fn = 0;
        void *arg = 0;

        uint64_t f = irq_save();
        if (wq_tail == wq_head) {
            irq_restore(f);
            return; // empty
        }

        fn = wq[wq_tail].fn;
        arg = wq[wq_tail].arg;
        wq_tail = (wq_tail + 1) % WQ_MAX;
        irq_restore(f);

        // run with interrupts enabled
        fn(arg);
    }
}
