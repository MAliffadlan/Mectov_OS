#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "types.h"

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT {0}

static inline void spin_lock(spinlock_t* lock) {
    // xchg swaps the contents of eax (1) and lock->locked atomically.
    // if the old value was 1, it means someone else had the lock, so we loop.
    // if the old value was 0, it means it was free and we just set it to 1.
    uint32_t val = 1;
    while (1) {
        __asm__ __volatile__("xchg %0, %1"
                             : "=r"(val), "+m"(lock->locked)
                             : "0"(val)
                             : "memory");
        if (val == 0) break;
        __asm__ __volatile__("pause");
    }
}

// Non-blocking acquire: returns 1 if the lock was taken, 0 if it was already
// held (by any CPU). Used by the exception path, where spinning on a lock
// held by the interrupted (pre-exception) context would deadlock the system.
static inline int spin_try_lock(spinlock_t* lock) {
    uint32_t val = 1;
    __asm__ __volatile__("xchg %0, %1"
                         : "=r"(val), "+m"(lock->locked)
                         : "0"(val)
                         : "memory");
    return val == 0;
}

static inline void spin_unlock(spinlock_t* lock) {
    // Memory barrier to ensure all previous writes are visible before unlocking
    __asm__ __volatile__("": : :"memory");
    lock->locked = 0;
}

// ---- irqsave helpers (process context) ----
//
// Rule: process context (syscalls, main loop, shell) takes a shared lock with
// spin_lock_irqsave() and releases with spin_unlock_irqrestore(). IRQ and
// exception context (IF already 0 on x86) uses the plain spin_lock()/unlock().
// A holder always has IF=0, so a timer IRQ can never re-enter the same lock on
// this core (no self-deadlock), and a cross-CPU holder is never preemptible.
//
// eflags is saved/restored in full (pushfl/popfl), so nested acquisition
// inside an existing cli section preserves the caller's IF state — the same
// pattern the kernel previously hand-rolled at every call site. The restore
// clobbers "cc" so the compiler does not rely on condition codes across it.
static inline uint32_t spin_lock_irqsave(spinlock_t* lock) {
    uint32_t eflags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=g"(eflags) : : "memory");
    spin_lock(lock);
    return eflags;
}

static inline void spin_unlock_irqrestore(spinlock_t* lock, uint32_t eflags) {
    spin_unlock(lock);
    __asm__ __volatile__("pushl %0; popfl" : : "g"(eflags) : "memory", "cc");
}

#endif
