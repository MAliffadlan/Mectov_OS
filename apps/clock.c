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

static int win_cw = 240 - 2;
static int win_ch = 120 - 22;

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
    
    sys_draw_rect(wid, 0, 0, win_cw, win_ch, 0x001E1E2E); // GUI_BG
    
    // Draw time (centered: defaults 85/80 at win_cw=238)
    sys_draw_text(wid, win_cw / 2 - 34, 30, tbuf, 0x00CDD6F4);
    
    // Draw date
    sys_draw_text(wid, win_cw / 2 - 39, 60, dbuf, 0x006C7086);
    
    // Draw decorative border (defaults: w=220=win_cw-18, btm=109=win_ch+11,
    // right=229=win_cw-9, h=100=win_ch+2 at win_cw=238, win_ch=98)
    sys_draw_rect(wid, 10, 10, win_cw - 18, 1, 0x00313144);
    sys_draw_rect(wid, 10, win_ch + 11, win_cw - 18, 1, 0x00313144);
    sys_draw_rect(wid, 10, 10, 1, win_ch + 2, 0x00313144);
    sys_draw_rect(wid, win_cw - 9, 10, 1, win_ch + 2, 0x00313144);
    
    sys_draw_text(wid, 15, win_ch - 8, "User Space App", 0x0094E2D5);
    
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
            } else if (ev.type == 5) { // Client size (WM reports real cw/ch)
                if (ev.x > 40 && ev.y > 40) {
                    win_cw = ev.x;
                    win_ch = ev.y;
                    draw_clock(wid);
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
