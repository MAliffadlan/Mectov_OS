/* M7.3 asldemo: ASLR proof. Forks; parent and child each exec the ELF
 * "execchild", which prints its own load BASE (SYS_GETBASE). The two BASE
 * lines must differ (per-exec sequence guarantees it; the PRNG offset
 * randomizes the absolute position per boot). */
#include "sys64.h"

void demo_main(void) {
    long c = sys0(71);
    if (c < 0) {
        DLINE(l);
        dl_s(&l, "ASLR-FORK-FAIL");
        dl_nl(&l);
        return;
    }
    /* Both sides replace themselves; whoever returns failed. */
    d_exec("execchild");
    DLINE(l);
    dl_s(&l, "ASLR-EXEC-FAIL");
    dl_nl(&l);
}
