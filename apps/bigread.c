// bigread.c — Mectov OS disk benchmark (v38.25, Ring 3; v38.62 block cache)
//
// Reads /bench.big (160 KB, seeded at boot) three times. 160 KB is above
// PCACHE_MAX_FILE (128 KB), so the whole-file page cache never absorbs it.
//
//  - v38.25 (cold): every read went to the disk through the multi-sector PIO
//    path (one ATA command per up-to-16-sector batch instead of one per
//    sector), so the three reads measured roughly equal.
//  - v38.62: the new sector-level block cache (blkcache.c, 256 KB, sits
//    under all filesystems at the ata_read_sectors_drive entry) absorbs the
//    file after the first read. read0 is the cold disk read; read1/read2 are
//    served from RAM and must be several times faster — the verdict below
//    turns that into a regression assertion for the block cache.
//
// Verifies the byte pattern (index mod 256) on every read so a data-
// corruption bug (wrong cache offset, stale fill after a write, bad DRQ
// loop) cannot pass silently, and prints each read's wall time.
//
// Run it from the terminal:  run /apps/bigread.mct
#include "src/include/syscall.h"

#define BUF_SIZE (160 * 1024)
#define READS 6

static char buf[BUF_SIZE];

static void itoa_dec(int n, char* out) {
    if (n < 0) { *out++ = '-'; n = -n; }
    if (n == 0) { out[0] = '0'; out[1] = '\0'; return; }
    char tmp[12]; int t = 0;
    while (n > 0) { tmp[t++] = (char)('0' + n % 10); n /= 10; }
    for (int i = t - 1; i >= 0; i--) *out++ = tmp[i];
    *out = '\0';
}

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

void _start(void) {
    sys_print("[BIGREAD] start\n", 0x0E);

    int fd = sys_open("/bench.big");
    if (fd < 0) { sys_print("[BIGREAD] FAIL open\n", 0x0C); sys_exit_with_code(1); }
    stat_t st;
    if (sys_fstat(fd, &st) != 0) { sys_print("[BIGREAD] FAIL fstat\n", 0x0C); sys_exit_with_code(1); }
    sys_close(fd);

    if (st.size != BUF_SIZE) {
        sys_print("[BIGREAD] FAIL size (expected 160KB)\n", 0x0C);
        sys_exit_with_code(1);
    }

    uint32_t times[READS];
    for (int i = 0; i < READS; i++) {
        fd = sys_open("/bench.big");
        if (fd < 0) { sys_print("[BIGREAD] FAIL reopen\n", 0x0C); sys_exit_with_code(1); }
        uint32_t t0 = now_us();
        int r = sys_read(fd, buf, BUF_SIZE);
        uint32_t t1 = now_us();
        sys_close(fd);
        if (r != BUF_SIZE) { sys_print("[BIGREAD] FAIL short read\n", 0x0C); sys_exit_with_code(1); }
        // A monotonic read straddling a second boundary can report t1 < t0 by
        // a whisker; clamp to 0 (a cache-hot read is genuinely sub-\u00b5s).
        times[i] = (t1 >= t0) ? (t1 - t0) : 0;

        // Verify the seeded pattern: buf[k] == k & 0xFF. Scan on every read so
        // a data-corruption bug (wrong offset within a batch, bad DRQ loop)
        // is caught, not just the timing.
        int ok = 1;
        for (int k = 0; k < BUF_SIZE; k++) {
            if (buf[k] != (char)(k & 0xFF)) { ok = 0; break; }
        }
        if (!ok) { sys_print("[BIGREAD] FAIL pattern mismatch\n", 0x0C); sys_exit_with_code(1); }
    }

    for (int i = 0; i < READS; i++) {
        sys_print("[BIGREAD] read", 0x0E);
        char idx[4]; itoa_dec(i, idx); sys_print(idx, 0x0E);
        sys_print("_us=", 0x0E);
        char num[16]; itoa_dec((int)times[i], num); sys_print(num, 0x0E);
        sys_print("\n", 0x0E);
    }
    // v38.62 verdict: read0 is the cold disk read; the later reads must be
    // served from the block cache and come back at least 4x faster (mirrors
    // the iobench page-cache verdict). min(hot) is asserted, not the mean:
    // a 160 KB copy to user space is preemptible under TCG, so an occasional
    // timer-interrupt-stretched read must not fail the cache. A single
    // unrelated write (e.g. shell history) between reads can also bump the
    // cache generation and legitimately make ONE read cold again.
    uint32_t hot_min = 0xFFFFFFFFu;
    for (int i = 1; i < READS; i++) {
        if (times[i] < hot_min) hot_min = times[i];
    }
    int ok_hot = hot_min * 4 < times[0] + 64;
    if (!ok_hot) {
        sys_print("[BIGREAD] FAIL hot reads not cache-fast (block cache?)\n", 0x0C);
        sys_exit_with_code(1);
    }

    uint32_t avg = (times[0] + times[1] + times[2]) / READS;
    mark("[BIGREAD] avg_us=", avg);

    sys_print("[BIGREAD] ALL PASS\n", 0x0A);
    sys_exit_with_code(0);
}
