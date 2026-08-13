// pollselectdemo — exercises the POSIX syscalls added in v38.18:
//   SYS_POLL (97), SYS_SELECT (98), SYS_GETCWD (99), SYS_CHDIR (100),
//   SYS_CLOCK_GETTIME (101).
//
//   1. poll() on an open file -> POLLIN|POLLOUT, returns 1
//   2. poll() on an empty pipe (timeout 0) -> returns 0
//   3. poll() after writing to the pipe -> POLLIN, returns 1
//   4. poll() on a closed fd -> POLLNVAL in revents
//   5. poll() timeout > 0 on an empty pipe -> returns 0, and clock_gettime
//      proves the wait actually took roughly the requested wall time
//   6. poll() after the write end closes -> POLLIN|POLLHUP (EOF)
//   7. select() readfds: empty pipe (timeout 0) -> 0; after a write -> 1
//   8. select() with a real timeout -> returns 0 (nothing readable)
//   9. getcwd() after chdir("/") -> "/"
//  10. chdir("/apps") -> getcwd "/apps"; chdir("..") -> "/"; chdir to a
//      non-directory and to a missing path both fail with -1
//  11. clock_gettime() is monotonic (t2 >= t1); bad clock id -> -1
#include "src/include/syscall.h"

static int fails = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { sys_print(msg, 0x0C); fails++; } \
} while (0)

static int slen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

