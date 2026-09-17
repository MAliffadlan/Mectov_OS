/* M4 demo 1: basic Ring-3 life — print, yield, exit. */
#include "sys64.h"

void demo_main(void) {
    DLINE(l);
    long pid = d_pid();
    for (int i = 1; i <= 5; i++) {
        dl_s(&l, "HELLO i=");
        dl_u(&l, (u64)i);
        dl_s(&l, " pid=");
        dl_u(&l, (u64)pid);
        dl_nl(&l);
        d_yield();
    }
    dl_s(&l, "HELLO-DONE pid=");
    dl_u(&l, (u64)pid);
    dl_nl(&l);
}
