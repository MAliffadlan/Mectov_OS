// mmapnx.c — W^X regression for mmap'd pages (v38.164).
//
// Sibling of nxtest.c, which proves the same invariant for the user stack.
// nxtest dies on its own first probe, so the mmap case needs its own task.
// Every anonymous mmap page is demand-filled with PAGE_NX once EFER.NXE is
// on, so executing data out of an mmap region must fault: the app writes a
// single `ret` opcode into a page it got from sys_mmap and calls it.
// With NX working the task dies with SIGSEGV (exit 139 = 128+11); if the
// call returns instead, NX is NOT active and the app logs MMAPNX FAIL.
//
// Run:  run /apps/mmapnx.mct     (headless; logs to serial via fd 2)
#include "src/include/syscall.h"

static void wlog(const char* s) {
    int n = 0;
    while (s[n]) n++;
    syscall(SYS_WRITE, 2, (int)(uintptr_t)s, n);
}

// Returns 0 if the mmap'd code executed (NX is NOT active), -1 if the
// reservation failed. Must never return 0 once NX is working.
__attribute__((noinline))
static int call_mmap_code(void) {
    volatile unsigned char* page = (volatile unsigned char*)sys_mmap(4096);
    if ((uint32_t)(uintptr_t)page == 0) return -1;

    // A `ret` (0xC3) in the first bytes, NOP-padded. The stores go through a
    // volatile pointer so the compiler cannot elide them or fold the target
    // into a constant — the call has to go through the mapping.
    for (int i = 0; i < 16; i++) page[i] = (i == 0) ? 0xC3 : 0x90;

    void (*f)(void) = (void (*)(void))(uintptr_t)page;
    f();
    return 0;
}

void _start(void) {
    wlog("MMAPNX start\n");
    int r = call_mmap_code();
    if (r != 0) {
        wlog("MMAPNX FAIL: sys_mmap returned no page\n");
        sys_exit();
        for (;;) ;
    }
    wlog("MMAPNX FAIL: mmap code executed - NX is not active\n");
    sys_exit();
    for (;;) ;
}
