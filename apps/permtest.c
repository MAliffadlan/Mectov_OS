// permtest.c — Mectov OS ownership & permission test (Ring 3)
//
// Runs as the logged-in user (uid 1000). Exercises the v38.23 permission
// model end to end through real syscalls:
//
//   1. create + write own file          -> allowed (owner rw)
//   2. chmod 0400                       -> open-for-write denied, read open
//                                          allowed; write via fd refused
//   3. chmod 0000                       -> both opens denied; read/write via
//                                          a pre-existing fd also refused
//   4. chmod back to 0644               -> owner write allowed again
//   5. chown 0 0                        -> denied (only root may chown)
//   6. write to a root-owned file       -> denied (other row has no write)
//   7. delete a root-owned file         -> denied
//   8. read a root-owned 0644 file      -> allowed (other row has read)
//
// A failure anywhere prints [PERTEST] FAIL <step> and exits nonzero.
// Success prints [PERTEST] ALL PASS.
//
// Run it from the terminal:  run /apps/permtest.mct
#include "src/include/syscall.h"

#define S_IRUSR 0x100
#define S_IWUSR 0x080
#define S_IRGRP 0x020
#define S_IROTH 0x004
#define MODE_0644 (S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH)
#define MODE_0400 (S_IRUSR)
#define MODE_0000 0

static char buf[128];

static int fail(const char* step) {
    sys_print("[PERTEST] FAIL ", 0x0C);
    sys_print(step, 0x0C);
    sys_print("\n", 0x0C);
    sys_exit();
    return -1;
}

void _start(void) {
    sys_print("[PERTEST] start\n", 0x0E);
    const char* mine = "/permtest.txt";

    // Make re-runs idempotent: a previous failed run may have left the file
    // behind with mode 0000 (unwritable even by its owner), so restore the
    // write bit first (chmod only needs ownership), then delete.
    sys_chmod(mine, MODE_0644);
    sys_delete_file(mine);

    // 1. Create + write own file: allowed.
    if (sys_create_file(mine) < 0) fail("create-own");
    int fd = sys_open(mine);
    if (fd < 0) fail("open-own");
    if (sys_write(fd, "hello perm\n", 11) != 11) fail("write-own");
    sys_close(fd);

    // 2. chmod 0400: write denied, read allowed. Opening for write is denied
    //    at open time (O_APPEND needs the write bit); opening for read
    //    succeeds; a write through that descriptor is refused per-call.
    if (sys_chmod(mine, MODE_0400) != 0) fail("chmod-0400");
    fd = sys_open_mode(mine, O_APPEND);
    if (fd >= 0) fail("append-open-denied-0400");
    fd = sys_open(mine);
    if (fd < 0) fail("open-0400");
    if (sys_write(fd, "x", 1) != -1) fail("write-denied-0400");
    int r = sys_read(fd, buf, 128);
    if (r < 0) fail("read-allowed-0400");
    sys_close(fd);

    // 3. chmod 0000: both opens denied, and even a descriptor opened before
    //    the chmod refuses read and write (per-call check in the kernel).
    if (sys_chmod(mine, MODE_0000) != 0) fail("chmod-0000");
    fd = sys_open(mine);
    if (fd >= 0) fail("open-denied-0000");
    fd = sys_open_mode(mine, O_APPEND);
    if (fd >= 0) fail("append-open-denied-0000");
    if (sys_chmod(mine, MODE_0644) != 0) fail("chmod-tmp");
    fd = sys_open(mine);
    if (fd < 0) fail("open-0000-fd");
    if (sys_chmod(mine, MODE_0000) != 0) fail("chmod-0000-2");
    if (sys_write(fd, "x", 1) != -1) fail("write-denied-0000");
    if (sys_read(fd, buf, 128) != -1) fail("read-denied-0000");
    sys_close(fd);

    // 4. chmod back to 0644: owner write works again.
    if (sys_chmod(mine, MODE_0644) != 0) fail("chmod-restore");
    fd = sys_open(mine);
    if (fd < 0) fail("open-restore");
    if (sys_write(fd, "again\n", 6) != 6) fail("write-restore");
    sys_close(fd);

    // 5. chown 0 0: denied for a non-root caller (POSIX root-only).
    if (sys_chown(mine, 0, 0) != -1) fail("chown-not-root");

    // 6. Write to a root-owned file: denied (other row has no write bit).
    //    hello.txt is seeded root-owned 0644 on every disk.
    fd = sys_open_mode("/hello.txt", O_APPEND);
    if (fd >= 0) {
        if (sys_write(fd, "x", 1) != -1) fail("write-root-file");
        sys_close(fd);
    } else {
        // open-for-append already denied by the open-time check
    }

    // 7. Delete a root-owned file: denied.
    if (sys_delete_file("/hello.txt") != -1) fail("delete-root-file");

    // 8. Read a root-owned 0644 file: allowed (other row has read).
    fd = sys_open("/hello.txt");
    if (fd < 0) fail("read-root-file");
    r = sys_read(fd, buf, 128);
    if (r < 0) fail("read-root-file-data");
    sys_close(fd);

    // Cleanup own file.
    if (sys_delete_file(mine) != 0) fail("cleanup");

    sys_print("[PERTEST] ALL PASS\n", 0x0E);
    sys_exit();
}
