/* M5 demo 5: EXEC — replace self with the embedded "execchild" image.
 * EXEC-POST must NEVER print; the child image (ELF64) proves the ELF loader
 * end to end from inside a live process.
 */
#include "sys64.h"

void demo_main(void) {
    DLINE(l);
    dl_s(&l, "EXEC-PRE pid=");
    dl_u(&l, (u64)d_pid());
    dl_nl(&l);
    long r = sys1(76, (u64)"execchild");
    dl_s(&l, "EXEC-FAIL r=");
    if (r < 0) {
        dl_s(&l, "-");
        dl_u(&l, (u64)(-r));
    } else {
        dl_u(&l, (u64)r);
    }
    dl_nl(&l);
    dl_s(&l, "EXEC-POST");
    dl_nl(&l);
}
