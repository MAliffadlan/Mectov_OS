/* M7.4 smptest: prove tasks really execute on APs. Clones 3 workers that
 * burn ~2s each printing their cpu (SYS_GETCPU); the parent waits them
 * all. The gate requires cpu=1,2,3 each to appear (with 4 CPUs × 100Hz
 * timers and 2s burns, every core provably schedules workers). */
#include "sys64.h"

static void worker(void) {
    long pid = d_pid();
    /* Print cpu repeatedly: the first pick usually lands on the BSP (it
     * schedules before woken APs take their first tick), but over a 2s
     * burn every core provably runs us (migration + AP timers). */
    long t0 = d_ticks();
    long last = -1;
    volatile unsigned long long acc = 0;
    while (d_ticks() - t0 < 200) {
        long c = d_getcpu();
        if (c != last) {
            last = c;
            DLINE(l);
            dl_s(&l, "CPU-WORKER pid=");
            dl_u(&l, (u64)pid);
            dl_s(&l, " cpu=");
            dl_u(&l, (u64)c);
            dl_nl(&l);
        }
        for (int i = 0; i < 1000; i++) acc += (unsigned long long)i;
        d_yield();
    }
    DLINE(l2);
    dl_s(&l2, "CPU-DONE pid=");
    dl_u(&l2, (u64)pid);
    dl_s(&l2, " acc=");
    dl_u(&l2, acc);
    dl_nl(&l2);
}

void demo_main(void) {
    long c1 = sys1(104, (u64)worker);
    long c2 = sys1(104, (u64)worker);
    long c3 = sys1(104, (u64)worker);
    if (c1 < 0 || c2 < 0 || c3 < 0) {
        DLINE(l);
        dl_s(&l, "SMP-CLONE-FAIL");
        dl_nl(&l);
        return;
    }
    long st = 0;
    d_wait(c1, &st, 0);
    d_wait(c2, &st, 0);
    d_wait(c3, &st, 0);
    DLINE(l);
    dl_s(&l, "SMP-DONE");
    dl_nl(&l);
}
