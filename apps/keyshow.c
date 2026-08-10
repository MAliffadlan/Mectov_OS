// keyshow.c — Ring 3 app that reads one key via SYS_GET_KEY
// and writes the received character to serial (fd 2).
// Used to verify the foreground-app keyboard buffer (v38.9).
#define SYS_GET_KEY  13
#define SYS_WRITE    4
#define SYS_EXIT     10
#define SYS_YIELD    9

static inline int syscall3(int num, int a, int b, int c) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a), "c"(b), "d"(c));
    return ret;
}

void _start() {
    // Signal we're ready
    char ready[] = "KEYSHOW_READY\n";
    syscall3(SYS_WRITE, 2, (int)ready, 13);

    char c = 0;
    while (c == 0) {
        c = (char)syscall3(SYS_GET_KEY, 0, 0, 0);
        if (c != 0) {
            char out[4] = { 'K', 'E', 'Y', '=' };
            syscall3(SYS_WRITE, 2, (int)out, 4);
            syscall3(SYS_WRITE, 2, (int)&c, 1);
            syscall3(SYS_WRITE, 2, (int)"\n", 1);
            break;
        }
        syscall3(SYS_YIELD, 0, 0, 0);
    }

    char done[] = "KEYSHOW_DONE\n";
    syscall3(SYS_WRITE, 2, (int)done, 12);
    syscall3(SYS_EXIT, 0, 0, 0);
    for(;;);
}