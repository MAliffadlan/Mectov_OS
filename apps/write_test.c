// write_test.c — Mectov OS Ring 3 Write Test
// Compiled with -fno-pic, string literals work directly.
#define SYS_PRINT  1
#define SYS_EXIT   10

static inline int syscall3(int num, int a, int b, int c);
static void print(const char* s, int color);

void _start() {
    print("=== WRITE TEST ===\n", 0x0E);
    print("Menulis angka: 42\n", 0x0F);
    print("Selesai.\n", 0x0A);

    syscall3(SYS_EXIT, 0, 0, 0);
    for(;;);
}

static inline int syscall3(int num, int a, int b, int c) {
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c)
    );
    return ret;
}

static void print(const char* s, int color) {
    syscall3(SYS_PRINT, (int)s, color, 0);
}
