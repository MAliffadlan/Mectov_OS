/* M4 Ring-3 syscall stubs (M4 ABI0 over int $0x80).
 * Freestanding: own types, no libc. The kernel preserves all registers
 * except RAX (result), so only "memory" needs clobbering. */
#ifndef SYS64_H
#define SYS64_H

typedef unsigned long u64;
typedef unsigned int u32;

static inline long sys0(long n) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n) : "memory");
    return r;
}
static inline long sys1(long n, u64 a) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a) : "memory");
    return r;
}
static inline long sys2(long n, u64 a, u64 b) {
    long r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(n), "b"(a), "c"(b)
                         : "memory");
    return r;
}
static inline long sys3(long n, u64 a, u64 b, u64 c) {
    long r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(n), "b"(a), "c"(b), "d"(c)
                         : "memory");
    return r;
}

static unsigned d_strlen(const char *s) __attribute__((unused));
static unsigned d_strlen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}
static inline void d_puts(const char *s) {
    sys2(1, (u64)s, d_strlen(s));
}
static void d_putu(u64 v) __attribute__((unused));
static void d_putu(u64 v) {
    char buf[21];
    int n = 0;
    if (!v) { d_puts("0"); return; }
    while (v && n < 20) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) { char c = buf[--n]; sys2(1, (u64)&c, 1); }
}
/* Fixed 6-fraction printing for exact-value checks (no float printf). */
static void d_putd(double d) __attribute__((unused));
static void d_putd(double d) {
    if (d < 0) { d_puts("-"); d = -d; }
    unsigned long long ip = (unsigned long long)d;
    d_putu(ip);
    d_puts(".");
    unsigned long long frac = (unsigned long long)((d - (double)ip) * 1000000.0);
    /* MSD-first via divisor (exact for our 6-digit expectations). */
    unsigned long long div = 100000;
    for (int i = 0; i < 6; i++) {
        char c = (char)('0' + (frac / div) % 10);
        sys2(1, (u64)&c, 1);
        div /= 10;
    }
}

static inline long d_pid(void) { return sys0(20); }
static inline long d_ticks(void) { return sys0(8); }
static inline long d_yield(void) { return sys0(9); }
static inline long d_sleep(u64 t) { return sys1(19, t); }
static inline long d_exec(const char *name) { return sys1(76, (u64)name); }
/* waitpid(pid, &status, wnohang) -> reaped pid / 0 / negative errno. */
static inline long d_wait(long pid, long *st, long wnohang) {
    return sys3(72, (u64)pid, (u64)st, (u64)wnohang);
}

/* M7.2 extensions (mirror k64/cpu64.h numbers/layouts). */
typedef struct {
    int id, state, parent, cpu;
    char name[16];
} ps_entry_t;
typedef struct {
    u64 total_frames, free_frames;
} meminfo_t;
/* M10 kernel heap snapshot (mirror of kmem64_t in k64/cpu64.h). */
typedef struct {
    u64 arena_base, arena_max, arena_used, arena_mapped;
    u64 pages, growths;
    u64 allocated, free_bytes, blocks, free_blocks, largest_free;
    u64 allocs, frees, oom, canary_failures, magic_failures, probes,
        probe_failures;
} kmem64_t;
static inline long d_kmemstats(kmem64_t *s) { return sys1(137, (u64)s); }
static inline long d_kmemprobe(u64 bytes) { return sys1(138, bytes); }
static inline long d_meminfo(meminfo_t *m) { return sys1(131, (u64)m); }
static inline long d_getcpu(void) { return sys0(133); }
static inline long d_getbase(void) { return sys0(132); }
static inline long d_brk(u64 nw) { return sys1(120, nw); }
static inline long d_ps(ps_entry_t *b, long max) {
    return sys2(134, (u64)b, (u64)max);
}
static inline long d_getchar(void) { return sys0(135); }
static inline long d_spawn(const char *n, long argc, const char **argv) {
    return sys3(136, (u64)n, (u64)argc, (u64)argv);
}

/* Line-buffered emit: a full line goes out in ONE syscall, so concurrent
 * tasks on SMP can interleave lines but never shatter them (each PRINT is
 * kernel-atomic, but a line built from N PRINTs is not).
 *
 * The buffer MUST live on the caller's stack (DLINE): clone workers share
 * all globals, so a shared static buffer corrupts across threads. Stacks
 * are always private per task (fresh pages even for clones). */
typedef struct {
    char b[224];
    int n;
} dline_t;
#define DLINE(name) dline_t name = { { 0 }, 0 }
static inline void dl_flush(dline_t *l) {
    if (l->n > 0) {
        sys2(1, (u64)l->b, (u64)l->n);
        l->n = 0;
    }
}
static inline void dl_s(dline_t *l, const char *s) {
    for (; *s; s++) {
        if (l->n >= (int)sizeof(l->b)) dl_flush(l);
        l->b[l->n++] = *s;
    }
}
static inline void dl_u(dline_t *l, u64 v) {
    char tmp[21];
    int n = 0;
    if (!v) { dl_s(l, "0"); return; }
    while (v && n < 20) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) {
        if (l->n >= (int)sizeof(l->b)) dl_flush(l);
        l->b[l->n++] = tmp[--n];
    }
}
static inline void dl_nl(dline_t *l) {
    if (l->n >= (int)sizeof(l->b)) dl_flush(l);
    l->b[l->n++] = '\n';
    dl_flush(l);
}

#endif
