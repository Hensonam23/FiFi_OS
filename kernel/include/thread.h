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

/* Set the current thread's page map CR3 (used by run task before switching) */
void g_cur_set_cr3(uint64_t cr3_phys);

/* Read current thread's page map CR3 (0 if no user map) */
uint64_t g_cur_cr3(void);

/* Zero the current thread's user map tracking array (without unmapping) */
void thread_user_maps_zero_current(void);

/* User-space program break (heap top) for the current thread */
uint64_t thread_get_brk(void);
void     thread_set_brk(uint64_t brk);

/* Exit status / waitpid support */
void thread_set_exit_code(int code);
void     thread_set_parent_for_slot(int slot, uint32_t ptid);
uint32_t thread_get_parent_tid(void); /* parent_tid of current thread, 0 if none */
long thread_reap_zombie_child(uint32_t parent_tid, uint32_t child_tid, int *code_out);
uint32_t thread_tid_of_slot(int slot);

/* Working directory */
const char *thread_get_cwd(void);
void        thread_set_cwd(const char *path);
void        thread_copy_cwd_to_slot(int slot, const char *cwd);

/* Signal delivery */
/* Send SIGINT to all user threads that were forked (parent_tid != 0).
 * Sleeping threads are woken so they can exit promptly. */
void thread_signal_children(void);
/* Send SIGTSTP to all user children (Ctrl-Z). */
void thread_sigtstp_children(void);
/* If the current thread has a pending signal, act on it.
 * Fatal signals exit with 128+sig; SIGTSTP stops the thread. */
void thread_check_signal(void);

/* Signal handler table for current thread. handler=0:SIG_DFL, 1:SIG_IGN, else user VA. */
uint64_t thread_get_sig_handler(int sig);
void     thread_set_sig_handler(int sig, uint64_t handler_va);
/* Atomically take and clear sig_pending; returns signal number or 0. */
int      thread_take_pending_sig(void);
/* Send signal to thread by TID. */
void     thread_kill_by_tid(uint32_t tid, int sig);
/* Stop current thread (SIGTSTP default action); resumes on SIGCONT. */
void     thread_do_stop(void);
/* Resume a stopped thread. */
void     thread_cont_by_tid(uint32_t tid);

/* Process group support */
uint32_t thread_get_pgid(uint32_t tid);       /* 0 = not found */
void     thread_set_pgid(uint32_t tid, uint32_t pgid);
/* Copy pgid from current thread to a named slot. */
void     thread_copy_pgid_to_slot(int slot, uint32_t pgid);

/* mmap watermark per thread */
uint64_t thread_get_mmap_next(void);
void     thread_set_mmap_next(uint64_t addr);

/* Copy all fork-inheritable state from current thread to child slot.
 * Must be called after thread_create() and before the child runs. */
void thread_fork_inherit_slot(int child_slot);

/* Check if any child of par_tid is stopped (not zombie).
 * Returns child TID and sets *code_out to WSTOPPED status, or 0. */
long     thread_check_stopped_child(uint32_t par_tid, uint32_t child_tid, int *code_out);