void _start(void) {
    sys_print("pollselectdemo: poll/select/getcwd/chdir/clock_gettime test\n", 0x0F);

    // --- 1. poll() on an open file: always POLLIN|POLLOUT, ready=1 ---
    int fd = sys_open("/apps/pollselectdemo.mct");
    if (fd < 0) {
        sys_print("pollselectdemo: FAIL open file\n", 0x0C);
        sys_exit_with_code(1);
    }
    {
        pollfd_t pfd;
        pfd.fd = fd;
        pfd.events = POLLIN | POLLOUT;
        pfd.revents = 0;
        int r = sys_poll(&pfd, 1, 0);
        CHECK(r == 1, "pollselectdemo: FAIL file poll ready count\n");
        CHECK((pfd.revents & POLLIN) != 0, "pollselectdemo: FAIL file POLLIN\n");
        CHECK((pfd.revents & POLLOUT) != 0, "pollselectdemo: FAIL file POLLOUT\n");
    }
    sys_close(fd);

    // --- pipes: read fd 0, write fd 1 ---
    int pipefd[2];
    if (sys_pipe(pipefd) != 0) {
        sys_print("pollselectdemo: FAIL pipe\n", 0x0C);
        sys_exit_with_code(1);
    }

    // --- 2. poll() on an empty pipe, timeout 0 -> 0 ready ---
    {
        pollfd_t pfd;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        int r = sys_poll(&pfd, 1, 0);
        CHECK(r == 0, "pollselectdemo: FAIL empty pipe poll\n");
    }

    // --- 3. poll() after writing -> POLLIN ---
    {
        char w = 'X';
        sys_write(pipefd[1], &w, 1);
        pollfd_t pfd;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        int r = sys_poll(&pfd, 1, 0);
        CHECK(r == 1, "pollselectdemo: FAIL written pipe poll\n");
        CHECK((pfd.revents & POLLIN) != 0, "pollselectdemo: FAIL pipe POLLIN\n");
        char b;
        sys_read(pipefd[0], &b, 1);  // drain
    }

    // --- 4. poll() on a closed fd -> POLLNVAL ---
    {
        int bad = pipefd[0];
        sys_close(bad);
        pollfd_t pfd;
        pfd.fd = bad;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int r = sys_poll(&pfd, 1, 0);
        CHECK(r == 1, "pollselectdemo: FAIL closed fd poll\n");
        CHECK(pfd.revents == POLLNVAL, "pollselectdemo: FAIL POLLNVAL\n");
        pipefd[0] = -1;
    }

    // --- 5. poll() timeout > 0: blocks, then returns 0; elapsed ~= timeout ---
    {
        int p2[2];
        if (sys_pipe(p2) != 0) {
            sys_print("pollselectdemo: FAIL pipe2\n", 0x0C);
            sys_exit_with_code(1);
        }
        timespec_t t1, t2;
        sys_clock_gettime(CLOCK_MONOTONIC, &t1);
        pollfd_t pfd;
        pfd.fd = p2[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        int r = sys_poll(&pfd, 1, 50);  // 50 ms
        sys_clock_gettime(CLOCK_MONOTONIC, &t2);
        CHECK(r == 0, "pollselectdemo: FAIL poll timeout result\n");
        // Rough wall-time check: at least 30ms, at most 5s (TCG is slow).
        uint32_t elapsed_ms = (t2.tv_sec - t1.tv_sec) * 1000
                            + (t2.tv_nsec - t1.tv_nsec) / 1000000;
        CHECK(elapsed_ms >= 30 && elapsed_ms <= 5000,
              "pollselectdemo: FAIL poll timeout elapsed\n");
        sys_close(p2[0]);
        sys_close(p2[1]);
    }

    // --- 6. poll() on pipe EOF -> POLLIN|POLLHUP ---
    {
        int p3[2];
        sys_pipe(p3);
        sys_close(p3[1]);  // close write end -> EOF
        pollfd_t pfd;
        pfd.fd = p3[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        int r = sys_poll(&pfd, 1, 0);
        CHECK(r == 1, "pollselectdemo: FAIL EOF poll\n");
        CHECK((pfd.revents & POLLIN) != 0, "pollselectdemo: FAIL EOF POLLIN\n");
        CHECK((pfd.revents & POLLHUP) != 0, "pollselectdemo: FAIL EOF POLLHUP\n");
        sys_close(p3[0]);
    }

    // --- 7. select() readfds: empty -> 0, written -> 1 ---
    {
        int p4[2];
        sys_pipe(p4);
        uint32_t rf = 1u << p4[0];
        uint32_t wf = 0;
        int r = sys_select(p4[0] + 1, &rf, &wf, 0, 0);
        CHECK(r == 0 && rf == 0, "pollselectdemo: FAIL select empty\n");
        char w = 'Y';
        sys_write(p4[1], &w, 1);
        rf = 1u << p4[0];
        r = sys_select(p4[0] + 1, &rf, &wf, 0, 0);
        CHECK(r == 1 && (rf & (1u << p4[0])) != 0,
              "pollselectdemo: FAIL select readable\n");
        sys_close(p4[0]);
        sys_close(p4[1]);
    }

    // --- 8. select() with a real timeout on nothing readable -> 0 ---
    {
        int p5[2];
        sys_pipe(p5);
        uint32_t rf = 1u << p5[0];
        int r = sys_select(p5[0] + 1, &rf, 0, 0, 50);
        CHECK(r == 0 && rf == 0, "pollselectdemo: FAIL select timeout\n");
        sys_close(p5[0]);
        sys_close(p5[1]);
    }

    // --- 9/10. getcwd()/chdir() round-trip ---
    {
        char buf[256];
        CHECK(sys_chdir("/") == 0, "pollselectdemo: FAIL chdir root\n");
        CHECK(sys_getcwd(buf, sizeof(buf)) == 0, "pollselectdemo: FAIL getcwd\n");
        CHECK(slen(buf) == 1 && buf[0] == '/', "pollselectdemo: FAIL cwd root\n");

        CHECK(sys_chdir("/apps") == 0, "pollselectdemo: FAIL chdir apps\n");
        CHECK(sys_getcwd(buf, sizeof(buf)) == 0, "pollselectdemo: FAIL getcwd2\n");
        CHECK(slen(buf) == 5 && buf[1] == 'a' && buf[2] == 'p' &&
              buf[3] == 'p' && buf[4] == 's',
              "pollselectdemo: FAIL cwd apps\n");

        CHECK(sys_chdir("..") == 0, "pollselectdemo: FAIL chdir dotdot\n");
        CHECK(sys_getcwd(buf, sizeof(buf)) == 0, "pollselectdemo: FAIL getcwd3\n");
        CHECK(buf[0] == '/', "pollselectdemo: FAIL cwd after dotdot\n");

        // chdir to a plain file must fail
        CHECK(sys_chdir("/apps/pollselectdemo.mct") == -1,
              "pollselectdemo: FAIL chdir to file\n");
        // chdir to a missing path must fail
        CHECK(sys_chdir("/no/such/dir") == -1,
              "pollselectdemo: FAIL chdir missing\n");
        CHECK(sys_chdir("/") == 0, "pollselectdemo: FAIL chdir root2\n");
    }

    // --- 11. clock_gettime() monotonic + bad clock id ---
    {
        timespec_t t1, t2;
        CHECK(sys_clock_gettime(CLOCK_MONOTONIC, &t1) == 0,
              "pollselectdemo: FAIL clock_gettime\n");
        CHECK(sys_clock_gettime(CLOCK_MONOTONIC, &t2) == 0,
              "pollselectdemo: FAIL clock_gettime2\n");
        CHECK((int32_t)(t2.tv_sec - t1.tv_sec) > 0 ||
              (t2.tv_sec == t1.tv_sec && t2.tv_nsec >= t1.tv_nsec),
              "pollselectdemo: FAIL clock monotonic\n");
        timespec_t t3;
        CHECK(sys_clock_gettime(999, &t3) == -1,
              "pollselectdemo: FAIL clock bad id\n");
    }

    // clean up the temp pipes opened earlier that are still alive
    if (pipefd[1] >= 0) sys_close(pipefd[1]);

    if (fails == 0) {
        sys_print("pollselectdemo: ALL TESTS PASSED\n", 0x0A);
        sys_exit_with_code(0);
    } else {
        sys_print("pollselectdemo: SOME TESTS FAILED\n", 0x0C);
        sys_exit_with_code(1);
    }
}
