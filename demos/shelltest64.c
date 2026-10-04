/* M7.2 shelltest: spawn_args mechanics without keyboard timing — spawns
 * argdemo with argv, waits, verifies reap id + status. The interactive
 * shell reuses this exact path (proven separately via sendkey). */
#include "sys64.h"

void demo_main(void) {
    long pid = d_pid();
    DLINE(l);
    dl_s(&l, "SHELLTEST pid=");
    dl_u(&l, (u64)pid);
    dl_nl(&l);
    const char *av[2] = { "alpha", "beta" };
    long id = d_spawn("argdemo", 2, av);
    if (id < 0) {
        dl_s(&l, "SHELLTEST-SPAWN-FAIL ");
        dl_u(&l, (u64)(-id));
        dl_nl(&l);
        return;
    }
    long st = 0;
    long w = d_wait(id, &st, 0);
    dl_s(&l, "SHELLTEST reaped=");
    dl_u(&l, (u64)w);
    dl_s(&l, " status=");
    dl_u(&l, (u64)st);
    dl_nl(&l);
    if (w == id && st == 0) {
        dl_s(&l, "SHELLTEST-DONE");
        dl_nl(&l);
    } else {
        dl_s(&l, "SHELLTEST-MISMATCH");
        dl_nl(&l);
    }
}
