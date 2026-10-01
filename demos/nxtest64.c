/* M7.3 nxtest: W^X proof. Forks children that (1) execute .data and
 * (2) write .text; each must die with 139 (SIGSEGV-equivalent). The parent
 * verifies both statuses (its own pages stay intact to report). */
#include "sys64.h"

static unsigned char payload[] = { 0xC3 }; /* ret */

/* Noinline + indirect call so GCC keeps the body (it otherwise inlines the
 * ret away and drops the symbol, breaking the RO write below). */
static void __attribute__((noinline)) volexec(void) {
    __asm__ volatile("call *%0" :: "r"((void *)payload) : "memory");
}

void demo_main(void) {
    DLINE(l);
    long c1 = sys0(71);
    if (c1 < 0) {
        dl_s(&l, "NX-FORK-FAIL");
        dl_nl(&l);
        return;
    }
    if (c1 == 0) {
        volexec(); /* NX fetch from .data -> killed 139, no return */
        dl_s(&l, "NX-SURVIVED");
        dl_nl(&l);
        return;
    }
    long st1 = 0;
    d_wait(c1, &st1, 0);
    dl_s(&l, (u64)st1 == 139 ? "NX-OK" : "NX-BADSTATUS");
    dl_nl(&l);

    long c2 = sys0(71);
    if (c2 < 0) {
        dl_s(&l, "RO-FORK-FAIL");
        dl_nl(&l);
        return;
    }
    if (c2 == 0) {
        /* Write our own code page (RX) -> RO kill, no return. */
        *(volatile unsigned char *)volexec = 0x90;
        dl_s(&l, "RO-SURVIVED");
        dl_nl(&l);
        return;
    }
    long st2 = 0;
    d_wait(c2, &st2, 0);
    dl_s(&l, (u64)st2 == 139 ? "RO-OK" : "RO-BADSTATUS");
    dl_nl(&l);
    DLINE(l2);
    dl_s(&l2, "NX-DONE");
    dl_nl(&l2);
}
