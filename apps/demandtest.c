// demandtest — proves the lazy zero page + demand paging machinery:
//   1. Heap: malloc() 1 MiB (no frames committed), READ untouched pages
//      (must hit the shared zero page), then WRITE pages (private frames)
//      and verify zero-fill before writing and values after.
//   2. Stack: recurse deep enough to cross several 4KB pages — the user
//      stack must grow on demand instead of being pre-mapped.
//   3. COW fork: fork(); the child writes its own heap pages; the parent's
//      pages must be untouched afterwards (copy-on-write isolation).
// Prints "demandtest: ALL PASS" and exits 0, or a FAIL line and exits 1.
// The KVM harness asserts the serial log shows no [CRASH]/[EXCEPTION].
#include "src/include/syscall.h"

static int fails = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { sys_print(msg, 0x0C); fails++; } \
} while (0)

static void itoa(int n, char* buf) {
    int i = 0;
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[12];
    int t = 0;
    while (n > 0) { tmp[t++] = '0' + n % 10; n /= 10; }
    while (t > 0) buf[i++] = tmp[--t];
    buf[i] = '\0';
}

// Recurse 100 deep with a ~512-byte frame per level = ~50KB of stack, which
// crosses ~12 demand-paged 4KB pages while staying inside the 64KB user stack
// (USER_STACK_SIZE) — a deeper run would trip the guard page on purpose.
// `sink` forces the locals to be touched so every page really gets mapped.
static int deep_rec(int depth, volatile char* sink) {
    volatile char local[512];
    local[0] = (char)depth;                 // force the frame's page to be touched
    if (depth <= 0) return 0;
    (void)deep_rec(depth - 1, sink);
    *sink = local[0];                       // force the write path as we unwind
    return depth;                           // deterministic: the depth we reached
}

void _start(void) {
    sys_print("demandtest: lazy zero + heap/stack demand paging + COW fork\n", 0x0F);

    // ---- 1. Heap: malloc 1 MiB, read untouched (shared zero page) ----
    char* heap = sys_malloc(1024 * 1024);
    CHECK(heap != 0, "demandtest: FAIL malloc returned 0\n");
    if (!heap) { sys_exit_with_code(1); }

    // Reads on never-written pages must be zero (shared zero page path).
    CHECK(heap[0] == 0 && heap[4096] == 0, "demandtest: FAIL read zero-fill\n");
    CHECK(heap[512 * 1024] == 0 && heap[1024 * 1024 - 1] == 0,
          "demandtest: FAIL read zero-fill (2)\n");

    // Writes: private frames, values must read back.
    heap[0] = 1; heap[4096] = 2; heap[512 * 1024] = 3; heap[1024 * 1024 - 1] = 4;
    CHECK(heap[0] == 1 && heap[4096] == 2, "demandtest: FAIL write/readback\n");
    CHECK(heap[512 * 1024] == 3 && heap[1024 * 1024 - 1] == 4,
          "demandtest: FAIL write/readback (2)\n");

    // ---- 2. Stack: deep recursion crosses many pages on demand ----
    {
        volatile char sink = 0;
        int r = deep_rec(100, (volatile char*)&sink);
        CHECK(r == 100, "demandtest: FAIL deep recursion\n");
    }

    // ---- 3. COW fork: child writes, parent must stay isolated ----
    {
        int pid = sys_fork();
        if (pid == 0) {
            // Child: modify its own heap pages, then exit 42.
            heap[0] = 100;
            heap[512 * 1024] = 200;
            char localc[512];
            localc[0] = 7;
            localc[1] = (char)(localc[0] + 1);
            sys_print("demandtest: child wrote, exiting\n", 0x0F);
            sys_exit_with_code(42);
        } else if (pid > 0) {
            int status = 0;
            int r = sys_waitpid(pid, &status, 0);
            CHECK(r == pid && status == 42, "demandtest: FAIL child status\n");
            // The child's writes must not have touched the parent's pages.
            CHECK(heap[0] == 1, "demandtest: FAIL COW isolation heap[0]\n");
            CHECK(heap[4096] == 2, "demandtest: FAIL COW isolation heap[4K]\n");
            CHECK(heap[512 * 1024] == 3, "demandtest: FAIL COW isolation heap[512K]\n");
            CHECK(heap[1024 * 1024 - 1] == 4, "demandtest: FAIL COW isolation heap[1M]\n");
        } else {
            sys_print("demandtest: FAIL fork returned < 0\n", 0x0C);
            fails++;
        }
    }

    if (fails == 0) {
        sys_print("demandtest: ALL PASS\n", 0x0A);
        sys_exit_with_code(0);
    } else {
        char msg[40];
        sys_print("demandtest: FAILED with ", 0x0C);
        itoa(fails, msg);
        sys_print(msg, 0x0C);
        sys_print(" checks\n", 0x0C);
        sys_exit_with_code(1);
    }
}
