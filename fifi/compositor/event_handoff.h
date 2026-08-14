#ifndef FIFI_EVENT_HANDOFF_H
#define FIFI_EVENT_HANDOFF_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>

typedef struct {
    atomic_bool waiting;
} fifi_event_handoff_t;

#define FIFI_EVENT_HANDOFF_INIT { ATOMIC_VAR_INIT(false) }

static inline void fifi_event_handoff_request(fifi_event_handoff_t *handoff)
{
    atomic_store_explicit(&handoff->waiting, true, memory_order_release);
}

static inline void fifi_event_handoff_acquired(fifi_event_handoff_t *handoff)
{
    atomic_store_explicit(&handoff->waiting, false, memory_order_release);
}

static inline bool fifi_event_handoff_is_waiting(fifi_event_handoff_t *handoff)
{
    return atomic_load_explicit(&handoff->waiting, memory_order_acquire);
}

/* Called with mx held.  If the event thread is blocked on the same mutex,
 * release it until that thread has acquired the mutex and acknowledged the
 * handoff, then reacquire it before returning. */
static inline void fifi_event_handoff_yield(fifi_event_handoff_t *handoff,
                                            pthread_mutex_t *mx)
{
    if (!fifi_event_handoff_is_waiting(handoff)) return;

    const struct timespec pause = { .tv_sec = 0, .tv_nsec = 100000 };
    pthread_mutex_unlock(mx);
    do {
        nanosleep(&pause, NULL);
    } while (fifi_event_handoff_is_waiting(handoff));
    pthread_mutex_lock(mx);
}

#endif
