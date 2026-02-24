#pragma once
#include <stdint.h>

typedef void (*thread_entry_t)(void *arg);

void thread_init(void);
int  thread_create(const char *name, thread_entry_t entry, void *arg);
void thread_yield(void);
__attribute__((noreturn)) void thread_exit(void);

void thread_dump(void);

/* temporary demo helper so we can test quickly from the shell */
int thread_spawn_demo(void);
void thread_request_resched(void);
void thread_check_resched(void);

int  thread_preempt_get(void);
void thread_preempt_set(int on);

void thread_sleep_ms(uint64_t ms);
