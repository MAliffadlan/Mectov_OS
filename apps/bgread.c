// bgread.c — Fase 2 process-group demo.
// The app is launched foreground by the shell (pgrp = own tid, fg group).
// It forks a child which calls setpgid(0,0) to move into its OWN background
// group, then tries to read the terminal (SYS_GET_KEY). The kernel must
// deliver SIGTTIN (default action: stop) instead of handing the key over.
// The parent waits, resumes the child with SIGCONT, and reaps it with a
// non-zero status to prove it survived.
#include "src/include/syscall.h"

#define SIGTTIN 21
#define SIGCONT 18

// SMP-safe logging: each line is ONE sys_write(2,...) so multi-CPU kernel
// logs cannot interleave bytes into the middle of a marker the tests grep for.
static void log_line(const char* fmt, int a, int b) {
    char buf[96];
    int i = 0;
    while (fmt[i] && i < 90) { buf[i] = fmt[i]; i++; }
    // '%d' placeholders (a then b; extra args ignored by the format)
    char tmp[12]; int t = 0;
    char out[96]; int o = 0;
    int arg = 0;
    for (int j = 0; j < i && o < 90; j++) {
        if (buf[j] == '%' && buf[j+1] == 'd') {
            int n = (arg == 0) ? a : b;
            arg++; t = 0;
            if (n == 0) tmp[t++] = '0';
            while (n > 0 && t < 10) { tmp[t++] = '0' + (n % 10); n /= 10; }
            while (t > 0) out[o++] = tmp[--t];
            j++;
        } else {
            out[o++] = buf[j];
        }
    }
    out[o++] = '\n';
    sys_write(2, out, o);
}

void _start() {
    int me = sys_getpid();
    int mygrp = sys_getpgrp();
    log_line("[BGREAD] starting, pid=%d pgrp=%d", me, mygrp);

    int child = sys_fork();
    if (child < 0) { log_line("[BGREAD] FAIL fork", 0, 0); sys_exit(); }

    if (child == 0) {
        // ---- child: move to our own background group, then read the tty ----
        int sid = sys_setpgid(0, 0);
        log_line("[BGREAD] child setpgid=%d new pgrp=%d", sid, sys_getpgrp());

        // Reading the terminal as a background group must stop us with SIGTTIN
        // (default action) — the single sys_get_key() below blocks us at the
        // first read. SIGCONT (from the parent) resumes us right here; we then
        // exit so the parent can reap us.
        int got = sys_get_key();   // 0 = no key (SIGTTIN stopped us first)
        log_line("[BGREAD] child resumed after SIGTTIN (got=%d)", got, 0);
        sys_exit();
        for (;;) ;
    }

    // ---- parent: let the child hit SIGTTIN, then resume it ----
    sys_sleep(40);
    log_line("[BGREAD] parent sending SIGCONT to child", 0, 0);
    int k = sys_kill(child, SIGCONT);
    log_line("[BGREAD] kill(SIGCONT)=%d", k, 0);

    int status = 0;
    int r = sys_waitpid(child, &status, 0);
    log_line("[BGREAD] waitpid=%d status=%d", r, status);
    log_line("[BGREAD] DONE", 0, 0);
    sys_exit();
    for (;;) ;
}
