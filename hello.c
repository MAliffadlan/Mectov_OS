#include "src/include/syscall.h"

// Define gui_event_t locally for the app
typedef struct {
    int type; // 1 = paint, 2 = key, 3 = mouse
    int x, y;
    int key;
} gui_event_t;

void _start() {
    sys_print("[App] Hello App starting in Ring 3...\n", 0x0A);
    
    int wid = sys_create_window(100, 100, 300, 200, "Hello Ring 3");
    if (wid < 0) sys_exit();
    
    gui_event_t ev;
    int tick = 0;
    
    while (1) {
        // Process all pending events
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) { // Paint event
                uint32_t bg = (tick & 1) ? 0x222222 : 0x333333;
                sys_draw_rect(wid, 0, 0, 300, 200, bg);
                sys_draw_text(wid, 50, 80, "Hello from User Space!", 0x00FF00);
                sys_draw_text(wid, 50, 100, "Memory is isolated.", 0xFFFFFF);
                sys_update_window(wid);
            } else if (ev.type == 2) { // Key event
                if (ev.key == 0x01) { // ESC key (scancode 1)
                    sys_exit();
                }
            }
        }
        
        tick++;
        if (tick % 100000 == 0) {
            // Force repaint every now and then
            uint32_t bg = ((tick/100000) & 1) ? 0x222222 : 0x333333;
            sys_draw_rect(wid, 0, 0, 300, 200, bg);
            sys_draw_text(wid, 50, 80, "Hello from User Space!", 0x00FF00);
            sys_draw_text(wid, 50, 100, "Memory is isolated.", 0xFFFFFF);
            sys_update_window(wid);
        }
        
        sys_yield();
    }
}
