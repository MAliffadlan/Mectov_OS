// mmapfiledemo — proves file-backed mmap() (SYS_MMAP_FILE / SYS_MSYNC):
//   1. Create /mmapfile.txt and write "Hello from file-backed mmap!\n"
//   2. mmap_file() the open fd — NO bytes are read yet (fully lazy; the
//      kernel serial log shows "[MMAP] file map", not "[MMAP] file paged")
//   3. Reading the mapping faults each page in FROM THE DISK on demand
//      (kernel serial log shows "[MMAP] file paged" per page)
//   4. Modify a byte in place — the first write marks the page dirty
//   5. msync() flushes the dirty page back to the file ("[MMAP] flushed")
//   6. Re-open the file and read it: the change must be on disk
//   7. munmap() flushes and releases the region ("[MMAP] unmapped")
#include "src/include/syscall.h"

static int fails = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { sys_print(msg, 0x0C); fails++; } \
} while (0)

static int mem_eq(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

void _start(void) {
    sys_print("mmapfiledemo: file-backed mmap test\n", 0x0F);

    const char* path = "/mmapfile.txt";
    // Start from a clean file: a previous run may have left one behind (the
    // disk persists), and sys_write seeds with a read-modify-write that would
    // keep stale trailing bytes.
    sys_delete_file(path);
    if (sys_create_file(path) < 0 && sys_stat_file(path) < 0) {
        sys_print("mmapfiledemo: FAIL cannot create /mmapfile.txt\n", 0x0C);
        sys_exit_with_code(1);
    }

    // Seed the file with known content via regular file I/O.
    const char* text = "Hello from file-backed mmap!\n";
    int n = 0;
    while (text[n]) n++;

    int fd = sys_open(path);
    if (fd < 0) {
        sys_print("mmapfiledemo: FAIL open\n", 0x0C);
        sys_exit_with_code(1);
    }
    if (sys_write(fd, text, n) != n) {
        sys_print("mmapfiledemo: FAIL write seed\n", 0x0C);
        sys_exit_with_code(1);
    }
    sys_close(fd);

    // Map the file. Nothing is read yet — pages fault in from disk on the
    // first access below.
    fd = sys_open(path);
    void* base = sys_mmap_file(fd, 1);  // MMAP_FILE_SHARED
    sys_close(fd);                       // mapping survives fd close
    if (base == 0) {
        sys_print("mmapfiledemo: FAIL mmap_file returned 0\n", 0x0C);
        sys_exit_with_code(1);
    }
    sys_print("mmapfiledemo: mapped, reading (lazy fault-in)...\n", 0x0F);

    // First touch: content must match what was written to the file.
    CHECK(mem_eq((const char*)base, text, n),
          "mmapfiledemo: FAIL content after fault-in\n");

    // Modify in place — first write marks the page dirty.
    ((char*)base)[0] = 'J';
    CHECK(((char*)base)[0] == 'J', "mmapfiledemo: FAIL read-back after write\n");

    // msync: flush the dirty page back to the file.
    if (!sys_msync(base)) {
        sys_print("mmapfiledemo: FAIL msync\n", 0x0C);
        fails++;
    } else {
        sys_print("mmapfiledemo: msync OK\n", 0x0A);
    }

    // Verify the change landed on disk via a fresh fd (bypassing the mapping).
    fd = sys_open(path);
    char buf[64];
    int r = sys_read(fd, buf, 63);
    sys_close(fd);
    CHECK(r == n, "mmapfiledemo: FAIL file size after msync\n");
    CHECK(r > 0 && buf[0] == 'J',
          "mmapfiledemo: FAIL file not updated on disk after msync\n");
    CHECK(r > 0 && mem_eq(buf, "Jello from file-backed mmap!\n", n),
          "mmapfiledemo: FAIL file content after msync\n");

    // munmap: flushes and releases the region.
    if (!sys_munmap(base)) {
        sys_print("mmapfiledemo: FAIL munmap\n", 0x0C);
        fails++;
    } else {
        sys_print("mmapfiledemo: munmap OK\n", 0x0A);
    }

    // The file must still be readable afterwards (nodes untouched).
    fd = sys_open(path);
    r = sys_read(fd, buf, 63);
    sys_close(fd);
    CHECK(r == n && buf[0] == 'J', "mmapfiledemo: FAIL post-munmap read\n");

    if (fails == 0) {
        sys_print("mmapfiledemo: ALL TESTS PASSED\n", 0x0A);
        sys_exit_with_code(0);
    } else {
        sys_print("mmapfiledemo: TESTS FAILED\n", 0x0C);
        sys_exit_with_code(1);
    }
}
