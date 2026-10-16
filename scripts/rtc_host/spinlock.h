// scripts/rtc_host/spinlock.h — host shim for src/include/spinlock.h (v38.165).
//
// The host build is single-threaded, so the lock cannot be exercised here: it
// serializes PORT ACCESS, and there is no second port writer to race with.
// The lock's own gate is the guest one — four threads on four cores in
// apps/rtc_stress.c, driven by scripts/rtc_test.py, which measured 32k-37k
// torn stamps per 48k samples before the lock and zero after.
//
// The straddle cases in this harness are about the OTHER half of the fix (the
// Reg A bracket and the retry), which is driver logic and therefore testable
// against a scripted chip.
#ifndef RTC_HOST_SPINLOCK_H
#define RTC_HOST_SPINLOCK_H

#include "types.h"

typedef struct { volatile uint32_t locked; } spinlock_t;

#define SPINLOCK_INIT {0}

static inline void spin_lock(spinlock_t* lock) { (void)lock; }
static inline void spin_unlock(spinlock_t* lock) { (void)lock; }
static inline int spin_try_lock(spinlock_t* lock) { (void)lock; return 1; }
static inline uint32_t spin_lock_irqsave(spinlock_t* lock) { (void)lock; return 0; }
static inline void spin_unlock_irqrestore(spinlock_t* lock, uint32_t eflags) {
    (void)lock; (void)eflags;
}

#endif
