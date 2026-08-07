// piperead.c — pipeline consumer: reads fd 0 (stdin) until EOF and echoes the
// bytes to the serial log. Used to prove the real fork/exec pipeline:
//   run /apps/pipegen.mct | run /apps/piperead.mct
#include "src/include/syscall.h"

static void log_str(const char* s) {
    int i = 0;
    while (s[i]) { sys_write(2, s + i, 1); i++; }  // fd 2 -> serial fallback
}

void _start(void) {
    log_str("PIPEREAD: reading stdin...\n");

    char buf[64];
    int total = 0;
    for (;;) {
        int n = sys_read(0, buf, 63);
        if (n <= 0) break;   // EOF (writer closed) or error
        buf[n] = '\0';
        log_str("PIPEREAD got: ");
        log_str(buf);
        log_str("\n");
        total += n;
    }

    char done[32];
    const char* pre = "PIPEREAD: EOF after ";
    int i = 0;
    while (pre[i] && i < 20) { done[i] = pre[i]; i++; }
    if (total >= 10) done[i++] = '0' + (total / 10);
    done[i++] = '0' + (total % 10);
    done[i++] = '\n';
    done[i] = '\0';
    log_str(done);

    sys_exit();
    for (;;) ;
}
