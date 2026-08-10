// src/sys/klog.c — kernel message ring buffer (the `dmesg` source).
//
// Every byte the kernel writes to the serial port goes through serial.c's
// locked putc helpers; those call klog_putc() so the same text is captured
// in memory. A user (or the shell) reads it back via /proc/dmesg, which
// calls klog_snapshot(). The buffer wraps: only the newest KLOG_SIZE bytes
// survive, which is exactly what a dmesg tail wants.
//
// Locking: klog_putc is called (a) from serial.c while holding serial_lock
// (normal path) and (b) from write_serial_try on the exception path with
// interrupts disabled. It deliberately takes NO lock: the exception path
// must be able to log even when the pre-exception context is mid-
// klog_snapshot (which holds klog_lock) — the same deadlock write_serial_try
// avoids by try-locking serial_lock. Writers are serialized by serial_lock
// (normal path) or are already interrupt-disabled (exception path), and a
// single aligned uint32 store is atomic on x86, so the worst case of a rare
// concurrent exception is a lost byte — never corruption or a hang. Readers
// (klog_snapshot) disable interrupts and take klog_lock, and copy under it.

#include "../include/klog.h"
#include "../include/spinlock.h"

static char klog_buf[KLOG_SIZE];
// head = index of the oldest byte; len = number of valid bytes (0..KLOG_SIZE).
static volatile uint32_t klog_head = 0;
static volatile uint32_t klog_len = 0;
static spinlock_t klog_lock = SPINLOCK_INIT;

void klog_putc(char c) {
    // Called with interrupts disabled (serial_lock held or exception path).
    // Single-writer per call site; atomic 32-bit updates make head/len safe
    // against a concurrent reader that took klog_lock (it sees either the
    // before or after state, never a torn one).
    uint32_t len = klog_len;
    if (len < KLOG_SIZE) {
        klog_buf[(klog_head + len) % KLOG_SIZE] = c;
        klog_len = len + 1;
    } else {
        // Buffer full: overwrite the oldest byte and advance head.
        klog_buf[klog_head] = c;
        klog_head = (klog_head + 1) % KLOG_SIZE;
    }
}

int klog_snapshot(char* dst, int max) {
    if (!dst || max <= 0) return 0;
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&klog_lock);

    uint32_t len = klog_len;
    // Tail semantics: when the ring has wrapped (len == KLOG_SIZE) or the
    // caller's buffer is smaller than the log, start at the newest `max`
    // bytes so a small read always shows the most recent activity.
    uint32_t start = 0;
    if (len > (uint32_t)max) start = (klog_head + len - (uint32_t)max) % KLOG_SIZE;
    else start = klog_head;
    int n = (int)(len < (uint32_t)max ? len : (uint32_t)max);
    for (int i = 0; i < n; i++) {
        dst[i] = klog_buf[(start + (uint32_t)i) % KLOG_SIZE];
    }
    if (n < max) dst[n] = '\0';

    spin_unlock(&klog_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
    return n;
}

int klog_bytes(void) {
    return (int)klog_len;
}
