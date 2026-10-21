// PTY round-trip test (v38.168). Exercises the pseudo-terminal syscall pair:
//   sys_pty_alloc() -> master fd
//   sys_pty_open(master) -> slave fd
// then verifies both directions of the ring:
//   master write -> slave read   (terminal keystrokes reaching the shell)
//   slave write  -> master read  (shell output returning to the terminal)
#include "src/include/syscall.h"

static void out(const char* s) { sys_print(s, 0x0B); }
static void fail(const char* s) {
    sys_print("PTY TEST FAIL: ", 0x0C);
    sys_print(s, 0x0C);
    sys_print("\n", 0x0C);
    sys_exit();
}

void _start(void) {
    out("[PTY] pty_test starting\n");
    out("[PTY] allocating master...\n");
    int master = sys_pty_alloc();
    if (master < 0) fail("sys_pty_alloc returned < 0 (table full / fd full)");
    {
        char c[2] = { '0' + (master % 10), '\0' };
        out("[PTY] master fd = ");
        sys_print(c, 0x0B);
        out("\n");
    }

    out("[PTY] opening slave...\n");
    int slave = sys_pty_open(master);
    if (slave < 0) fail("sys_pty_open returned < 0");
    {
        char c[2] = { '0' + (slave % 10), '\0' };
        out("[PTY] slave fd = ");
        sys_print(c, 0x0B);
        out("\n");
    }

    // Direction 1: master -> slave ("shell receives a keystroke").
    out("[PTY] master writes \"hi\" -> slave should read it\n");
    int w = sys_write(master, "hi", 2);
    if (w != 2) fail("master write did not return 2");
    char buf[8];
    int r = sys_read(slave, buf, sizeof(buf));
    if (r != 2 || buf[0] != 'h' || buf[1] != 'i') {
        buf[r < 0 ? 0 : r] = '\0';
        out("[PTY] slave read back: '");
        sys_print(r < 0 ? "(err)" : buf, 0x0E);
        out("'\n");
        fail("master->slave round trip mismatch");
    }
    out("[PTY]   OK: slave read \"hi\"\n");

    // Direction 2: slave -> master ("shell echoes output to the terminal").
    out("[PTY] slave writes \"ok\" -> master should read it\n");
    w = sys_write(slave, "ok", 2);
    if (w != 2) fail("slave write did not return 2");
    r = sys_read(master, buf, sizeof(buf));
    if (r != 2 || buf[0] != 'o' || buf[1] != 'k') {
        fail("slave->master round trip mismatch");
    }
    out("[PTY]   OK: master read \"ok\"\n");

    // Both rings independently drained once. Verify empty now.
    r = sys_read(slave, buf, sizeof(buf));
    if (r != 0) fail("slave ring not empty after drain (expected 0 bytes)");
    r = sys_read(master, buf, sizeof(buf));
    if (r != 0) fail("master ring not empty after drain (expected 0 bytes)");

    // Close both ends; the pty should be reclaimed (no leak on re-alloc).
    sys_close(slave);
    sys_close(master);

    out("[PTY] allocating a second pty to confirm slot reuse...\n");
    int m2 = sys_pty_alloc();
    if (m2 < 0) fail("second sys_pty_alloc failed (slot not reclaimed after close)");
    {
        char c[2] = { '0' + (m2 % 10), '\0' };
        out("[PTY] second master fd = ");
        sys_print(c, 0x0B);
        out("\n");
    }
    sys_close(m2);

    out("[PTY] ALL PTY TESTS PASSED\n");
    sys_exit();
}
