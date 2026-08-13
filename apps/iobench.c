// iobench.c — Mectov OS page-cache micro benchmark (Ring 3)
//
// Demonstrates the whole-file page cache (src/sys/pcache.c) from user space:
//
//   Phase 1 (cached reads):   read a pre-existing seeded app file 30 times
//     with CLOCK_MONOTONIC timing. The first read is a cache MISS (the boot
//     seed only cached 4 magic bytes, which mismatch the full size) and pays
//     the disk; every later read is a RAM hit. Verdict asserts the cached
//     reads are several times faster than the cold read.
//
//   Phase 2 (same-size writes): rewrite a freshly created file 10 times.
//     Each rewrite is a write-through (8 data sectors) but skips the
//     256-sector vfs_save() when the node record is unchanged, so repeated
//     writes are cheap (printed for the log, not asserted — TCG timing is
//     environment-dependent).
//
// Run it from the terminal:  run /apps/iobench.mct
#include "src/include/syscall.h"

#define READ_FILE  "/apps/hello.mct"
#define WRITE_FILE "/bench.dat"
#define WRITE_SIZE 4095     // fd writes are whole-file RMW capped at 4 KB
#define READS      30
#define WRITES     10

static void itoa_dec(int n, char* out) {
    if (n < 0) { *out++ = '-'; n = -n; }
    if (n == 0) { out[0] = '0'; out[1] = '\0'; return; }
    char tmp[12]; int t = 0;
    while (n > 0) { tmp[t++] = (char)('0' + n % 10); n /= 10; }
    for (int i = t - 1; i >= 0; i--) *out++ = tmp[i];
    *out = '\0';
}

// Monotonic time in microseconds (SYS_CLOCK_GETTIME).
static uint32_t now_us(void) {
    timespec_t ts;
    if (sys_clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return ts.tv_sec * 1000000u + ts.tv_nsec / 1000u;
}

static void mark(const char* tag, uint32_t v) {
    sys_print(tag, 0x0E);
    char num[16];
    itoa_dec((int)v, num);
    sys_print(num, 0x0E);
    sys_print("\n", 0x0E);
}

static char buf[8192];

void _start(void) {
    sys_print("[IOBENCH] start\n", 0x0E);

    // ---- Phase 1: cold vs cached reads of a pre-existing file ----
    // hello.mct ships on every disk (embedded + seeded at boot), and the
    // boot-time seeding only ever read its 4 magic bytes — so the first
    // full-size read here is a genuine cache miss (disk), and reads 2..N
    // are served from RAM.
    int fd = sys_open(READ_FILE);
    if (fd < 0) { sys_print("[IOBENCH] open read file FAILED\n", 0x0C); sys_exit(); }
    stat_t st;
    if (sys_fstat(fd, &st) != 0 || st.size <= 0 || st.size > 8192) {
        sys_print("[IOBENCH] fstat FAILED\n", 0x0C); sys_exit();
    }
    sys_close(fd);
    int fsize = st.size;
    mark("[IOBENCH] file_size=", (uint32_t)fsize);

    uint32_t cold = 0, hot_sum = 0, hot_min = 0xFFFFFFFFu;
    int hot_n = 0;
    for (int i = 0; i < READS; i++) {
        fd = sys_open(READ_FILE);
        if (fd < 0) { sys_print("[IOBENCH] reopen FAILED\n", 0x0C); sys_exit(); }
        uint32_t t0 = now_us();
        int r = sys_read(fd, buf, fsize);
        uint32_t t1 = now_us();
        sys_close(fd);
        if (r != fsize) { sys_print("[IOBENCH] short read\n", 0x0C); sys_exit(); }
        uint32_t d = t1 - t0;
        if (i == 0) cold = d;
        else { hot_sum += d; hot_n++; if (d < hot_min) hot_min = d; }
    }
    // Use the MINIMUM of the cached reads, not the average: the fixed syscall
    // overhead (int 0x80 + user-pointer validation) is comparable to the disk
    // time on TCG, so averages drown the cache gain in emulation noise.
    uint32_t hot = hot_n ? hot_min : 0;
    mark("[IOBENCH] cold_us=", cold);
    mark("[IOBENCH] hot_us=", hot);
    mark("[IOBENCH] speedup=", cold ? cold / (hot ? hot : 1) : 0);

    // ---- Phase 2: repeated same-size writes (save-skip path) ----
    for (int i = 0; i < WRITE_SIZE; i++) buf[i] = (char)('a' + (i % 26));
    if (sys_create_file(WRITE_FILE) < 0) { sys_print("[IOBENCH] create FAILED\n", 0x0C); sys_exit(); }
    fd = sys_open(WRITE_FILE);
    if (fd < 0) { sys_print("[IOBENCH] write-open FAILED\n", 0x0C); sys_exit(); }
    if (sys_write(fd, buf, WRITE_SIZE) != WRITE_SIZE) {
        sys_print("[IOBENCH] first write FAILED\n", 0x0C); sys_exit();
    }
    sys_close(fd);

    uint32_t w_sum = 0;
    for (int i = 0; i < WRITES; i++) {
        fd = sys_open(WRITE_FILE);
        if (fd < 0) { sys_print("[IOBENCH] wopen FAILED\n", 0x0C); sys_exit(); }
        uint32_t t0 = now_us();
        sys_write(fd, buf, WRITE_SIZE);
        uint32_t t1 = now_us();
        sys_close(fd);
        w_sum += t1 - t0;
    }
    mark("[IOBENCH] write_avg_us=", w_sum / WRITES);
    sys_delete_file(WRITE_FILE);

    // ---- Verdict ----
    if (cold > 0 && hot > 0 && hot * 3 < cold) {
        sys_print("[IOBENCH] verdict OK (cached reads several times faster)\n", 0x0E);
    } else {
        sys_print("[IOBENCH] verdict BAD (cache not helping)\n", 0x0C);
    }
    sys_print("[IOBENCH] DONE\n", 0x0E);
    sys_exit();
}
