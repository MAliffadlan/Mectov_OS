// scripts/tls_host_shim.h — the only substitution the host TLS driver needs.
//
// apps/lib/tls/tls13.c takes exactly three things from the kernel: SYS_GETRANDOM,
// the RTC (for certificate validity), and the syscall() wrapper itself. On the
// host those come from libc instead, so the engine's `#include
// "src/include/syscall.h"` is rewritten to this file and not one line of the
// engine changes. Everything the engine then does -- the record layer, the key
// schedule, certificate verification -- is the code that ships.
//
// This is a test/development header. It is never compiled into the image.
#ifndef MCT_TLS_HOST_SHIM_H
#define MCT_TLS_HOST_SHIM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/random.h>
#include <time.h>

// Matches src/include/syscall.h purely so the call site in tls13.c reads the
// same as it does in the guest.
#define SYS_GETRANDOM 117

static inline int syscall(int num, int a, int b, int c) {
    (void)num; (void)c;
    return getrandom((void*)(uintptr_t)a, (size_t)b, 0) == (ssize_t)b ? 0 : -1;
}

// Mirrors the kernel's rtc_time_t field names, so the civil-from-days conversion
// in tls13.c's now_unix() is exercised rather than bypassed.
typedef struct {
    uint32_t year, month, day, hour, minute, second;
} rtc_time_t;

static inline void sys_get_time(rtc_time_t* t) {
    time_t now = time(NULL);
    struct tm g;
    gmtime_r(&now, &g);
    t->year = (uint32_t)(g.tm_year + 1900);
    t->month = (uint32_t)(g.tm_mon + 1);
    t->day = (uint32_t)g.tm_mday;
    t->hour = (uint32_t)g.tm_hour;
    t->minute = (uint32_t)g.tm_min;
    t->second = (uint32_t)g.tm_sec;
}

#endif // MCT_TLS_HOST_SHIM_H
