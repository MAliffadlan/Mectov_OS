/* M7.2 argdemo: proves argv delivery (spawned by the shell or tests). */
#include "sys64.h"

void args_main(int argc, const char **argv) {
    DLINE(l);
    dl_s(&l, "ARGC=");
    dl_u(&l, (u64)argc);
    dl_nl(&l);
    for (int i = 0; i < argc && i < 16; i++) {
        dl_s(&l, "ARGV");
        dl_u(&l, (u64)i);
        dl_s(&l, "=");
        dl_s(&l, argv[i] ? argv[i] : "(null)");
        dl_nl(&l);
    }
    dl_s(&l, "ARG-DONE");
    dl_nl(&l);
}

/* Satisfy the normal _start (spawn without args is refused... actually it
 * just runs with argc=0 through _start_args? No: plain spawn enters at
 * _start -> demo_main. Provide it too (prints usage). */
void demo_main(void) {
    DLINE(l);
    dl_s(&l, "argdemo: use `run argdemo ...` for argv");
    dl_nl(&l);
}
