#include "src/include/syscall.h"

void main() {
    sys_print("[App] External App running from VFS at 32MB mark!\n", 0x0A);
    
    int tick = 0;
    while (1) {
        uint32_t color = (tick & 1) ? 0xFF0000 : 0x0000FF;
        
        // Gambar di tengah layar (400, 300) biar kelihatan
        sys_draw_rect(400, 300, 100, 50, color);
        sys_draw_text(410, 315, "EXTERNAL!", 0xFFFFFF, 0x000000);
        
        tick++;
        // Ngasih tau CPU kalau aplikasi lagi standby, biar FPS sistem nggak turun
        sys_yield();
        for (volatile int i = 0; i < 500000; i++);
    }
}
