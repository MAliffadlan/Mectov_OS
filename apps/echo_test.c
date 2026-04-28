// echo_test.c - Mectov OS Native Version
// Tanpa string literal, tapi menggunakan nomor syscall Mectov OS yang BENAR.

#define SYS_PRINT    1
#define SYS_YIELD    9
#define SYS_EXIT     10
#define SYS_GET_KEY  13

static inline int syscall3(int num, int a, int b, int c);

void _start() {
    // 1. Cetak prompt "> " (dibangun di stack agar terhindar dari bug literal .rodata)
    char prompt[3];
    prompt[0] = '>';
    prompt[1] = ' ';
    prompt[2] = '\0';
    syscall3(SYS_PRINT, (int)prompt, 0x0A, 0);

    // 2. Baca satu karakter (Looping dengan GET_KEY & YIELD karena Mectov non-blocking)
    char c = 0;
    while (c == 0) {
        c = (char)syscall3(SYS_GET_KEY, 0, 0, 0);
        if (c != 0) break;
        syscall3(SYS_YIELD, 0, 0, 0); // Cegah hang/CPU 100%
    }

    // 3. Cetak ulang karakter yang dibaca + newline
    char out[3];
    out[0] = c;
    out[1] = '\n';
    out[2] = '\0';
    syscall3(SYS_PRINT, (int)out, 0x0F, 0);

    // 4. Exit (Di Mectov OS, exit = 10, bukan 0)
    syscall3(SYS_EXIT, 0, 0, 0);
    
    // Fallback infinite loop (praktik wajib di _start OS)
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
