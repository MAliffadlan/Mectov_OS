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
    int me = sys_getpid();
    int mygrp = sys_getpgrp();
    log_str("[BGREAD] starting, pid=");
    log_num(me);
    log_str(" pgrp=");
    log_num(mygrp);
    log_str("\n");

    int child = sys_fork();
    if (child < 0) { log_str("[BGREAD] FAIL fork\n"); sys_exit(); }

    if (child == 0) {
        // ---- child: move to our own background group, then read the tty ----
        int sid = sys_setpgid(0, 0);
        log_str("[BGREAD] child setpgid=");
        log_num(sid);
        log_str(" new pgrp=");
        log_num(sys_getpgrp());
        log_str("\n");

        // Reading the terminal as a background group must stop us with SIGTTIN
        // (default action) — the single sys_get_key() below blocks us at the
        // first read. SIGCONT (from the parent) resumes us right here; we then
        // exit so the parent can reap us.
        int got = sys_get_key();   // 0 = no key (SIGTTIN stopped us first)
        log_str("[BGREAD] child resumed after SIGTTIN (got=");
        log_num(got);
        log_str(")\n");
        sys_exit();
        for (;;) ;
    }

    // ---- parent: let the child hit SIGTTIN, then resume it ----
    sys_sleep(40);
    log_str("[BGREAD] parent sending SIGCONT to child\n");
    int k = sys_kill(child, SIGCONT);
    log_str("[BGREAD] kill(SIGCONT)=");
    log_num(k);
    log_str("\n");

    int status = 0;
    int r = sys_waitpid(child, &status, 0);
    log_str("[BGREAD] waitpid=");
    log_num(r);
    log_str(" status=");
    log_num(status);
    log_str("\n");
    log_str("[BGREAD] DONE\n");
    sys_exit();
    for (;;) ;
}
