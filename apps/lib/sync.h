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
//
// Ordering (v38.89): the lock word and the sequence counter are plain
// `int`, so WITHOUT compiler barriers `-O2` is free to reorder them
// against the critical section — and did: conddemo.elf once showed
// unlock's `lock=0` store + wake hoisted ABOVE `g_counter++`, silently
// dropping mutual exclusion under contention (flaky short counter).
// Every barrier below is `asm("" ::: "memory")`: zero instructions, but
// the compiler may not move plain accesses across it. On x86 (TSO)
// compilation order is visibility order, so this is sufficient — no
// hardware fence needed.
// ============================================================

// Compiler barrier: nothing (plain or volatile) moves across it.
#define MCT_BARRIER() __asm__ __volatile__("" ::: "memory")

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
    if (mct_cmpxchg(&m->lock, 0, 1) == 0) { MCT_BARRIER(); return; }
    // Slow path: contended. Park on the futex while the lock is held.
    // v38.87: the park is BOUNDED (50 ms) — even if a wake is lost to a
    // kernel race, the timed wait returns and the loop re-checks m->lock,
    // so the waiter can never sleep past the lock's release.
    for (;;) {
        while (m->lock == 1) {
            sys_futex_wait_timeout((void*)&m->lock, 1, 50);
        }
        // Acquire barrier: the critical section below must not be
        // compiled above the winning cmpxchg.
        if (mct_cmpxchg(&m->lock, 0, 1) == 0) { MCT_BARRIER(); return; }
    }
}

static inline void mct_mutex_unlock(mct_mutex_t* m) {
    // Release barrier FIRST: everything in the critical section above
    // must be compiled before the release-store. Without this, -O2 once
    // hoisted the store (+wake) above the caller's increment, silently
    // unprotecting it under contention (flaky short mutex counter).
    // On x86 the plain store is already visible before the syscall.
    MCT_BARRIER();
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
    // v38.87: the sleep itself is bounded (50 ms) as a second line of
    // defense — a signal always writes seq before waking, so a parker the
    // sweep releases re-checks a moved word (or a false predicate) and
    // retries; it can never sleep on a satisfied condition forever.
    sys_futex_wait_timeout((void*)&c->seq, s, 50);
    mct_mutex_lock(m);
}

static inline void mct_cond_signal(mct_cond_t* c) {
    c->seq++;
    MCT_BARRIER();
    // The wake must be compiled after the seq bump: a waiter snapshots
    // seq to detect signals that race its sleep, so a reordered
    // wake-before-bump is a lost wakeup (healed only by the 50 ms sweep).
    MCT_BARRIER();
    sys_futex_wake((void*)&c->seq, 1);
}

static inline void mct_cond_broadcast(mct_cond_t* c) {
    // Plain wake-all (POSIX broadcast without mutex knowledge). Kept for
    // cold paths — hot paths should use mct_cond_broadcast_requeue below.
    c->seq++;
    MCT_BARRIER();  // same ordering contract as signal (see above)
    sys_futex_wake((void*)&c->seq, 0x7FFFFFFF);
}

// v38.91: herd-free broadcast via FUTEX_WAIT_REQUEUE — EXPERIMENTAL.
// The kernel op itself is exercised by procsysdemo-era tests, but the
// per-futex-entry timeout design (one deadline per futex word, not per
// waiter) interacts badly with requeued waiters: a moved waiter inherits
// the COND word's deadline, so a later timed park on the same cond word
// can expire the entry while the waiter legitimately sits on the mutex
// queue — the wake_one-purge in the sweep then drops it and the unlock's
// wake hands the mutex to nobody. Do NOT use on the hot path yet;
// mct_cond_broadcast + the 50 ms timed park remain the proven protocol.
static inline void mct_cond_broadcast_requeue(mct_cond_t* c, mct_mutex_t* m) {
    (void)c; (void)m;
    sys_futex_wake((void*)&c->seq, 0x7FFFFFFF);
    mct_mutex_unlock(m);
}

#endif
