/* M7 spinlock: test-and-set via xchg + pause, irqsave/restore pairing.
 *
 * Rules (system-wide):
 * - cli around every critical section: an IRQ/IPI handler taking the same
 *   lock would self-deadlock otherwise. Unlock restores the saved IF.
 * - Global order: sched_lock -> mem_lock, never reverse.
 * - IPI handlers take NO locks (BSP waits are bounded; APs always make
 *   progress with IF=1 outside critical sections, so no wait can wedge).
 */
#ifndef SPIN64_H
#define SPIN64_H

#include "cpu64.h"

typedef struct {
    volatile int locked;
} spin64_t;

#define SPIN64_INIT { 0 }

static inline u64 spin64_lock_irqsave(spin64_t *l) {
    u64 flags;
    __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    while (__sync_lock_test_and_set(&l->locked, 1)) {
        __asm__ __volatile__("pause");
    }
    return flags;
}

static inline void spin64_unlock_irqrestore(spin64_t *l, u64 flags) {
    __asm__ __volatile__("" ::: "memory");
    __sync_lock_release(&l->locked);
    __asm__ __volatile__("push %0; popfq" :: "r"(flags) : "memory", "cc");
}

#endif
