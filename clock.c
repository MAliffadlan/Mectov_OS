#include "src/include/syscall.h"

// Basic stdlib replacements
static void itoa_pad(int val, char* buf, int pad) {
    char temp[12];
    int len = 0;
    if (val == 0) temp[len++] = '0';
    while (val > 0) { temp[len++] = '0' + (val % 10); val /= 10; }
    
    int pos = 0;
    while (pos < pad - len) buf[pos++] = '0';
    for (int i = len - 1; i >= 0; i--) buf[pos++] = temp[i];
    buf[pos] = '\0';
}

typedef struct {
    int type; // 1 = paint, 2 = key, 3 = mouse
    int x, y;
    int key;
} gui_event_t;

static void draw_clock(int wid) {
    rtc_time_t tm;
    sys_get_time(&tm);
    
    int hour = (tm.hour + 7) % 24; // WIB
    
    char tbuf[16];
    itoa_pad(hour, tbuf, 2);
    tbuf[2] = ':';
    itoa_pad(tm.minute, tbuf + 3, 2);
    tbuf[5] = ':';
    itoa_pad(tm.second, tbuf + 6, 2);
    tbuf[8] = '\0';
    
    char dbuf[16];
    itoa_pad(tm.year, dbuf, 4);
    dbuf[4] = '-';
    itoa_pad(tm.month, dbuf + 5, 2);
    dbuf[7] = '-';
    itoa_pad(tm.day, dbuf + 8, 2);
    dbuf[10] = '\0';
    
    sys_draw_rect(wid, 0, 0, 240, 120, 0x001E1E2E); // GUI_BG
    
    // Draw time
    sys_draw_text(wid, 85, 30, tbuf, 0x00CDD6F4);
    
    // Draw date
    sys_draw_text(wid, 80, 60, dbuf, 0x006C7086);
    
    // Draw decorative border
    sys_draw_rect(wid, 10, 10, 220, 1, 0x00313144);
    sys_draw_rect(wid, 10, 109, 220, 1, 0x00313144);
    sys_draw_rect(wid, 10, 10, 1, 100, 0x00313144);
    sys_draw_rect(wid, 229, 10, 1, 100, 0x00313144);
    
    sys_draw_text(wid, 15, 90, "User Space App", 0x0094E2D5);
    
    sys_update_window(wid);
}

void _start() {
    int wid = sys_create_window(200, 150, 240, 120, "Clock (Ring 3)");
    if (wid < 0) sys_exit();
    
    draw_clock(wid); // Gambar pertama kali
    
    gui_event_t ev;
    int tick = 0;
    
    while (1) {
        // Process events
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) { // Paint event
                draw_clock(wid);
            } else if (ev.type == 2) { // Key event
                if (ev.key == 27) { // ESC ASCII
                    sys_exit();
                }
            }
        }
        
        tick++;
        if (tick % 5000 == 0) { // Lebih cepat update-nya biar detiknya gak lag
            draw_clock(wid);
        }
        
        sys_yield();
    }
}
