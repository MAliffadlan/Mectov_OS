#ifndef SYNC_H
#define SYNC_H

#include "types.h"

// ---- Kernel semaphores (System V style, id-based) ----
int sem_create(int initial);
int sem_wait(int id);          // blocks current task if count == 0
int sem_post(int id);          // increments count / wakes one waiter
int sem_destroy(int id);

// ---- Kernel futexes (keyed by address-space + address) ----
// futex_wait blocks the current task while *addr == expected (must be called
// from the task's own address space). Returns 0 if it slept, -1 if the value
// already differed (EAGAIN-like, caller should retry the check).
int futex_wait(uint32_t addr, uint32_t expected);
// Wakes up to `max_waiters` tasks blocked on addr in this address space.
int futex_wake(uint32_t addr, int max_waiters);

void sync_init(void);

// v38.55: remove a dying task from every sem/futex waiter list. Teardown
// calls the _defer variant (safe under task_lock); the BSP main loop calls
// sync_drain_pending() each iteration to do the actual removal.
void sync_task_cleanup(int tid);
void sync_task_cleanup_defer(int tid);
void sync_drain_pending(void);

#endif
