// lseekfiledemo — proves POSIX file positioning & metadata (SYS_LSEEK /
// SYS_FSTAT / O_APPEND):
//   1. Seed /lseekfile.txt with "ABCDEFGHIJKLMNOPQRSTUVWXYZ\n" (27 bytes)
//   2. lseek(SEEK_SET, 5)  -> read continues at "F..."
//   3. lseek(SEEK_END, 0)  -> read returns 0 (EOF)
//   4. lseek(SEEK_END, -3) -> read returns "XYZ"
//   5. lseek(SEEK_CUR, -3) -> read returns "XYZ" again (relative)
//   6. fstat -> size 27, type FS_FILE(0)
//   7. open(O_APPEND) + write "123\n" -> size becomes 31, appended at end
//   8. non-append overwrite at offset 0 replaces bytes in place
//   9. lseek on a pipe -> -1 (not seekable); bad whence -> -1
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

void _start(void) {
    sys_print("lseekfiledemo: lseek/fstat/O_APPEND test\n", 0x0F);

    const char* path = "/lseekfile.txt";
    // Start from a clean file (the disk persists across runs).
    sys_delete_file(path);
    if (sys_create_file(path) < 0 && sys_stat_file(path) < 0) {
        sys_print("lseekfiledemo: FAIL cannot create /lseekfile.txt\n", 0x0C);
        sys_exit_with_code(1);
    }

    const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZ\n";  // 27 bytes
    int n = slen(abc);

    int fd = sys_open(path);
    if (fd < 0) { sys_print("lseekfiledemo: FAIL open\n", 0x0C); sys_exit_with_code(1); }
    if (sys_write(fd, abc, n) != n) {
        sys_print("lseekfiledemo: FAIL seed write\n", 0x0C);
        sys_exit_with_code(1);
    }
    sys_close(fd);

    char buf[64];

    // --- lseek SEEK_SET: continue reading from an absolute offset ---
    fd = sys_open(path);
    CHECK(sys_lseek(fd, 5, SEEK_SET) == 5, "lseekfiledemo: FAIL SEEK_SET\n");
    int r = sys_read(fd, buf, 63);
    CHECK(r == n - 5, "lseekfiledemo: FAIL read after SEEK_SET len\n");
    CHECK(r > 0 && mem_eq(buf, "FGHIJKLMNOPQRSTUVWXYZ\n", n - 5),
          "lseekfiledemo: FAIL read after SEEK_SET content\n");

    // --- lseek SEEK_END: read at EOF returns 0 ---
    CHECK(sys_lseek(fd, 0, SEEK_END) == n, "lseekfiledemo: FAIL SEEK_END\n");
    r = sys_read(fd, buf, 63);
    CHECK(r == 0, "lseekfiledemo: FAIL read at EOF (expected 0)\n");

    // --- lseek SEEK_END negative: read the tail (last 4 bytes = "XYZ\n") ---
    CHECK(sys_lseek(fd, -4, SEEK_END) == n - 4, "lseekfiledemo: FAIL SEEK_END -4\n");
    r = sys_read(fd, buf, 63);
    CHECK(r == 4 && mem_eq(buf, "XYZ\n", 4), "lseekfiledemo: FAIL read tail\n");

    // --- lseek SEEK_CUR: relative to current offset (now 27) ---
    CHECK(sys_lseek(fd, -4, SEEK_CUR) == n - 4, "lseekfiledemo: FAIL SEEK_CUR\n");
    r = sys_read(fd, buf, 63);
    CHECK(r == 4 && mem_eq(buf, "XYZ\n", 4), "lseekfiledemo: FAIL SEEK_CUR read\n");

    // --- fstat ---
    stat_t st;
    CHECK(sys_fstat(fd, &st) == 0, "lseekfiledemo: FAIL fstat\n");
    CHECK(st.size == n, "lseekfiledemo: FAIL fstat size\n");
    CHECK(st.type == 0, "lseekfiledemo: FAIL fstat type\n");
    CHECK(st.node_idx >= 0, "lseekfiledemo: FAIL fstat node_idx\n");

    // --- lseek error paths: bad whence, negative result, pipe ---
    CHECK(sys_lseek(fd, 0, 99) == -1, "lseekfiledemo: FAIL lseek bad whence\n");
    CHECK(sys_lseek(fd, -100, SEEK_SET) == -1, "lseekfiledemo: FAIL lseek negative\n");
    int pipefd[2];
    CHECK(sys_pipe(pipefd) == 0, "lseekfiledemo: FAIL pipe\n");
    CHECK(sys_lseek(pipefd[0], 0, SEEK_SET) == -1, "lseekfiledemo: FAIL lseek pipe\n");
    sys_close(pipefd[0]);
    sys_close(pipefd[1]);
    sys_close(fd);

    // --- O_APPEND: writes always land at the end ---
    const char* tail = "123\n";
    fd = sys_open_mode(path, O_APPEND);
    if (fd < 0) { sys_print("lseekfiledemo: FAIL open O_APPEND\n", 0x0C); sys_exit_with_code(1); }
    // Move the offset backwards on purpose: O_APPEND must still append at EOF.
    sys_lseek(fd, 0, SEEK_SET);
    if (sys_write(fd, tail, 4) != 4) {
        sys_print("lseekfiledemo: FAIL append write\n", 0x0C);
        sys_exit_with_code(1);
    }
    sys_close(fd);

    fd = sys_open(path);
    CHECK(sys_fstat(fd, &st) == 0 && st.size == n + 4,
          "lseekfiledemo: FAIL size after append\n");
    r = sys_read(fd, buf, 63);
    CHECK(r == n + 4, "lseekfiledemo: FAIL read len after append\n");
    CHECK(r > 0 && mem_eq(buf, "ABCDEFGHIJKLMNOPQRSTUVWXYZ\n123\n", n + 4),
          "lseekfiledemo: FAIL content after append\n");
    sys_close(fd);

    // --- non-append write at an offset overwrites in place ---
    fd = sys_open(path);
    sys_lseek(fd, 0, SEEK_SET);
    if (sys_write(fd, "ZZ", 2) != 2) {
        sys_print("lseekfiledemo: FAIL overwrite write\n", 0x0C);
        sys_exit_with_code(1);
    }
    sys_close(fd);
    fd = sys_open(path);
    r = sys_read(fd, buf, 63);
    sys_close(fd);
    CHECK(r > 0 && mem_eq(buf, "ZZCDEFGHIJKLMNOPQRSTUVWXYZ\n123\n", n + 4),
          "lseekfiledemo: FAIL in-place overwrite\n");

    if (fails == 0) {
        sys_print("lseekfiledemo: ALL TESTS PASSED\n", 0x0A);
        sys_exit_with_code(0);
    } else {
        sys_print("lseekfiledemo: TESTS FAILED\n", 0x0C);
        sys_exit_with_code(1);
    }
}
