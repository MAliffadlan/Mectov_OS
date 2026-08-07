// tcpserver.c - minimal TCP echo server demo (tests SYS_TCP_LISTEN + accept)
#include "src/include/syscall.h"

// fd 2 routes to the serial log, so a headless test can verify server state.
static void log_str(const char* s) {
    int i = 0;
    while (s[i]) { sys_write(2, s + i, 1); i++; }
}
static void log_num(int n) {
    char b[12]; int i = 0;
    if (n == 0) b[i++] = '0';
    while (n > 0 && i < 11) { b[i++] = '0' + (n % 10); n /= 10; }
    for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i-1-j]; b[i-1-j] = t; }
    b[i] = '\n'; b[i+1] = '\0';
    log_str(b);
}

void _start() {
    log_str("[TCPSRV] echo server starting, listen 8080\n");
    int id = sys_tcp_listen(8080);
    if (id < 0) {
        log_str("[TCPSRV] listen failed id=");
        log_num(id);
        sys_exit();
    }
    log_str("[TCPSRV] listen ok id=");
    log_num(id);

    // Wait for the connection to become established (accept happens in-kernel
    // on the final handshake ACK); poll until data is available.
    char buf[128];
    int echoed = 0;
    for (int iter = 0; iter < 600; iter++) {  // ~30s budget
        int r = sys_tcp_recv(id, buf, sizeof(buf) - 1);
        if (r > 0) {
            buf[r] = '\0';
            log_str("[TCPSRV] recv: ");
            log_str(buf);
            log_str("\n");
            int s = sys_tcp_send(id, buf, r);
            log_str("[TCPSRV] echo sent=");
            log_num(s);
            log_str(" len=");
            log_num(r);
            echoed = 1;
            break;
        }
        if (r < 0 && r != -2) {   // -2 = not established yet, keep waiting
            log_str("[TCPSRV] recv error=");
            log_num(r);
            break;
        }
        sys_sleep(50);
    }

    if (echoed) {
        log_str("[TCPSRV] echo test DONE\n");
    } else {
        log_str("[TCPSRV] no data received, exiting\n");
    }
    sys_tcp_close(id);
    sys_exit();
    for (;;) ;
}
