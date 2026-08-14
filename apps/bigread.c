// bigread.c — Mectov OS multi-sector PIO benchmark (v38.25, Ring 3)
//
// Reads /bench.big (160 KB, seeded at boot) three times. 160 KB is above
// PCACHE_MAX_FILE (128 KB), so the page cache never absorbs it — every read
// is a real disk read through the multi-sector PIO path (one ATA command per
// up-to-16-sector batch instead of one per sector). Verifies the byte
// pattern (index mod 256) so a batch-offset bug cannot pass silently, and
// prints each read's wall time.
//
// Run it from the terminal:  run /apps/bigread.mct
#include "src/include/syscall.h"

#define BUF_SIZE (160 * 1024)
#define READS 3

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
        times[i] = t1 - t0;

        // Verify the seeded pattern: buf[k] == k & 0xFF. Scan on every read so
        // a data-corruption bug (wrong offset within a batch, bad DRQ loop)
        // is caught, not just the timing.
        int ok = 1;
        for (int k = 0; k < BUF_SIZE; k++) {
            if (buf[k] != (char)(k & 0xFF)) { ok = 0; break; }
        }
        if (!ok) { sys_print("[BIGREAD] FAIL pattern mismatch\n", 0x0C); sys_exit_with_code(1); }
    }

    mark("[BIGREAD] read0_us=", times[0]);
    mark("[BIGREAD] read1_us=", times[1]);
    mark("[BIGREAD] read2_us=", times[2]);
    // All three must be roughly equal (every read pays the disk; no cache).
    uint32_t avg = (times[0] + times[1] + times[2]) / READS;
    mark("[BIGREAD] avg_us=", avg);

    sys_print("[BIGREAD] ALL PASS\n", 0x0A);
    sys_exit_with_code(0);
}
