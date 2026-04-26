// --- MECTOV OS v11.0 (Modular Kernel Entry Point) ---
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

void kernel_main(void) {
    // 1. Inisialisasi Memori (MMM Core) - Alokasikan 32MB untuk simulasi
    init_mem(32 * 1024 * 1024);
    
    // 2. Deteksi Hardware
    detect_cpu();
    init_uptime();
    vfs_load(); 
    
    // 3. Setup UI
    d_desktop(); 
    d_win(WIN_X, WIN_Y, WIN_W, WIN_H, " Mectov Security Login "); 
    c_work();
    
    // 4. Login System
    const char* pass = "mectov123"; 
    char in[32]; 
    int in_idx = 0;
    print("Welcome to Mectov OS v11.0\n", 0x0E);
    print("MMM Core : Active (32MB RAM Managed)\n", 0x0A);
    print("Password: ", 0x0E);
    
    int log = 0; 
    unsigned char ls = 0;
    
    while (1) {
        // --- Loop Login / Lock ---
        if (!log || is_locked) {
            rand_seed++; 
            update_marquee(); 
            update_hud();
            
            if (inb(0x64) & 1) {
                unsigned char sc = inb(0x60);
                if (sc < 0x80 && sc != ls) {
                    char c = scancode_to_char(sc);
                    if (c == '\n') { 
                        in[in_idx] = '\0'; 
                        if (strcmp(in, pass) == 0) { 
                            log = 1; is_locked = 0; beep(); 
                            c_work(); 
                            d_win(WIN_X, WIN_Y, WIN_W, WIN_H, " Terminal - Mectov OS "); 
                            print("Login Success! Memory Managed.\nroot@mectov:~# ", 0x0A); 
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
                    ls = sc;
                } else if (sc >= 0x80) ls = 0;
            }
            continue;
        }

        // --- Loop Terminal Utama ---
        rand_seed++; 
        update_marquee(); 
        update_hud();
        
        if (inb(0x64) & 1) {
            unsigned char sc = inb(0x60);
            
            // Modifier Keys
            if (sc == 0x2A || sc == 0x36) shift_p = 1; 
            else if (sc == 0xAA || sc == 0xB6) shift_p = 0; 
            else if (sc == 0x3A) caps_a = !caps_a;
            
            // History (Panah Atas)
            if (sc == 0x48 && ls != 0x48 && !ed_a) {
                while (cx > 15) { cx--; d_char(CX + cx, CY + cy, ' ', 0x0F); }
                strcpy(cmd_b, hist_b); b_idx = 0; 
                while(cmd_b[b_idx]) { d_char(CX + cx, CY + cy, cmd_b[b_idx], cur_col); cx++; b_idx++; }
                update_hw_cursor(CX + cx, CY + cy); ls = sc; continue;
            }

            // Tab Auto-Complete
            if (sc == 0x0F && ls != 0x0F && !ed_a && b_idx > 0) {
                cmd_b[b_idx] = '\0';
                for (int i = 0; i < 21; i++) { // 21 commands including 'mem'
                    if (strncmp(cmd_b, cmd_list[i], b_idx) == 0) {
                        const char* match = cmd_list[i];
                        while (match[b_idx]) { d_char(CX + cx, CY + cy, match[b_idx], cur_col); cmd_b[b_idx] = match[b_idx]; cx++; b_idx++; }
                        cmd_b[b_idx] = '\0'; update_hw_cursor(CX + cx, CY + cy); break;
                    }
                }
                ls = sc; continue;
            }

            // ESC (Editor / Lock)
            if (sc == 0x01 && ls != 0x01) { if (ed_a) sa_ex_ed(); ls = sc; continue; }

            // Normal Keys
            if (sc < 0x80 && sc != ls) {
                char c = scancode_to_char(sc);
                if (ed_a) { // Mode Editor
                    if (c == '\b' && ed_c > 0) { ed_c--; ed_b[ed_c] = '\0'; c_work(); print(ed_b, 0x0F); } 
                    else if (c != 0 && ed_c < MAX_FILE_SIZE-1) { ed_b[ed_c++] = c; p_char(c, 0x0F); } 
                } else { // Mode Terminal
                    if (c == '\n') { ex_cmd(); in_idx = 0; }
                    else if (c == '\b') { if (b_idx > 0) { b_idx--; if (cx > 15) { cx--; d_char(CX + cx, CY + cy, ' ', 0x0F); update_hw_cursor(CX + cx, CY + cy); } } } 
                    else if (c != 0) { d_char(CX + cx, CY + cy, c, cur_col); cx++; if (cx >= CW) { cx = 0; cy++; } if (cy >= CH-1) s_work(); if (b_idx < 255) { cmd_b[b_idx] = c; b_idx++; } update_hw_cursor(CX + cx, CY + cy); } 
                }
                ls = sc;
            } else if (sc >= 0x80) ls = 0;
        }
    }
}
