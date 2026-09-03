// smpstress.c — Fase 3 SMP scheduler stress test.
//
// Forks N children that mix CPU burns, sleep slices and self-signals (a
// workload that exercises every per-CPU runqueue path: enqueue on create,
// wake from sleep, signal delivery, exit + parent wake), then waitpids them
// all and verifies every exit code. On the 4-core SMP scheduler the busy
// phases overlap, so the whole wave finishes in a fraction of the serial
// time — a hang or a wrong code means a runqueue race.
#include "src/include/syscall.h"

#define SIGUSR1 10

// fd 2 routes to the serial log for headless verification. Each LOG LINE is
// written with a SINGLE sys_write so the whole line lands in one locked serial
// write — on the SMP scheduler, byte-per-byte writes from different cores
// would interleave and garble every marker the test greps for.
static void log_line(const char* s, int num, const char* tail) {
    char buf[80]; int n = 0;
    while (s && s[n] && n < 40) { buf[n] = s[n]; n++; }
    if (num >= 0) {
        char nb[12]; int k = 0;
        if (num == 0) nb[k++] = '0';
        while (num > 0 && k < 11) { nb[k++] = '0' + (num % 10); num /= 10; }
        for (int i = 0; i < k / 2; i++) { char t = nb[i]; nb[i] = nb[k-1-i]; nb[k-1-i] = t; }
        for (int i = 0; i < k && n < 70; i++) buf[n++] = nb[i];
    }
    while (tail && tail[0] && n < 79) { buf[n] = tail[0]; tail++; n++; }
    buf[n++] = '\n';
    sys_write(2, buf, n);
}

static volatile int got_usr1 = 0;
static void on_usr1(int sig) { got_usr1 = 1; }

// MCT convention: apps define _start(), not main() — build_mct.py uses the
// _start symbol as the binary entry point; without it the entry falls back to
// offset 0 (the syscall helper) and the app crashes on its first instruction.
void _start() {
    int n = 8;   // children per wave (the test runs the default)

    uint32_t t0 = sys_get_ticks();
    log_line("[SMPSTRESS] begin n=", n, "");

    for (int i = 1; i <= n; i++) {
        int pid = sys_fork();
        if (pid == 0) {
            // ---- child i ----
            // 1) CPU burn (~1s of core time). The accumulator must be
            //    volatile: with a plain local the loop's result is dead and
            //    -O2 eliminates the whole burn, silently gutting the stress.
            volatile unsigned long v_acc = 0x12345678ul;
            for (int t = 0; t < 300; t++) {
                for (int j = 0; j < 3000; j++) {
                    v_acc = v_acc * 31u + (unsigned long)j + (v_acc >> 17);
                }
            }
            // 2) sleep slices (wake-from-sleep path on the runqueues)
            for (int s = 0; s < 5; s++) sys_sleep(50);
            // 3) even children also round-trip a self-signal
            if ((i % 2) == 0) {
                sigaction_t act; act.handler = (uint32_t)(uintptr_t)&on_usr1; act.mask = 0; act.flags = 0;
                sys_sigaction(SIGUSR1, &act, 0);
                sys_kill(sys_getpid(), SIGUSR1);
                if (!got_usr1) { log_line("[SMPSTRESS] child ", i, " MISSING SIGNAL"); sys_exit_with_code(1); }
            }
            log_line("[SMPSTRESS] child ", i, " done");
            sys_exit_with_code(100 + i);
        }
    }

    // Parent: reap every child and verify its code.
    int seen[25]; for (int i = 0; i <= n; i++) seen[i] = 0;
    int reaped = 0, bad = 0;
    while (reaped < n) {
        int st = 0;
        int r = sys_waitpid(-1, &st, 0);
        if (r < 0) { log_line("[SMPSTRESS] waitpid error", -1, ""); bad = 1; break; }
        int code = st & 0xFF;
        int idx = code - 100;
        if (idx >= 1 && idx <= n) { if (!seen[idx]) reaped++; seen[idx] = 1; }
        else { log_line("[SMPSTRESS] bad exit code ", code, ""); bad = 1; }
    }
    int all_seen = 1;
    for (int i = 1; i <= n; i++) if (!seen[i]) all_seen = 0;

    uint32_t el = sys_get_ticks() - t0;
    log_line("[SMPSTRESS] elapsed_ms=", (int)el, "");
    if (!bad && all_seen) {
        log_line("[SMPSTRESS] ALL PASS", -1, "");
    } else {
        log_line("[SMPSTRESS] FAIL reaped=", reaped, " all_seen");
    }
    sys_exit_with_code(0);
}
