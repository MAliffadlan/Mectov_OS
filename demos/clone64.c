/* M4 demo 3: CLONE — one parent spawns two workers sharing the address
 * space; all three print, yield, and exit independently (no waitpid in M4:
 * the parent just yields enough, workers outlive it freely). */
#include "sys64.h"

static void worker(void) {
    DLINE(l);
    long pid = d_pid();
    for (int i = 0; i < 3; i++) {
        dl_s(&l, "WORKER pid=");
        dl_u(&l, (u64)pid);
        dl_s(&l, " i=");
        dl_u(&l, (u64)i);
        dl_nl(&l);
        d_yield();
    }
    dl_s(&l, "WORKER-DONE pid=");
    dl_u(&l, (u64)pid);
    dl_nl(&l);
}

void demo_main(void) {
    DLINE(l);
    long pid = d_pid();
    dl_s(&l, "CLONE parent pid=");
    dl_u(&l, (u64)pid);
    dl_nl(&l);
    long c1 = sys1(104, (u64)worker);
    long c2 = sys1(104, (u64)worker);
    dl_s(&l, "CLONE kids ");
    dl_u(&l, (u64)c1);
    dl_s(&l, " ");
    dl_u(&l, (u64)c2);
    dl_nl(&l);
    for (int i = 0; i < 30; i++) d_yield();
    dl_s(&l, "CLONE-DONE");
    dl_nl(&l);
}
