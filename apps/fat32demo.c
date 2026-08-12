// fat32demo — proves the FAT32 driver end-to-end from Ring 3:
//   1. Read /fat32/HELLO.TXT  (created on the image by mtools, 29 bytes)
//   2. Read /fat32/hello2.txt (lowercase name -> case-insensitive resolve)
//   3. Read /fat32/docs/note.txt (nested subdirectory)
//   4. Read LFN files: "/fat32/The quick brown fox.txt" and the long-named
//      file inside "/fat32/My Vacation Photos" (mtools wrote LFN entries)
//   5. mkdir /fat32/demo + create /fat32/demo/write.txt + write + read back
//   6. O_APPEND append -> size grows, old content preserved at the front
//   7. Create a long-named file from the OS (LFN write) + read back
//   8. Delete the file -> stat says gone
// All checks print to the serial console; "ALL TESTS PASSED" on success.
#include "src/include/syscall.h"

static int fails = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { sys_print(msg, 0x0C); fails++; } \
} while (0)

static int mem_eq(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int slen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int read_whole(const char* path, char* buf, int max) {
    int fd = sys_open(path);
    if (fd < 0) return -1;
    int n = sys_read(fd, buf, max - 1);
    sys_close(fd);
    if (n >= 0) buf[n] = '\0';
    return n;
}

void _start(void) {
    sys_print("fat32demo: FAT32 read/write test\n", 0x0F);

    // --- 1. read a file created on the image by mtools (uppercase SFN) ---
    const char* expect = "Hello from FAT32 disk!\nline2\n";  // 29 bytes
    int en = slen(expect);
    char buf[256];

    int n = read_whole("/fat32/HELLO.TXT", buf, sizeof(buf));
    CHECK(n == en && mem_eq(buf, expect, en),
          "fat32demo: FAIL read /fat32/HELLO.TXT\n");
    if (n == en && mem_eq(buf, expect, en))
        sys_print("fat32demo: [OK] read /fat32/HELLO.TXT\n", 0x0A);

    // --- 2. case-insensitive lookup of a lowercase request ---
    n = read_whole("/fat32/hello2.txt", buf, sizeof(buf));
    CHECK(n == en && mem_eq(buf, expect, en),
          "fat32demo: FAIL read /fat32/hello2.txt (case)\n");

    // --- 3. nested directory ---
    n = read_whole("/fat32/docs/note.txt", buf, sizeof(buf));
    CHECK(n == en && mem_eq(buf, expect, en),
          "fat32demo: FAIL read /fat32/docs/note.txt\n");
    if (n == en && mem_eq(buf, expect, en))
        sys_print("fat32demo: [OK] read nested /fat32/docs/note.txt\n", 0x0A);

    // --- 4. LFN read: long names written by mtools ---
    n = read_whole("/fat32/The quick brown fox.txt", buf, sizeof(buf));
    CHECK(n == en && mem_eq(buf, expect, en),
          "fat32demo: FAIL read LFN file\n");
    if (n == en && mem_eq(buf, expect, en))
        sys_print("fat32demo: [OK] read LFN '/fat32/The quick brown fox.txt'\n", 0x0A);
    n = read_whole("/fat32/My Vacation Photos/summer2026 beach.txt", buf, sizeof(buf));
    CHECK(n == en && mem_eq(buf, expect, en),
          "fat32demo: FAIL read nested LFN file\n");
    if (n == en && mem_eq(buf, expect, en))
        sys_print("fat32demo: [OK] read nested LFN 'My Vacation Photos/summer2026 beach.txt'\n", 0x0A);

    // --- 5. create dir + file, write, read back ---
    if (sys_mkdir("/fat32/demo") < 0) {
        // May already exist from a previous run — that's fine.
        sys_print("fat32demo: mkdir /fat32/demo (exists?)\n", 0x07);
    }
    sys_delete_file("/fat32/demo/write.txt");  // clean slate
    if (sys_create_file("/fat32/demo/write.txt") < 0) {
        sys_print("fat32demo: FAIL create /fat32/demo/write.txt\n", 0x0C);
        fails++;
    }
    const char* payload = "FAT32 WRITE OK\n";  // 14 bytes
    int pn = slen(payload);
    int fd = sys_open("/fat32/demo/write.txt");
    if (fd < 0) {
        sys_print("fat32demo: FAIL open write.txt\n", 0x0C);
        fails++;
    } else {
        if (sys_write(fd, payload, pn) != pn) {
            sys_print("fat32demo: FAIL write\n", 0x0C);
            fails++;
        }
        sys_close(fd);
        n = read_whole("/fat32/demo/write.txt", buf, sizeof(buf));
        CHECK(n == pn && mem_eq(buf, payload, pn),
              "fat32demo: FAIL read back write.txt\n");
        if (n == pn && mem_eq(buf, payload, pn))
            sys_print("fat32demo: [OK] created + wrote + read back file\n", 0x0A);
    }

    // --- 6. O_APPEND: append without clobbering the front ---
    fd = sys_open_mode("/fat32/demo/write.txt", O_APPEND);
    if (fd < 0) {
        sys_print("fat32demo: FAIL open O_APPEND\n", 0x0C);
        fails++;
    } else {
        if (sys_write(fd, "TAIL\n", 5) != 5) {
            sys_print("fat32demo: FAIL append write\n", 0x0C);
            fails++;
        }
        sys_close(fd);
        n = read_whole("/fat32/demo/write.txt", buf, sizeof(buf));
        const char* expect_append = "FAT32 WRITE OK\nTAIL\n";  // 19 bytes
        int ea = slen(expect_append);
        CHECK(n == ea && mem_eq(buf, expect_append, ea),
              "fat32demo: FAIL O_APPEND result\n");
        if (n == ea && mem_eq(buf, expect_append, ea))
            sys_print("fat32demo: [OK] O_APPEND grew the file\n", 0x0A);
    }

    // --- 7. create a long-named file from the OS (LFN write) + read back ---
    sys_delete_file("/fat32/demo/long file name test.txt");  // clean slate
    if (sys_create_file("/fat32/demo/long file name test.txt") < 0) {
        sys_print("fat32demo: FAIL create LFN file\n", 0x0C);
        fails++;
    }
    fd = sys_open("/fat32/demo/long file name test.txt");
    if (fd < 0) {
        sys_print("fat32demo: FAIL open LFN file\n", 0x0C);
        fails++;
    } else {
        const char* lfn_payload = "LONG NAME WRITE OK\n";  // 18 bytes
        int ln = slen(lfn_payload);
        if (sys_write(fd, lfn_payload, ln) != ln) {
            sys_print("fat32demo: FAIL write LFN file\n", 0x0C);
            fails++;
        }
        sys_close(fd);
        n = read_whole("/fat32/demo/long file name test.txt", buf, sizeof(buf));
        CHECK(n == ln && mem_eq(buf, lfn_payload, ln),
              "fat32demo: FAIL read back LFN file\n");
        if (n == ln && mem_eq(buf, lfn_payload, ln))
            sys_print("fat32demo: [OK] created + wrote + read long-named file\n", 0x0A);
    }

    // --- 8. delete the file -> stat says gone ---
    if (sys_delete_file("/fat32/demo/write.txt") < 0) {
        sys_print("fat32demo: FAIL delete write.txt\n", 0x0C);
        fails++;
    }
    if (sys_stat_file("/fat32/demo/write.txt") >= 0) {
        sys_print("fat32demo: FAIL file still exists after delete\n", 0x0C);
        fails++;
    } else {
        sys_print("fat32demo: [OK] delete removed the file\n", 0x0A);
    }
    // Note: the LFN file deliberately stays on disk — the test script reads
    // the image with host mtools afterwards to prove OS-written LFN entries
    // are visible to real tools. The next run starts from a clean slate.

    if (fails == 0) {
        sys_print("fat32demo: ALL TESTS PASSED\n", 0x0A);
        sys_exit_with_code(0);
    }
    sys_print("fat32demo: FAILURES\n", 0x0C);
    sys_exit_with_code(1);
}
