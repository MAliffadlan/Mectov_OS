#ifndef MCT_SYNC_H
#define MCT_SYNC_H

#include "../../src/include/syscall.h"

// ============================================================
// Mectov OS pthread-style synchronization (v38.27)
// ============================================================
// Futex-based mutex + condition variable, built on SYS_FUTEX_WAIT/WAKE.
// The kernel parks a task (TASK_STATE_BLOCKED) while it sleeps, so a
// blocked thread burns no CPU and the scheduler runs other threads —
// including on other cores (the futex table is spinlock-protected and
// keyed by address space + address, so it is SMP-safe).
//
//   mct_mutex_t  — non-recursive mutual exclusion lock.
//                  fast path: one `lock cmpxchgl`; slow path parks on the
//                  futex only while the lock is contended.
//   mct_cond_t   — condition variable (wait/signal/broadcast). Uses a
//                  sequence counter: waiters snapshot the counter BEFORE
//                  releasing the mutex, so a signal that races between the
//                  snapshot and the sleep is never lost — the kernel's
//                  futex_wait re-checks the value under its lock and
//                  returns -1 immediately, and the caller re-checks the
//                  predicate in the standard `while (pred) wait();` loop.
// ============================================================

// ---- atomic compare-and-swap (i386 userland, SMP-safe) ----
static inline int mct_cmpxchg(volatile int* p, int old, int new) {
    int prev = old;
    __asm__ volatile("lock cmpxchgl %2, %0"
                     : "+m"(*p), "+a"(prev)
                     : "r"(new)
                     : "cc", "memory");
    return prev;  // old value of *p: == old means we won
}

// ============================================================
// Mutex
// ============================================================
typedef struct {
    volatile int lock;  // 0 = unlocked, 1 = locked (owner)
} mct_mutex_t;

#define MCT_MUTEX_INITIALIZER { 0 }

static inline void mct_mutex_init(mct_mutex_t* m) { m->lock = 0; }

static inline void mct_mutex_lock(mct_mutex_t* m) {
    // Fast path: uncontended acquire.
    if (mct_cmpxchg(&m->lock, 0, 1) == 0) return;
    // Slow path: contended. Park on the futex while the lock is held.
    // futex_wait sleeps only while *addr == expected (the kernel re-checks
    // under its own lock, so a wake racing between our check and the sleep
    // is not lost — it returns -1 and we simply retry).
    for (;;) {
        while (m->lock == 1) {
            sys_futex_wait((void*)&m->lock, 1);
        }
        if (mct_cmpxchg(&m->lock, 0, 1) == 0) return;
    }
}

static inline void mct_mutex_unlock(mct_mutex_t* m) {
    // Release-store then wake one waiter (if any). The syscall acts as a
    // barrier; on x86 the plain store is already visible before it.
    m->lock = 0;
    sys_futex_wake((void*)&m->lock, 1);
}

// ============================================================
// Condition variable (sequence-counter style, no lost wakeups)
// ============================================================
typedef struct {
    volatile int seq;  // bumped on every signal/broadcast
} mct_cond_t;

#define MCT_COND_INITIALIZER { 0 }

static inline void mct_cond_init(mct_cond_t* c) { c->seq = 0; }

// Atomically release the mutex and sleep until signaled/broadcast, then
// reacquire the mutex before returning. The caller MUST re-check its
// predicate in a while loop (standard POSIX contract).
static inline void mct_cond_wait(mct_cond_t* c, mct_mutex_t* m) {
    int s = c->seq;  // snapshot BEFORE releasing the mutex
    mct_mutex_unlock(m);
    // If a signal raced between the snapshot and here, seq already changed:
    // futex_wait sees it and returns -1 without sleeping — no lost wakeup.
    sys_futex_wait((void*)&c->seq, s);
    mct_mutex_lock(m);
}

static inline void mct_cond_signal(mct_cond_t* c) {
    c->seq++;
    sys_futex_wake((void*)&c->seq, 1);
}

static inline void mct_cond_broadcast(mct_cond_t* c) {
    c->seq++;
    sys_futex_wake((void*)&c->seq, 0x7FFFFFFF);
}

#endif
