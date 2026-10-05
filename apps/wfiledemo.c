// wfiledemo — reports the RESULT of the write syscalls themselves (v38.159).
//
// Every other file test in this tree verifies a write by reading the data back.
// That check cannot see a dropped write error: the block cache and the page
// cache happily serve the bytes the kernel just failed to store, and the demo
// reports success while the medium never saw them. This app exists to make the
// *syscall return value* observable, so a medium that refuses a write is
// reported as a failure instead of a success.
//
// Pair it with scripts/wfail_test.py --mode inject (QEMU blkdebug injecting
// write_aio errors onto the FAT32 image): that boot must never print a success
// marker here.
//
// Markers, one per line on the serial console:
//   WFILE create ok   / WFILE create FAILED
//   WFILE write ok    / WFILE write FAILED   / WFILE write SHORT
//   WFILE readback ok / WFILE readback FAILED
//   WFILE done
#include "src/include/syscall.h"

static int fails = 0;

static int mem_eq(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

void _start(void) {
    const char* path = "/fat32/wfail_out.txt";
    const int nbytes = 512;

    sys_print("wfiledemo: write-result test\n", 0x0F);

    sys_delete_file(path);
    if (sys_create_file(path) < 0 && sys_stat_file(path) < 0) {
        sys_print("WFILE create FAILED\n", 0x0C);
        fails++;
        sys_print("WFILE done\n", 0x0F);
        sys_exit_with_code(1);
    }
    sys_print("WFILE create ok\n", 0x0A);

    int fd = sys_open(path);
    if (fd < 0) {
        sys_print("WFILE open FAILED\n", 0x0C);
        fails++;
        sys_print("WFILE done\n", 0x0F);
        sys_exit_with_code(1);
    }

    // A deterministic payload; the content itself does not matter, only
    // whether the kernel admits the write failed.
    char data[512];
    for (int i = 0; i < nbytes; i++) data[i] = (char)('A' + (i % 26));

    int n = sys_write(fd, data, nbytes);
    if (n == nbytes) {
        sys_print("WFILE write ok\n", 0x0A);
    } else if (n < 0) {
        sys_print("WFILE write FAILED\n", 0x0C);
        fails++;
    } else {
        // A short write is a failure for a fixed-size buffer too: the caller
        // asked for 512 bytes and the file did not get them.
        sys_print("WFILE write SHORT\n", 0x0C);
        fails++;
    }
    sys_close(fd);

    // Read back through a fresh descriptor: on a working medium this must match
    // byte for byte; with a refused write it must NOT claim a match.
    char back[512];
    fd = sys_open(path);
    if (fd < 0) {
        sys_print("WFILE readback FAILED\n", 0x0C);
        fails++;
    } else {
        int got = sys_read(fd, back, nbytes);
        sys_close(fd);
        if (got == nbytes && mem_eq(back, data, nbytes)) {
            sys_print("WFILE readback ok\n", 0x0A);
        } else {
            sys_print("WFILE readback FAILED\n", 0x0C);
            fails++;
        }
    }

    sys_print("WFILE done\n", 0x0F);
    sys_exit_with_code(fails ? 1 : 0);
}
