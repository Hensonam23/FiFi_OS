#pragma once
#include <stdint.h>

typedef void (*thread_entry_t)(void *arg);

void thread_init(void);
int  thread_create(const char *name, thread_entry_t entry, void *arg);
void thread_yield(void);
__attribute__((noreturn)) void thread_exit(void);
int thread_wait_slot(int slot);
int thread_kill_slot(int slot);
int thread_user_any_active(void);
void thread_mark_user_slot(int slot, int on);
int thread_user_map_add(uint64_t va, uint64_t size);
void thread_user_map_cleanup_current(void);
void thread_user_map_cleanup_slot(int slot);


void thread_dump(void);

/* temporary demo helper so we can test quickly from the shell */
int thread_spawn_demo(void);
int thread_spawn_demo_bg(void);
void thread_request_resched(void);
void thread_check_resched(void);

int  thread_preempt_get(void);
void thread_preempt_set(int on);

void thread_sleep_ms(uint64_t ms);

int thread_resched_pending(void);

void thread_top(void);
void thread_cpu_reset(void);
int  thread_set_prio(int id, int prio);
int  thread_get_prio(int id);
int  thread_spawn_spin(void);
int  thread_aging_get(void);
void thread_aging_set(int on);
int  thread_timeslice_get(void);
void thread_timeslice_set(int ticks);

int thread_spawn_talk(uint64_t period_ms, uint32_t count);
int thread_stop_talk(void);

int thread_kill(int slot);
void thread_reap_dead(void);
void thread_jobs(void);

// Kernel stack top for the currently running thread (for TSS.rsp0)
uint64_t thread_current_kstack_top(void);

// Current thread id (tid)
uint32_t thread_current_tid(void);

// Debug: list threads for shell 'ps'
void thread_ps_dump(void);
