// nxtest.c — W^X regression (v38.49). The user stack (and heap) pages are
// demand-filled with PAGE_NX once EFER.NXE is on, so executing data must
// fault: the app places a single `ret` opcode on its stack and calls it.
// With NX working the task dies with SIGSEGV (exit 139 = 128+11); if the
// call returns instead, NX is NOT active and the app logs NXTEST FAIL.
//
// Run:  run /apps/nxtest.mct     (headless; logs to serial via fd 2)
#include "src/include/syscall.h"

static void wlog(const char* s) {
    int n = 0;
    while (s[n]) n++;
    syscall(SYS_WRITE, 2, (int)(uintptr_t)s, n);
}

__attribute__((noinline))
static int call_stack_code(void) {
    // A `ret` (0xC3) on the local (stack) frame. volatile so the compiler
    // cannot promote the array to a register or constant-fold the call.
    volatile unsigned char code[16];
    for (int i = 0; i < 16; i++) code[i] = (i == 0) ? 0xC3 : 0x90;
    __asm__ __volatile__("" : : "m"(code));   // force it onto the stack slot
    void (*f)(void) = (void (*)(void))code;
    f();
    return 0;
}

void _start(void) {
    wlog("NXTEST start\n");
    call_stack_code();   // must never return
    wlog("NXTEST FAIL: stack code executed - NX is not active\n");
    sys_exit();
    for (;;) ;
}
