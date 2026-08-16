// fputest.c — FPU/SSE context-switch regression (v38.41)
// Two processes accumulate in the live x87 register stack AND an SSE
// register across preemptions (sys_yield) and across fork(). Before the
// scheduler swapped the fxsave image eagerly, whichever task ran second
// on a core clobbered the other's live state and the sums came out wrong.
//
// Apps are built -msoft-float -mno-80387, so every FPU op below is inline
// asm — the compiler never touches those registers, only the test does.
//
// Run:  run /apps/fputest.mct     (headless; logs to serial via fd 2)
#include "src/include/syscall.h"

// The accumulator lives in st0 across statements AND syscalls; the SSE
// counter lives in xmm0 (each 32-bit lane += xmm1's lane, set to 1s).
#define X87_PRE()    __asm__ __volatile__("fldz")
#define X87_ADD(k)   __asm__ __volatile__("fildq %0\n\tfaddp" : : "m"(k))
#define X87_GET(out) __asm__ __volatile__("fistpq %0" : "=m"(out))
#define SSE_PRE()    __asm__ __volatile__( \
        "movl $1, %%eax\n\t"          \
        "movd %%eax, %%xmm1\n\t"      \
        "pshufd $0, %%xmm1, %%xmm1\n\t" \
        "pxor %%xmm0, %%xmm0" : : : "eax")
// NB: an asm statement WITHOUT operands skips GCC's %% -> % template
// substitution, so SSE_ADD spells its registers with a single %.
#define SSE_ADD()    __asm__ __volatile__("paddd %xmm1, %xmm0")
#define SSE_GET(out) __asm__ __volatile__("movdqu %%xmm0, %0" : "=m"(out))

static void wlog(const char* s) {
    int n = 0;
    while (s[n]) n++;
    syscall(SYS_WRITE, 2, (int)(uintptr_t)s, n);
}

static void whex(uint64_t v) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    static const char d[] = "0123456789ABCDEF";
    for (int i = 0; i < 16; i++)
        buf[2 + i] = d[(v >> (60 - 4 * i)) & 0xF];
    buf[18] = '\0';
    wlog(buf);
}

static void run_loops(int iters, long long addend) {
    for (int i = 0; i < iters; i++) {
        X87_ADD(addend);
        SSE_ADD();
        if ((i & 7) == 0) sys_yield();   // preempt against the sibling task
    }
}

static int verify(const char* who, long long got87, long long exp87,
                  uint32_t lanes[4], int exp_lane) {
    int ok = (got87 == exp87);
    for (int i = 0; i < 4; i++) if (lanes[i] != (uint32_t)exp_lane) ok = 0;
    if (ok) { wlog("FPUTEST "); wlog(who); wlog(" OK\n"); return 1; }
    wlog("FPUTEST "); wlog(who); wlog(" FAIL x87=");
    whex((uint64_t)got87); wlog(" want "); whex((uint64_t)exp87);
    wlog(" lanes="); whex(((uint64_t)lanes[1] << 32) | lanes[0]);
    wlog(" want "); whex((uint64_t)exp_lane); wlog("\n");
    return 0;
}

void _start(void) {
    wlog("FPUTEST start\n");

    X87_PRE();          // st0 = 0
    SSE_PRE();          // xmm0 = {0,0,0,0}, xmm1 = {1,1,1,1}
    run_loops(100, 3);  // st0 = 300, lanes = 100

    int pid = sys_fork();
    if (pid < 0) { wlog("FPUTEST FAIL fork\n"); sys_exit(); }

    if (pid == 0) {
        // CHILD: inherits the parent's FPU image at fork (POSIX) — its
        // st0 starts at 300 and lanes at 100. Keep accumulating.
        run_loops(200, 7);
        long long g87; uint32_t lanes[4];
        X87_GET(g87); SSE_GET(lanes);
        int ok = verify("CHILD", g87, 300 + 200 * 7, lanes, 100 + 200);
        syscall(SYS_EXIT, ok ? 0 : 1, 0, 0);
        for (;;) ;
    }

    // PARENT: continues its own accumulation in parallel with the child.
    run_loops(300, 3);
    long long g87; uint32_t lanes[4];
    X87_GET(g87); SSE_GET(lanes);
    int parent_ok = verify("PARENT", g87, (100 + 300) * 3, lanes, 100 + 300);

    int status = -1;
    sys_waitpid(pid, &status, 0);
    int child_ok = (status == 0);

    if (parent_ok && child_ok) wlog("FPUTEST PASS\n");
    else                       wlog("FPUTEST FAIL\n");
    sys_exit();
}
