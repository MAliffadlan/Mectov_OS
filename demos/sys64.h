/* M4 Ring-3 syscall stubs (M4 ABI0 over int $0x80).
 * Freestanding: own types, no libc. The kernel preserves all registers except
 * RAX (result), so only "memory" needs clobbering — GPRs *and* the vector/
 * x87 state, which is why GCC may keep a live constant in xmm across a call
 * (it does: the shell's fs request magic lives in xmm1 for its whole life).
 *
 * Two kernel invariants make that promise true, and both are load-bearing:
 *   - kernel code never executes a vector instruction (`CFLAGS64` has
 *     -mgeneral-regs-only) — otherwise a syscall would silently trash them;
 *   - every task switch saves the outgoing image, including the voluntary
 *     block in sleep/waitpid (k64/task64.c fx_save_self()): the scheduler's
 *     own save only covers a task still T_RUNNING when it arrives, so a task
 *     that demotes itself must save first. M12 lost a request struct to a
 *     missed save here, and it looked exactly like memory corruption. */
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

/* M12 read-only filesystem (mirror of fs64_req_t / FS64_OP_* in k64/cpu64.h).
 * One syscall per operation, op-selected; `magic` is what catches a layout
 * drift between this header and the kernel's, so a mismatch returns EINVAL
 * instead of silently reinterpreting a field. */
typedef struct {
    char name[64];
    u64 size;
    u32 id;
    int is_dir;
} d_dirent_t;
typedef struct {
    u32 magic;
    u32 op;
    u32 mnt;
    u32 index;
    u64 off;
    u64 len;
    u64 buf;
    char path[128];
    long rc;
    u64 hash; /* D_FS_HASH result (rc = success/error only) */
    d_dirent_t ent;
} d_fsreq_t;
#define D_FS_MAGIC 0x5346C0DEu
#define D_FS_LS 1
#define D_FS_CAT 2
#define D_FS_STAT 3
#define D_FS_HASH 4
#define D_FS_MOUNT 5

/* Every op goes through the same tiny builder, so no call site can forget the
 * magic or leave a stale field from a previous request. */
static inline long d_fs_call(d_fsreq_t *q, u32 op, u32 mnt, const char *path,
                             u32 index, u64 off, u64 len, void *buf) {
    q->magic = D_FS_MAGIC;
    q->op = op;
    q->mnt = mnt;
    q->index = index;
    q->off = off;
    q->len = len;
    q->buf = (u64)buf;
    int i = 0;
    for (; path[i] && i < (int)sizeof(q->path) - 1; i++) q->path[i] = path[i];
    q->path[i] = '\0';
    return sys1(139, (u64)q);
}
static inline long d_fs_ls(d_fsreq_t *q, u32 mnt, const char *path, u32 idx) {
    return d_fs_call(q, D_FS_LS, mnt, path, idx, 0, 0, 0);
}
static inline long d_fs_stat(d_fsreq_t *q, u32 mnt, const char *path) {
    return d_fs_call(q, D_FS_STAT, mnt, path, 0, 0, 0, 0);
}
static inline long d_fs_cat(d_fsreq_t *q, u32 mnt, const char *path, u64 off,
                            void *buf, u64 len) {
    return d_fs_call(q, D_FS_CAT, mnt, path, 0, off, len, buf);
}
static inline long d_fs_hash(d_fsreq_t *q, u32 mnt, const char *path, u64 len) {
    return d_fs_call(q, D_FS_HASH, mnt, path, 0, 0, len, 0);
}
/* Enumerate mount `idx`: ent.id = fs type (1 ISO9660, 2 ext2), ent.size = block
 * count, ent.name = volume label. */
static inline long d_fs_mount(d_fsreq_t *q, u32 idx) {
    return d_fs_call(q, D_FS_MOUNT, 0, "/", idx, 0, 0, 0);
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
/* 64-bit hex, so hashes/sizes read the same on screen as they do in a gate
 * regex (decimal FNV values are unreadable and unreviewable by eye). */
static inline void dl_x(dline_t *l, u64 v) {
    const char *digits = "0123456789ABCDEF";
    dl_s(l, "0x");
    for (int i = 15; i >= 0; i--) {
        char c = digits[(v >> (i * 4)) & 0xF];
        if (l->n >= (int)sizeof(l->b)) dl_flush(l);
        l->b[l->n++] = c;
    }
}
static inline void dl_nl(dline_t *l) {
    if (l->n >= (int)sizeof(l->b)) dl_flush(l);
    l->b[l->n++] = '\n';
    dl_flush(l);
}

#endif
