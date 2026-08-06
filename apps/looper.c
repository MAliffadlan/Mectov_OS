// looper.c — infinite loop app to test Ctrl+C job interruption.
// Run it:  run /apps/looper.mct   then press Ctrl+C — SIGINT should
// terminate the foreground job (and its children, if any) while the
// terminal + OS stay alive.
#include "src/include/syscall.h"

void _start(void) {
    sys_print("LOOPER: running forever... press Ctrl+C\\n", 0x0E);
    unsigned long i = 0;
    for (;;) {
        i++;
        if ((i % 5000000) == 0) {
            // Occasionally announce we're still alive (rate-limited so the
            // terminal output stays readable).
            sys_print("LOOPER: still looping\\n", 0x08);
            i = 0;
        }
    }
}
