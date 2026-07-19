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

static inline void spin_unlock(spinlock_t* lock) {
    // Memory barrier to ensure all previous writes are visible before unlocking
    __asm__ __volatile__("": : :"memory");
    lock->locked = 0;
}

#endif
