// segvtest.c — verifies synchronous SIGSEGV delivery on an unresolvable
// user #PF (dereferencing NULL).
//
//   run /apps/segvtest.mct          -> no handler: task is killed with exit
//                                      status 128+SIGSEGV (139), OS survives
//   run /apps/segvtest.mct catch    -> handler installed: SIGSEGV is delivered
//                                      to it, the faulting instruction is
//                                      re-executed on return (faulting again),
//                                      the handler counts re-deliveries and
//                                      exits cleanly after 2 catches.
#include "src/include/syscall.h"

#define SIGSEGV 11

static volatile int segv_count = 0;

// fd 2 routes to the serial log for headless verification.
static void log_str(const char* s) {
    int i = 0;
    while (s[i]) { sys_write(2, s + i, 1); i++; }
}

static void segv_handler(int sig) {
    (void)sig;
    segv_count++;
    char b[32];
    int i = 0;
    b[i++] = '#'; b[i++] = '0' + segv_count; b[i++] = '\n'; b[i] = '\0';
    log_str("[SEGVTEST] caught SIGSEGV ");
    log_str(b);
    // The faulting `*p = 1` is re-executed when this returns, so SIGSEGV is
    // re-delivered until we exit. Two catches prove delivery + re-delivery.
    if (segv_count >= 2) {
        log_str("[SEGVTEST] ALL PASSED (2 SIGSEGV deliveries, no kernel panic)\n");
        sys_exit();
    }
}

void _start(void) {
    log_str("[SEGVTEST] starting\n");

    char arg[16];
    int alen = sys_get_launch_arg(arg, sizeof(arg) - 1);
    arg[alen < 0 ? 0 : alen] = '\0';
    int catch_mode = 0;
    if (alen > 0 && arg[0] == 'c') catch_mode = 1;

    if (catch_mode) {
        void* old = sys_signal(SIGSEGV, (void*)&segv_handler);
        if (old != 0) {
            // 0 = previous handler was the default; anything else is wrong.
            log_str("[SEGVTEST] FAIL: sys_signal should have returned the default (0)\n");
            sys_exit();
        }
        log_str("[SEGVTEST] handler installed, dereferencing NULL...\n");
    } else {
        log_str("[SEGVTEST] no handler, dereferencing NULL (expect kill 139)...\n");
    }

    volatile int* p = 0;
    *p = 1;   // #PF, not-present write at 0x0

    // Only reached if catch_mode and the handler somehow fixed the fault.
    log_str("[SEGVTEST] FAIL: survived the NULL dereference\n");
    sys_exit();
    for (;;) ;
}
