// ============================================================
// calc.c — Mectov OS Ring 3 Calculator
// Aplikasi kalkulator integer yang berjalan di terminal GUI.
// Mendukung: + - * / dengan loop interaktif.
// Compiled with -fno-pic, EBX is safe to use directly.
// ============================================================

// --- Syscall interface (no kernel headers, no PIC) ---
static inline int syscall3(int num, int a, int b, int c) {
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c)
    );
    return ret;
}

#define SYS_PRINT    1
#define SYS_GET_KEY  13
#define SYS_YIELD    9
#define SYS_EXIT     10

// --- Print string to terminal ---
static void print(const char* s, int color) {
    syscall3(SYS_PRINT, (int)s, color, 0);
}

// --- Get one key (blocking) ---
static char getchar(void) {
    char c;
    while (1) {
        c = (char)syscall3(SYS_GET_KEY, 0, 0, 0);
        if (c != 0) return c;
        syscall3(SYS_YIELD, 0, 0, 0);
    }
}

// --- String helpers ---
static int my_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

// --- Read a line from keyboard into buf (max len-1 chars) ---
static int readline(char* buf, int len) {
    int i = 0;
    while (i < len - 1) {
        char c = getchar();
        if (c == '\n' || c == '\r') {
            print("\n", 0x0F);
            break;
        }
        if (c == '\b' || c == 127) {
            if (i > 0) {
                i--;
                print("\b \b", 0x0F);
            }
            continue;
        }
        if (c >= 32 && c < 127) {
            buf[i++] = c;
            // Echo the character
            char echo[2] = { c, 0 };
            print(echo, 0x0B); // cyan echo
        }
    }
    buf[i] = '\0';
    return i;
}

// --- Convert string to integer (supports negative) ---
static int my_atoi(const char* s) {
    int result = 0;
    int sign = 1;
    int i = 0;
    
    // Skip whitespace
    while (s[i] == ' ') i++;
    
    // Handle sign
    if (s[i] == '-') { sign = -1; i++; }
    else if (s[i] == '+') { i++; }
    
    // Convert digits
    while (s[i] >= '0' && s[i] <= '9') {
        result = result * 10 + (s[i] - '0');
        i++;
    }
    
    return result * sign;
}

// --- Convert integer to string ---
static void my_itoa(int num, char* buf) {
    if (num == 0) {
        buf[0] = '0'; buf[1] = '\0';
        return;
    }
    
    int negative = 0;
    unsigned int unum;
    
    if (num < 0) {
        negative = 1;
        unum = (unsigned int)(-(num + 1)) + 1; // handle INT_MIN
    } else {
        unum = (unsigned int)num;
    }
    
    // Build digits in reverse
    char tmp[12];
    int i = 0;
    while (unum > 0) {
        tmp[i++] = '0' + (unum % 10);
        unum /= 10;
    }
    
    int j = 0;
    if (negative) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

// --- Entry point ---
void _start() {
    // Banner
    print("====================================\n", 0x0E);
    print("   Mectov Calculator v1.0 (Ring 3)\n", 0x0A);
    print("====================================\n", 0x0E);
    print("  Operasi: + - * /\n", 0x07);
    print("  Ketik 'q' untuk keluar.\n\n", 0x07);
    
    char buf1[16], buf2[16], op_buf[4], again_buf[4];
    
    while (1) {
        // --- Input angka pertama ---
        print("[>] Angka pertama : ", 0x0F);
        int n1 = readline(buf1, 16);
        if (n1 == 0) continue;
        if (buf1[0] == 'q') break;
        int a = my_atoi(buf1);
        
        // --- Input operator ---
        print("[>] Operator (+,-,*,/) : ", 0x0F);
        int n2 = readline(op_buf, 4);
        if (n2 == 0) continue;
        char op = op_buf[0];
        
        if (op != '+' && op != '-' && op != '*' && op != '/') {
            print("[!] Operator tidak valid!\n\n", 0x0C);
            continue;
        }
        
        // --- Input angka kedua ---
        print("[>] Angka kedua   : ", 0x0F);
        int n3 = readline(buf2, 16);
        if (n3 == 0) continue;
        int b = my_atoi(buf2);
        
        // --- Hitung ---
        int result = 0;
        int error = 0;
        
        switch (op) {
            case '+': result = a + b; break;
            case '-': result = a - b; break;
            case '*': result = a * b; break;
            case '/':
                if (b == 0) {
                    print("[!] ERROR: Pembagian dengan nol!\n\n", 0x0C);
                    error = 1;
                } else {
                    result = a / b;
                }
                break;
        }
        
        if (error) continue;
        
        // --- Tampilkan hasil ---
        char str_a[12], str_b[12], str_r[12];
        my_itoa(a, str_a);
        my_itoa(b, str_b);
        my_itoa(result, str_r);
        
        char op_str[4] = { ' ', op, ' ', '\0' };
        
        print("\n  Hasil: ", 0x0E);
        print(str_a, 0x0B);
        print(op_str, 0x0F);
        print(str_b, 0x0B);
        print(" = ", 0x0F);
        print(str_r, 0x0A);
        print("\n\n", 0x0F);
        
        // --- Lanjut? ---
        print("[?] Hitung lagi? (y/n): ", 0x0F);
        readline(again_buf, 4);
        if (again_buf[0] != 'y' && again_buf[0] != 'Y') break;
        print("\n", 0x0F);
    }
    
    print("\n[*] Terima kasih! Sampai jumpa.\n", 0x0E);
    
    // Exit task
    syscall3(SYS_EXIT, 0, 0, 0);
    for(;;);
}
