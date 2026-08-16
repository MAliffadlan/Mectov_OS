// tcpserver.c — POSIX socket API demo (v38.43, fd-integrated)
// Two phases, exercising the whole socket syscall surface end to end:
//   1. CLIENT: socket/connect to the host echo server (10.0.2.2:9999 via
//      QEMU user networking), write + read the echo back.
//   2. SERVER: socket/bind/listen on :8080, poll for an inbound connection
//      (the host connects through hostfwd), accept it, echo one line back.
// Every step logs to serial (fd 2) so a headless CI test can verify it:
//   "[SOCK] client OK" then "[SOCK] server OK" then exit 0.
#include "src/include/syscall.h"

static void log_str(const char* s) {
    int n = 0;
    while (s[n]) n++;
    syscall(SYS_WRITE, 2, (int)(uintptr_t)s, n);
}

static int str_eq(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void client_phase(void) {
    int fd = sys_socket(AF_INET, SOCK_STREAM);
    if (fd < 0) { log_str("[SOCK] client: socket failed\n"); return; }

    sockaddr_t sa = { AF_INET, 9999, { 10, 0, 2, 2 } };
    if (sys_connect(fd, &sa) < 0) { log_str("[SOCK] client: connect failed\n"); return; }

    // connect() returns when the SYN is out; POLLOUT means established.
    pollfd_t p = { fd, POLLOUT, 0 };
    int est = 0;
    for (int i = 0; i < 100; i++) {
        if (sys_poll(&p, 1, 100) > 0 && (p.revents & POLLOUT)) { est = 1; break; }
    }
    if (!est) { log_str("[SOCK] client: never established\n"); return; }

    const char* msg = "SOCKET HELLO\n";
    if (sys_write(fd, msg, 13) != 13) { log_str("[SOCK] client: write failed\n"); return; }

    char buf[64];
    int got = 0;
    for (int i = 0; i < 100 && got < 13; i++) {
        int r = sys_read(fd, buf + got, 13 - got);
        if (r > 0) got += r;
        else if (r < 0) break;
        else sys_sleep(50);
    }
    if (got == 13 && str_eq(buf, msg, 13)) log_str("[SOCK] client OK\n");
    else log_str("[SOCK] client: echo mismatch\n");
    sys_close(fd);
}

static void server_phase(void) {
    int lfd = sys_socket(AF_INET, SOCK_STREAM);
    if (lfd < 0) { log_str("[SOCK] server: socket failed\n"); return; }

    sockaddr_t sa = { AF_INET, 8080, { 0, 0, 0, 0 } };
    if (sys_bind(lfd, &sa) < 0) { log_str("[SOCK] server: bind failed\n"); return; }
    if (sys_listen(lfd, 4) < 0) { log_str("[SOCK] server: listen failed\n"); return; }
    log_str("[SOCK] server listening on 8080\n");

    // Wait for the host's inbound connection (hostfwd -> :8080).
    int cfd = -1;
    pollfd_t p = { lfd, POLLIN, 0 };
    for (int i = 0; i < 200; i++) {
        cfd = sys_accept(lfd);
        if (cfd >= 0) break;
        sys_poll(&p, 1, 100);
    }
    if (cfd < 0) { log_str("[SOCK] server: no inbound connection\n"); return; }
    log_str("[SOCK] server accepted\n");

    char buf[64];
    int got = 0;
    for (int i = 0; i < 100 && got < 5; i++) {
        int r = sys_read(cfd, buf + got, 5 - got);
        if (r > 0) got += r;
        else if (r < 0) break;
        else sys_sleep(50);
    }
    if (got == 5 && str_eq(buf, "PING\n", 5)) {
        if (sys_write(cfd, "PONG\n", 5) == 5) {
            log_str("[SOCK] server OK\n");
            sys_close(cfd);
            sys_close(lfd);
            return;
        }
    }
    log_str("[SOCK] server: echo failed\n");
    sys_close(cfd);
    sys_close(lfd);
}

void _start() {
    log_str("[SOCK] start\n");
    client_phase();
    server_phase();
    log_str("[SOCK] done\n");
    sys_exit();
    for (;;) ;
}
