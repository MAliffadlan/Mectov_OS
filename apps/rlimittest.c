// rlimittest.c — Mectov OS resource-limit test (Ring 3, v38.28)
//
// Runs as the logged-in user (uid 1000). Exercises the RLIMIT_* subset
// end to end through real syscalls:
//
//   1. getrlimit defaults                -> cur/max sane (NPROC 64, AS 256 MB,
//                                           NOFILE 16)
//   2. setrlimit NPROC cur=1             -> allowed (lowering cur)
//   3. fork() with NPROC=1               -> refused (-1): the caller already
//                                           shares its uid with other live
//                                           tasks (terminal/shell), so count
//                                           >= 1 > limit
//   4. raise NPROC cur back to 64        -> allowed (cur <= max)
//   5. fork() with NPROC=64              -> succeeds; child exits 0, reaped
//   6. non-root may NOT raise max        -> setrlimit {64,128} refused (-1)
//   7. setrlimit NOFILE cur=4            -> allowed
//   8. fd allocation stops at the limit  -> open until it fails, then close
//                                           one and open again (succeeds)
//   9. setrlimit AS cur=1 MB             -> allowed
//  10. mmap 2 MB under a 1 MB limit      -> refused (0)
//  11. mmap 256 KB under a 1 MB limit    -> succeeds, then munmap
//  12. raise AS back to 64 MB            -> mmap 2 MB succeeds again
//
// A failure anywhere prints [RLIM] FAIL <step> and exits nonzero.
// Success prints [RLIM] ALL PASS.
//
// Run it from the terminal:  run /apps/rlimittest.mct
#include "src/include/syscall.h"

#define MB (1024u * 1024u)

static char buf[64];

static int fail(const char* step) {
    sys_print("[RLIM] FAIL ", 0x0C);
    sys_print(step, 0x0C);
    sys_print("\n", 0x0C);
    sys_exit();
    return -1;
}

void _start(void) {
    sys_print("[RLIM] start\n", 0x0E);
    rlimit_t rl;

    // 1. Defaults are sane.
    if (sys_getrlimit(RLIMIT_NPROC, &rl) != 0 || rl.cur != 64 || rl.max != 64)
        fail("nproc-default");
    if (sys_getrlimit(RLIMIT_AS, &rl) != 0 || rl.cur != 256 * MB || rl.max != 256 * MB)
        fail("as-default");
    if (sys_getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.cur != 16 || rl.max != 16)
        fail("nofile-default");

    // 2. Lower NPROC cur to 1 (max stays 64 — a non-root caller may only
    //    lower cur / raise cur up to max, never touch max itself).
    rl.cur = 1; rl.max = 64;
    if (sys_setrlimit(RLIMIT_NPROC, &rl) != 0) fail("set-nproc-1");

    // 3. fork() refused: uid 1000 is shared with the terminal/shell, so the
    //    per-uid count already exceeds a soft limit of 1.
    if (sys_fork() != -1) fail("fork-nproc-1");

    // 4. Raise NPROC cur back (cur <= max is legal for a non-root caller).
    rl.cur = 64; rl.max = 64;
    if (sys_setrlimit(RLIMIT_NPROC, &rl) != 0) fail("raise-nproc");

    // 5. fork() now succeeds; the child exits 0 and is reaped.
    int pid = sys_fork();
    if (pid < 0) fail("fork-nproc-64");
    if (pid == 0) {
        sys_exit_with_code(0);
    }
    int status = -1;
    if (sys_waitpid(pid, &status, 0) != pid) fail("waitpid");
    if (status != 0) fail("child-status");

    // 6. A non-root caller may never raise the hard limit.
    rl.cur = 64; rl.max = 128;
    if (sys_setrlimit(RLIMIT_NPROC, &rl) != -1) fail("raise-max");

    // 7-8. RLIMIT_NOFILE: fd allocation stops at the soft limit, and closing
    //      a descriptor frees a slot again. Use 4 so the test is robust no
    //      matter how many fds the launcher wired (0/1/2 may already be open).
    rl.cur = 4; rl.max = 16;
    if (sys_setrlimit(RLIMIT_NOFILE, &rl) != 0) fail("set-nofile");
    int fds[8];
    int nopen = 0;
    for (int i = 0; i < 8; i++) {
        fds[i] = sys_open("/hello.txt");
        if (fds[i] < 0) break;
        nopen++;
    }
    if (nopen < 1) fail("nofile-no-open");
    if (nopen > 4) fail("nofile-over-limit");  // >= 4 slots (0..3) max
    if (nopen < 4) {
        // Fewer than 4 were openable? Only possible if the soft limit was not
        // enforced (the task had more fds already wired than the limit).
        // Verify enforcement differently: with the limit at 4 the total
        // (pre-wired + ours) must never exceed 4, so opening again must fail.
        if (sys_open("/hello.txt") >= 0) fail("nofile-not-enforced");
    }
    // Close one, then opening succeeds again (slot freed).
    if (nopen > 0) {
        sys_close(fds[nopen - 1]);
        int fd2 = sys_open("/hello.txt");
        if (fd2 < 0) fail("nofile-after-close");
        sys_close(fd2);
    }
    for (int i = 0; i < nopen; i++) sys_close(fds[i]);
    rl.cur = 16; rl.max = 16;
    if (sys_setrlimit(RLIMIT_NOFILE, &rl) != 0) fail("restore-nofile");

    // 9-12. RLIMIT_AS: mmap reservations stop at the soft limit.
    rl.cur = 1 * MB; rl.max = 64 * MB;
    if (sys_setrlimit(RLIMIT_AS, &rl) != 0) fail("set-as");

    void* big = sys_mmap(2 * MB);
    if (big != 0) fail("mmap-over-as");      // 2 MB > 1 MB limit

    void* ok = sys_mmap(256 * 1024);
    if (ok == 0) fail("mmap-under-as");      // 256 KB fits under 1 MB
    if (sys_munmap(ok) != 1) fail("munmap");

    rl.cur = 64 * MB; rl.max = 64 * MB;
    if (sys_setrlimit(RLIMIT_AS, &rl) != 0) fail("raise-as");
    big = sys_mmap(2 * MB);
    if (big == 0) fail("mmap-after-raise");
    if (sys_munmap(big) != 1) fail("munmap2");

    sys_print("[RLIM] ALL PASS\n", 0x0E);
    sys_exit();
}
