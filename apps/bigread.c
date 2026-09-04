// bigread.c — Mectov OS disk benchmark (v38.25 Ring 3, v38.62 block cache,
// v38.65 sequential readahead)
//
// Streams /bench.big (160 KB, seeded at boot) in 4 KB chunks — the access
// pattern of a real small-buffer reader. 160 KB is above PCACHE_MAX_FILE
// (128 KB), so the whole-file page cache never absorbs it; every demand
// reaches the ATA layer as an 8-sector read.
//
//  - v38.25 (cold): every 4 KB chunk was a separate disk command — one ATA
//    multi-sector read per chunk, ~40 disk commands per pass.
//  - v38.62: the sector-level block cache absorbs the file after the first
//    pass, so passes 1..N issue ~zero disk reads.
//  - v38.65: sequential readahead ALSO makes the first (cold) pass cheap —
//    once the stream pattern is confirmed, each miss prefetches the next
//    32 KB into the cache, coalescing the ~40 small demands into a handful
//    of disk commands.
//
// The proof is deterministic and does not rely on wall-clock noise: the app
// reads /proc/atastats (kernel ATA multi-sector read command counter) before
// and after each pass, and asserts
//    * the cold pass issued <= RA_CMDS_ALLOWED disk reads (readahead
//      coalesced the stream — without it, 40+ commands),
//    * every later pass issued <= 2 (the block cache served it from RAM).
// Timings are printed for the log but never drive the verdict. The byte
// pattern (index mod 256) is verified on every pass so a data-corruption bug
// (wrong cache offset, stale fill after a write, bad prefetch range) cannot
// pass silently.
//
// Run it from the terminal:  run /apps/bigread.mct
#include "src/include/syscall.h"

#define BUF_SIZE (160 * 1024)
#define CHUNK 4096
#define PASSES 5
#define RA_CMDS_ALLOWED 18   // cold-pass disk-command budget (readahead)

static char buf[BUF_SIZE];
static char procbuf[160];

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

// Read the kernel's ATA multi-sector read command counter from /proc.
static int read_ata_cmds(void) {
    int fd = sys_open("/proc/atastats");
    if (fd < 0) return -1;
    int n = sys_read(fd, procbuf, (int)sizeof(procbuf) - 1);
    sys_close(fd);
    if (n < 0) return -1;
    procbuf[n] = '\0';
    // Format: "ATA_RD_CMDS=NNNN RA_FILLS=NNNN"
    const char* p = procbuf;
    while (*p && *p != '=') p++;
    if (*p != '=') return -1;
    p++;
    int v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    return v;
}

static int verify_pattern(int pass) {
    for (int k = 0; k < BUF_SIZE; k++) {
        if (buf[k] != (char)(k & 0xFF)) {
            sys_print("[BIGREAD] FAIL pattern mismatch pass ", 0x0C);
            char idx[4]; itoa_dec(pass, idx);
            sys_print(idx, 0x0C);
            sys_print("\n", 0x0C);
            return 0;
        }
    }
    return 1;
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
    if (read_ata_cmds() < 0) {
        sys_print("[BIGREAD] FAIL /proc/atastats\n", 0x0C);
        sys_exit_with_code(1);
    }

    uint32_t us[PASSES];
    int cmds[PASSES];
    for (int p = 0; p < PASSES; p++) {
        fd = sys_open("/bench.big");
        if (fd < 0) { sys_print("[BIGREAD] FAIL reopen\n", 0x0C); sys_exit_with_code(1); }

        // Stream the file in 4 KB chunks (v38.65: the pattern readahead
        // exists for). Count disk commands around the data reads only.
        int c0 = read_ata_cmds();
        uint32_t t0 = now_us();
        int off = 0, r = 0;
        while (off < BUF_SIZE) {
            r = sys_read(fd, buf + off, CHUNK);
            if (r <= 0) break;
            off += r;
        }
        uint32_t t1 = now_us();
        int c1 = read_ata_cmds();
        sys_close(fd);

        if (off != BUF_SIZE) { sys_print("[BIGREAD] FAIL short read\n", 0x0C); sys_exit_with_code(1); }
        // A monotonic read straddling a second boundary can report t1 < t0 by
        // a whisker; clamp to 0 (a cache-hot pass is genuinely sub-µs).
        us[p] = (t1 >= t0) ? (t1 - t0) : 0;
        cmds[p] = (c0 >= 0 && c1 >= 0) ? (c1 - c0) : -1;

        if (!verify_pattern(p)) sys_exit_with_code(1);

        sys_print("[BIGREAD] pass", 0x0E);
        char idx[4]; itoa_dec(p, idx); sys_print(idx, 0x0E);
        sys_print("_us=", 0x0E);
        char num[16]; itoa_dec((int)us[p], num); sys_print(num, 0x0E);
        sys_print(" cmds=", 0x0E);
        itoa_dec(cmds[p], num); sys_print(num, 0x0E);
        sys_print("\n", 0x0E);
    }

    // ---- v38.62 block cache: every pass after the first must be served
    // from the sector cache (<= 2 residual disk reads, e.g. a metadata
    // re-read racing between passes — on a quiet system it is 0).
    int ok = 1;
    for (int p = 1; p < PASSES; p++) {
        if (cmds[p] < 0 || cmds[p] > 2) {
            sys_print("[BIGREAD] FAIL hot pass issued disk reads (block cache?)\n", 0x0C);
            ok = 0;
            break;
        }
    }
    // ---- v38.65 sequential readahead: the COLD pass must not pay one disk
    // command per 4 KB demand. Without readahead that is ~40+ commands (the
    // 8-sector demands); the prefetch coalesces them into a handful. A warm
    // re-run (the test relaunches the app) trivially passes with ~0 cmds.
    if (ok && (cmds[0] < 0 || cmds[0] > RA_CMDS_ALLOWED)) {
        sys_print("[BIGREAD] FAIL cold pass not readahead-coalesced\n", 0x0C);
        ok = 0;
    }
    // ---- timing (informational only; verdicts are the counters above) ----
    for (int p = 0; p < PASSES; p++) {
        sys_print("[BIGREAD] t_pass", 0x0E);
        char idx[4]; itoa_dec(p, idx); sys_print(idx, 0x0E);
        sys_print("_us=", 0x0E);
        char num[16]; itoa_dec((int)us[p], num); sys_print(num, 0x0E);
        sys_print("\n", 0x0E);
    }

    if (!ok) sys_exit_with_code(1);
    sys_print("[BIGREAD] ALL PASS\n", 0x0A);
    sys_exit_with_code(0);
}
