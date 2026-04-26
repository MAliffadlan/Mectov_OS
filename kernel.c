// --- MECTOV OS v12.0 (The Interrupt & Foundation Update) ---
#include "src/include/types.h"
#include "src/include/vga.h"
#include "src/include/keyboard.h"
#include "src/include/speaker.h"
#include "src/include/ata.h"
#include "src/include/vfs.h"
#include "src/include/security.h"
#include "src/include/shell.h"
#include "src/include/mem.h"
#include "src/include/utils.h"
#include "src/include/apps.h"
#include "src/include/io.h"
#include "src/include/idt.h"
#include "src/include/timer.h"

void kernel_main(void) {
    // 1. Memory & Interrupt Core (MUST BE FIRST)
    init_mem(32 * 1024 * 1024);
    idt_init();
    
    // 2. Drivers & Hardware
    init_timer(50);    // 50Hz detak jantung
    init_keyboard();   // Aktifkan Bel Rumah Keyboard
    detect_cpu();
    init_uptime();
    vfs_load(); 
    
    // 3. Enable Interrupts
    __asm__ __volatile__ ("sti"); 
    
    // 4. Setup UI
    d_desktop(); 
    d_win(WIN_X, WIN_Y, WIN_W, WIN_H, " Mectov Security Login "); 
    c_work();
    
    const char* pass = "mectov123"; 
    char in[32]; 
    int in_idx = 0;
    print("Welcome to Mectov OS v12.0\n", 0x0E);
    print("IDT Core : Active (Interrupt Driven)\n", 0x0A);
    print("Password: ", 0x0E);
    
    int log = 0; 
    
    while (1) {
        if (!log || is_locked) {
            uint8_t sc = k_get_scancode(); // Baca dari antrean, bukan polling hardware!
            if (sc != 0 && sc < 0x80) {
                char c = scancode_to_char(sc);
                if (c == '\n') { 
                    in[in_idx] = '\0'; 
                    if (strcmp(in, pass) == 0) { 
                        log = 1; is_locked = 0; beep(); 
                        c_work(); 
                        d_win(WIN_X, WIN_Y, WIN_W, WIN_H, " Terminal - Mectov OS "); 
                        print("Login Success! Interrupt Engine OK.\nroot@mectov:~# ", 0x0A); 
                    } else { 
                        print("\nDenied!\nPassword: ", 0x0C); in_idx = 0; 
                    } 
                }
                else if (c == '\b' && in_idx > 0) { 
                    in_idx--; 
                    d_char(CX + 10 + in_idx, CY + (is_locked?2:1), ' ', 0x0F); 
                    update_hw_cursor(CX + 10 + in_idx, CY + (is_locked?2:1)); 
                }
                else if (c != 0 && in_idx < 31) { 
                    in[in_idx++] = c; 
                    p_char('*', 0x0F); 
                }
            }
            __asm__ __volatile__ ("hlt");
            continue;
        }

        // --- Loop Terminal Utama ---
        uint8_t sc = k_get_scancode();
        if (sc != 0) {
            // Panah Atas (History)
            if (sc == 0x48 && !ed_a) {
                while (cx > 15) { cx--; d_char(CX + cx, CY + cy, ' ', 0x0F); }
                strcpy(cmd_b, hist_b); b_idx = 0; 
                while(cmd_b[b_idx]) { d_char(CX + cx, CY + cy, cmd_b[b_idx], cur_col); cx++; b_idx++; }
                update_hw_cursor(CX + cx, CY + cy);
            }
            // Tab Auto-Complete
            else if (sc == 0x0F && !ed_a && b_idx > 0) {
                cmd_b[b_idx] = '\0';
                for (int i = 0; i < 21; i++) {
                    if (strncmp(cmd_b, cmd_list[i], b_idx) == 0) {
                        const char* match = cmd_list[i];
                        while (match[b_idx]) { d_char(CX + cx, CY + cy, match[b_idx], cur_col); cmd_b[b_idx] = match[b_idx]; cx++; b_idx++; }
                        cmd_b[b_idx] = '\0'; update_hw_cursor(CX + cx, CY + cy); break;
                    }
                }
            }
            // ESC
            else if (sc == 0x01) { if (ed_a) sa_ex_ed(); }
            // Normal Keys (Make code only)
            else if (sc < 0x80) {
                char c = scancode_to_char(sc);
                if (ed_a) {
                    if (c == '\b' && ed_c > 0) { ed_c--; ed_b[ed_c] = '\0'; c_work(); print(ed_b, 0x0F); } 
                    else if (c != 0 && ed_c < MAX_FILE_SIZE-1) { ed_b[ed_c++] = c; p_char(c, 0x0F); } 
                } else {
                    if (c == '\n') { ex_cmd(); }
                    else if (c == '\b') { if (b_idx > 0) { b_idx--; if (cx > 15) { cx--; d_char(CX + cx, CY + cy, ' ', 0x0F); update_hw_cursor(CX + cx, CY + cy); } } } 
                    else if (c != 0) { d_char(CX + cx, CY + cy, c, cur_col); cx++; if (cx >= CW) { cx = 0; cy++; } if (cy >= CH-1) s_work(); if (b_idx < 255) { cmd_b[b_idx] = c; b_idx++; } update_hw_cursor(CX + cx, CY + cy); } 
                }
            }
        }
        
        __asm__ __volatile__ ("hlt"); // Idle gracefully
    }
}
