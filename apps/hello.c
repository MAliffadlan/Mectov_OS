// hello.c — Mectov OS Ring 3 Hello World
// Compiled with -fno-pic, no GOT issues.

static inline int syscall3(int num, int a, int b, int c) {
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c)
    );
    return ret;
}

#define SYS_PRINT  1
#define SYS_EXIT   10

void _start() {
    syscall3(SYS_PRINT, (int)"Hello from Ring 3!\n", 0x0A, 0);
    syscall3(SYS_EXIT, 0, 0, 0);
    for(;;);
}
