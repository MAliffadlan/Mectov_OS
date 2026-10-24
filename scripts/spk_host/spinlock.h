// scripts/spk_host/spinlock.h — host model of the kernel's IRQ-save lock.
//
// The real driver runs with interrupts enabled in task context and with IF=0
// in the timer IRQ. The host is single-threaded, so a clock-read hook stands in
// for the IRQ: when local IRQs are masked it is deferred until the matching
// irqrestore, just as on one CPU. This is what lets the race case distinguish
// a lock-free driver (the tick runs in the middle of its transaction) from an
// IRQ-safe one (the tick sees the complete transaction afterwards).
//
// This models LOCAL IRQ masking, not a second CPU. The guest boot test runs
// with -smp 4, but it does not deliberately stress concurrent tone calls.
// No claim of an SMP stress test is made by this harness.
#ifndef SPK_HOST_SPINLOCK_H
#define SPK_HOST_SPINLOCK_H

#include "types.h"

typedef struct { volatile uint32_t locked; } spinlock_t;

#define SPINLOCK_INIT {0}

extern int spk_host_irqs_enabled;
extern int spk_host_timer_irq_pending;

void spk_host_fire_timer_irq(void);
void spk_host_irq_restore(uint32_t eflags);

static inline void spin_lock(spinlock_t* lock) {
    while (__sync_lock_test_and_set(&lock->locked, 1)) { }
}

static inline void spin_unlock(spinlock_t* lock) {
    __sync_lock_release(&lock->locked);
}

static inline uint32_t spin_lock_irqsave(spinlock_t* lock) {
    uint32_t eflags = spk_host_irqs_enabled ? 0x200u : 0;
    spk_host_irqs_enabled = 0;       // cli is per-CPU
    spin_lock(lock);
    return eflags;
}

static inline void spin_unlock_irqrestore(spinlock_t* lock, uint32_t eflags) {
    spin_unlock(lock);
    spk_host_irq_restore(eflags);
}

#endif
