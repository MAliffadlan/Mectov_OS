#ifndef KLOG_H
#define KLOG_H

#include "types.h"

// Kernel message ring buffer (the `dmesg` source). Every byte the kernel
// writes to the serial port is also captured here, so anything a developer
// would tail in a serial log is queryable in-guest via /proc/dmesg. The
// buffer wraps; snapshots return the newest KLOG_SIZE bytes.
#define KLOG_SIZE 65536

// Capture one character. Called from serial.c under serial_lock (normal
// path) and from the raw exception path — it must be safe with interrupts
// disabled and must never block, spin, or allocate.
void klog_putc(char c);

// Copy the newest log content into `dst`: the most recent `max` bytes of the
// ring (oldest of those first, NUL-terminated if room). When the buffer has
// fewer than `max` bytes this is the entire log, oldest first — exactly the
// tail semantics a dmesg reader wants. Returns the number of bytes copied.
// Locked against klog_putc writers.
int klog_snapshot(char* dst, int max);

// Number of valid bytes currently in the ring (0..KLOG_SIZE).
int klog_bytes(void);

#endif
