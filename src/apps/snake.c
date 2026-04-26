#include "../include/apps.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/speaker.h"
#include "../include/utils.h"
#include "../include/mem.h"
#include "../include/timer.h"
#include "../include/io.h"

void start_ular() {
    c_work(); d_win(15, 5, 50, 15, " Mectov Ular v1.2 - WASD / Arrows ");
    int sx[100], sy[100], len = 3, fx, fy, dir = 1, score = 0;
    for(int i=0; i<len; i++) { sx[i] = 25 - i; sy[i] = 12; } fx = 30; fy = 10;
    
    uint32_t last_move_tick = 0;

    while(1) {
        // UI updates are now handled by timer interrupt!
        
        uint8_t sc = k_get_scancode();
        if (sc != 0) {
            if (sc == 0x01) break; // ESC
            if ((sc == 0x11 || sc == 0x48) && dir != 2) dir = 0; // W / UP
            if ((sc == 0x1F || sc == 0x50) && dir != 0) dir = 2; // S / DOWN
            if ((sc == 0x1E || sc == 0x4B) && dir != 1) dir = 3; // A / LEFT
            if ((sc == 0x20 || sc == 0x4D) && dir != 3) dir = 1; // D / RIGHT
        }

        // Kontrol kecepatan ular pake timer ticks (bukan busy loop)
        uint32_t current_tick = get_ticks();
        uint32_t speed = 8 - (len > 20 ? 5 : len / 4); // Makin panjang makin dikit tick jedanya
        
        if (current_tick - last_move_tick >= speed) {
            last_move_tick = current_tick;

            for(int i=len-1; i>0; i--) { sx[i] = sx[i-1]; sy[i] = sy[i-1]; }
            if(dir==0) sy[0]--; else if(dir==1) sx[0]++; else if(dir==2) sy[0]++; else if(dir==3) sx[0]--;
            
            if(sx[0] < 16 || sx[0] > 63 || sy[0] < 6 || sy[0] > 18) break;
            if(sx[0] == fx && sy[0] == fy) { score += 10; len++; fx = 16 + (rand()%47); fy = 6 + (rand()%12); beep(); }
            
            // Draw
            for(int y=6; y<19; y++) for(int x=16; x<64; x++) d_char(x,y,' ',0x0F);
            d_char(fx, fy, '*', 0x0E);
            for(int i=0; i<len; i++) d_char(sx[i], sy[i], (i==0?'0':'o'), 0x0A);
        }
        
        __asm__ __volatile__ ("hlt"); // Irit CPU, bangun pas ada interrupt aja
    }
    beep(); c_work(); print("Game Over! Skor: ", 0x0E); p_int(score, 0x0A); print("\n", 0x0F);
}
