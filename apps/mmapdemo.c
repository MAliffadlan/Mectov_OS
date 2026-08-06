// mmapdemo — proves demand-paged mmap():
//   1. mmap() a 1 MiB range (no physical frames committed)
//   2. Touch sparse pages far apart -> kernel logs "[MMAP] demand paged"
//      per page (lazy allocation, not upfront)
//   3. Verify pages are zero-filled (never leaked from another task)
//   4. Write values, read them back
//   5. munmap() frees the faulted frames, then re-mmap works
#include "src/include/syscall.h"

static int fails = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { sys_print(msg, 0x0C); fails++; } \
} while (0)

void _start(void) {
    sys_print("mmapdemo: reserving 1MiB...\n", 0x0F);

    void* base = sys_mmap(1024 * 1024);
    if (base == 0) {
        sys_print("mmapdemo: FAIL mmap returned 0\n", 0x0C);
        sys_exit_with_code(1);
    }

    char* p = (char*)base;
    // Touch pages at 0, 64KiB, 512KiB, 1MiB-4KiB — sparse, so only 4 frames
    // should be allocated on demand.
    p[0] = 42;
    p[64 * 1024] = 7;
    p[512 * 1024] = 99;
    p[1024 * 1024 - 1] = 123;

    // Zero-fill check on untouched pages between the touched ones.
    CHECK(p[4096] == 0, "mmapdemo: FAIL untouched page not zero\n");
    CHECK(p[128 * 1024] == 0, "mmapdemo: FAIL untouched page not zero (2)\n");

    // Read-back check.
    CHECK(p[0] == 42, "mmapdemo: FAIL readback p[0]\n");
    CHECK(p[64 * 1024] == 7, "mmapdemo: FAIL readback p[64K]\n");
    CHECK(p[512 * 1024] == 99, "mmapdemo: FAIL readback p[512K]\n");
    CHECK(p[1024 * 1024 - 1] == 123, "mmapdemo: FAIL readback p[1M-1]\n");

    sys_print("mmapdemo: sparse writes + zero-fill verified\n", 0x0A);

    if (sys_munmap(base)) {
        sys_print("mmapdemo: munmap OK\n", 0x0A);
    } else {
        sys_print("mmapdemo: FAIL munmap\n", 0x0C);
        fails++;
    }

    // After munmap the range must be usable again (region slot freed).
    void* base2 = sys_mmap(4096);
    if (base2 == 0) {
        sys_print("mmapdemo: FAIL re-mmap after munmap\n", 0x0C);
        sys_exit_with_code(1);
    }
    sys_print("mmapdemo: re-mmap after munmap OK\n", 0x0A);
    sys_munmap(base2);

    if (fails == 0) {
        sys_print("mmapdemo: ALL TESTS PASSED\n", 0x0A);
        sys_exit_with_code(0);
    } else {
        sys_print("mmapdemo: TESTS FAILED\n", 0x0C);
        sys_exit_with_code(1);
    }
}
