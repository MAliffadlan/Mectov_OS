/* M5 demo 4: FORK + COW + waitpid + sleep.
 *
 * A .data word is COW-shared after fork: parent and child each write a
 * distinct pattern and must read back their OWN value (isolation proof).
 * The child sleeps (blocking-sleep proof), the parent waitpids (blocking-
 * wait + zombie-reap proof, incl. exit status).
 */
#include "sys64.h"

static volatile unsigned long cowvar = 0xAAAABBBBCCCCDDDDUL;

void demo_main(void) {
    DLINE(l);
    long pid = d_pid();
    dl_s(&l, "FORK parent pid=");
    dl_u(&l, (u64)pid);
    dl_s(&l, " shared=");
    dl_u(&l, cowvar);
    dl_nl(&l);
    long c = sys0(71);
    if (c < 0) {
        dl_s(&l, "FORK-FAIL");
        dl_nl(&l);
        return;
    }
    if (c == 0) {
        long cp = d_pid();
        dl_s(&l, "FORK child pid=");
        dl_u(&l, (u64)cp);
        dl_s(&l, " shared=");
        dl_u(&l, cowvar);
        dl_nl(&l);
        cowvar = 0x1111111111111111UL;
        d_sleep(50);
        dl_s(&l, "FORK child reread ");
        dl_u(&l, cowvar);
        dl_nl(&l);
        dl_s(&l, "FORK-CHILD-DONE");
        dl_nl(&l);
        return;
    }
    dl_s(&l, "FORK kid ");
    dl_u(&l, (u64)c);
    dl_nl(&l);
    cowvar = 0x2222222222222222UL;
    long st = 0;
    long w = d_wait(c, &st, 0);
    dl_s(&l, "FORK parent reread ");
    dl_u(&l, cowvar);
    dl_s(&l, " reaped=");
    dl_u(&l, (u64)w);
    dl_s(&l, " status=");
    dl_u(&l, (u64)st);
    dl_nl(&l);
    dl_s(&l, "FORK-DONE");
    dl_nl(&l);
}
