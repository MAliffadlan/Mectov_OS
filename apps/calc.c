// ============================================================
// calc.c — Mectov OS Ring 3 Calculator
// Aplikasi kalkulator integer yang berjalan di terminal GUI.
// Mendukung: + - * / dengan loop interaktif.
// Compiled with -fno-pic, EBX is safe to use directly.
// ============================================================

#include "lib/libc.h"

// Define the global pointer to the shared library export table
void** __mct_lib_ptr = 0;

// --- Print string to terminal ---
static void print(const char* s, int color) {
    sys_print(s, color);
}


// --- Get one key (blocking) ---
static char getchar(void) {
    char c;
    while (1) {
        c = (char)sys_get_key();
        if (c != 0) return c;
        sys_yield();
    }
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

// --- Entry point ---
void _start() {
    // Load shared library
    __mct_lib_ptr = (void**)mct_load_library("apps/libc.mct");
    if (!__mct_lib_ptr) {
        syscall5(SYS_WRITE, 1, (int)"[!] Fatal: Failed to load libc.mct\n", 35, 0, 0); // fallback if sys_print doesn't work
        sys_exit();
    }
    
    // We loaded the library successfully!
    int fd = sys_open("/dev/serial");
    if (fd < 0) {
        // Just directly call sys_write to fd 1 if serial is 1, or use dummy
    }
    
    // Print banner using print
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
        int a = atoi(buf1);
        
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
        int b = atoi(buf2);
        
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
        itoa(a, str_a);
        itoa(b, str_b);
        itoa(result, str_r);
        
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
    sys_exit();
}
