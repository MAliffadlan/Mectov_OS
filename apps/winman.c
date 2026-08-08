// winman.c — opens 12 windows back-to-back to prove the WM accepts more
// than the old MAX_WINDOWS=8 (now 16). Each wid is printed via SYS_PRINT
// (mirrored to serial), and every SYS_CREATE_WINDOW is also mirrored by the
// kernel as "[WM] create wid=0x...". Run it:  run /apps/winman.mct
#include "src/include/syscall.h"

// Standalone MCT apps are linked without libc.mct, so declare the
// 5-arg syscall shim directly (matches syscall5 in apps/lib/libc.h).
static int sys_win_open(int x, int y, int w, int h, const char* title) {
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((int)SYS_CREATE_WINDOW), "b"(x), "c"(y), "d"(w), "S"(h), "D"((int)title)
        : "memory");
    return ret;
}

void _start(void) {
    sys_print("WINMAN: opening 12 windows...\n", 0x0E);
    char title[24];
    int ok = 0, failed = 0;
    for (int i = 0; i < 12; i++) {
        title[0] = 'W'; title[1] = 'i'; title[2] = 'n'; title[3] = ' ';
        title[4] = '0' + (i / 10);
        title[5] = '0' + (i % 10);
        title[6] = '\0';
        int wid = sys_win_open(40 + i * 20, 60 + i * 20, 160, 120, title);
        if (wid >= 0) ok++; else failed++;
    }
    // Summary line so tests can assert all 12 succeeded. The per-window
    // wids are already mirrored by the kernel as "[WM] create wid=0x...".
    char sum[40];
    sum[0] = 'O'; sum[1] = 'K'; sum[2] = '=';
    sum[3] = '0' + (ok / 10); sum[4] = '0' + (ok % 10);
    sum[5] = 'F'; sum[6] = 'A'; sum[7] = 'I'; sum[8] = 'L';
    sum[9] = '='; sum[10] = '0' + (failed / 10); sum[11] = '0' + (failed % 10);
    sum[12] = '\n'; sum[13] = '\0';
    sys_print(sum, 0x0A);
    sys_exit();
}
