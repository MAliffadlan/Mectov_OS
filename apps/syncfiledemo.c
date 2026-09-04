// syncfiledemo — proves fsync() and the kernel's periodic write-back
// (v38.61). Mectov's regular file I/O is synchronous write-through, so dirty
// file-backed mmap pages are the ONLY data that can be lost on a power cut
// (they normally wait for msync/munmap). This demo exercises the two new
// durability paths that flush them earlier:
//
//   1. fsync(fd)  — flush the file's dirty mmap pages WHILE the mapping stays
//      alive. We then re-open the file through a fresh fd and read it: the
//      write must already be on disk, with no msync/munmap having run.
//   2. periodic write-back — dirty the page again but call NOTHING, then
//      sleep for 12 s. The kernel's ~5 s main-loop write-back must flush it
//      on its own (serial log shows "[SYNC]" / "[MMAP] flushed" without any
//      app syscall); after waking, a fresh fd read must see the data.
//
// If either phase fails, the bytes only lived in RAM — the exact failure
// mode fsync/sync exist to prevent.
#include "src/include/syscall.h"

static int fails = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { sys_print(msg, 0x0C); fails++; } \
} while (0)

static int all_eq(const char* p, char c, int n) {
    for (int i = 0; i < n; i++) if (p[i] != c) return 0;
    return 1;
}

// Write `c` over the first `n` bytes of path via regular file I/O.
static int seed_file(const char* path, char c, int n) {
    sys_delete_file(path);
    if (sys_create_file(path) < 0 && sys_stat_file(path) < 0) return -1;
    int fd = sys_open(path);
    if (fd < 0) return -1;
    char buf[512];
    for (int i = 0; i < 512; i++) buf[i] = c;
    int left = n;
    int off = 0;
    while (left > 0) {
        int chunk = (left > 512) ? 512 : left;
        if (sys_write(fd, buf, chunk) != chunk) { sys_close(fd); return -1; }
        left -= chunk;
        off += chunk;
    }
    sys_close(fd);
    return 0;
}

// Re-open the file and verify every byte equals `c`.
static int verify_file(const char* path, char c, int n, const char* failmsg) {
    int fd = sys_open(path);
    if (fd < 0) { sys_print(failmsg, 0x0C); fails++; return -1; }
    int ok = 1;
    char buf[512];
    int got = 0;
    while (got < n) {
        int r = sys_read(fd, buf, 512);
        if (r <= 0) { ok = 0; break; }
        if (!all_eq(buf, c, r)) { ok = 0; break; }
        got += r;
    }
    sys_close(fd);
    if (!ok || got != n) { sys_print(failmsg, 0x0C); fails++; return -1; }
    return 0;
}

void _start(void) {
    const int N = 4096;  // one 4K page
    const char* path = "/syncfile.txt";

    sys_print("syncfiledemo: fsync + periodic write-back test\n", 0x0F);

    // Seed the file with 'A's.
    if (seed_file(path, 'A', N) < 0) {
        sys_print("syncfiledemo: FAIL seed\n", 0x0C);
        sys_exit_with_code(1);
    }

    // Map it MAP_SHARED. Keep the fd open — fsync needs it.
    int fd = sys_open(path);
    if (fd < 0) {
        sys_print("syncfiledemo: FAIL open\n", 0x0C);
        sys_exit_with_code(1);
    }
    void* base = sys_mmap_file(fd, 1);  // MMAP_FILE_SHARED
    if (base == 0) {
        sys_print("syncfiledemo: FAIL mmap_file\n", 0x0C);
        sys_exit_with_code(1);
    }
    sys_print("syncfiledemo: mapped (lazy fault-in)...\n", 0x0F);

    // Fault the page in and check the seeded content.
    CHECK(all_eq((const char*)base, 'A', N),
          "syncfiledemo: FAIL content after fault-in\n");

    // ---- Phase 1: fsync() ----
    // Dirty the whole page to 'B', then fsync the fd. The write must land on
    // disk while the mapping is still alive and untouched by msync/munmap.
    for (int i = 0; i < N; i++) ((char*)base)[i] = 'B';
    sys_print("syncfiledemo: dirtied to 'B', calling fsync(fd)...\n", 0x0F);
    if (sys_fsync(fd) != 0) {
        sys_print("syncfiledemo: FAIL fsync returned error\n", 0x0C);
        fails++;
    } else {
        sys_print("syncfiledemo: fsync OK\n", 0x0A);
    }
    CHECK(verify_file(path, 'B', N,
          "syncfiledemo: FAIL file not on disk after fsync\n") == 0,
          "syncfiledemo: FAIL fsync persistence\n");
    sys_print("syncfiledemo: fsync persisted OK\n", 0x0A);

    // ---- Phase 2: periodic write-back ----
    // Dirty the page to 'C' and call NOTHING — no msync/fsync/sync/munmap.
    // Then sleep 12 s: the kernel's ~5 s write-back must flush it by itself.
    for (int i = 0; i < N; i++) ((char*)base)[i] = 'C';
    sys_print("syncfiledemo: dirtied to 'C', sleeping 12s (no syscall)...\n", 0x0F);
    sys_sleep(12000);
    CHECK(verify_file(path, 'C', N,
          "syncfiledemo: FAIL periodic write-back never flushed file\n") == 0,
          "syncfiledemo: FAIL periodic persistence\n");
    sys_print("syncfiledemo: periodic write-back persisted OK\n", 0x0A);

    // Clean up (munmap flushes nothing here — the flush above already did).
    if (!sys_munmap(base)) {
        sys_print("syncfiledemo: FAIL munmap\n", 0x0C);
        fails++;
    }
    sys_close(fd);

    if (fails == 0) {
        sys_print("syncfiledemo: ALL TESTS PASSED\n", 0x0A);
        sys_exit_with_code(0);
    } else {
        sys_print("syncfiledemo: TESTS FAILED\n", 0x0C);
        sys_exit_with_code(1);
    }
}
