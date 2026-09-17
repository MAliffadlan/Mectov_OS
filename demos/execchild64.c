/* M5 exec target: built as ELF64 PIE (not MCT2), so exec'ing it proves the
 * in-kernel ELF64 loader from a live process. Keeps its parent's pid. */
#include "sys64.h"

void demo_main(void) {
    DLINE(l);
    dl_s(&l, "EXECCHILD-RAN pid=");
    dl_u(&l, (u64)d_pid());
    dl_s(&l, " base=");
    dl_u(&l, (u64)d_getbase());
    dl_nl(&l);
}
