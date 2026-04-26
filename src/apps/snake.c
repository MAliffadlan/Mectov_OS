#include "../include/apps.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/speaker.h"
#include "../include/utils.h"
#include "../include/mem.h"
#include "../include/io.h"

void start_ular() {
    c_work(); 
    d_win(15, 5, 50, 15, " Mectov Ular v1.2 - WASD / Arrows ");
    
    int sx[100], sy[100], len = 3, fx, fy, dir = 1, score = 0;
    for(int i=0; i<len; i++) { sx[i] = 25 - i; sy[i] = 12; } 
    fx = 30; fy = 10;
    
    while(1) {
        update_marquee(); 
        update_hud();
        
        int ms_delay = 80 - (len > 30 ? 60 : len);
        int exit_game = 0;
        for (volatile int d_i = 0; d_i < ms_delay; d_i++) {
            if (inb(0x64) & 1) {
                unsigned char sc = inb(0x60);
                if (sc == 0x01) { exit_game = 1; break; } 
                if ((sc == 0x11 || sc == 0x48) && dir != 2) dir = 0; // W / Up
                if ((sc == 0x1F || sc == 0x50) && dir != 0) dir = 2; // S / Down
                if ((sc == 0x1E || sc == 0x4B) && dir != 1) dir = 3; // A / Left
                if ((sc == 0x20 || sc == 0x4D) && dir != 3) dir = 1; // D / Right
            }
            for (volatile int d_j = 0; d_j < 4000; d_j++) __asm__ __volatile__ ("pause");
        }
        if (exit_game) break;

        for(int i=len-1; i>0; i--) { sx[i] = sx[i-1]; sy[i] = sy[i-1]; }
        if(dir==0) sy[0]--; else if(dir==1) sx[0]++; else if(dir==2) sy[0]++; else if(dir==3) sx[0]--;
        
        if(sx[0] < 16 || sx[0] > 63 || sy[0] < 6 || sy[0] > 18) break;
        if(sx[0] == fx && sy[0] == fy) { 
            score += 10; len++; 
            fx = 16 + (rand()%47); fy = 6 + (rand()%12); 
            beep(); 
        }
        
        for(int y=6; y<19; y++) for(int x=16; x<64; x++) d_char(x,y,' ',0x0F);
        d_char(fx, fy, '*', 0x0E);
        for(int i=0; i<len; i++) d_char(sx[i], sy[i], (i==0?'0':'o'), 0x0A);
    }
    beep(); 
    c_work(); 
    print("Game Over! Skor: ", 0x0E); 
    p_int(score, 0x0A); 
    print("\n", 0x0F);
}
