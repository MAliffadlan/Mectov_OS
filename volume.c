#include "src/include/syscall.h"

typedef struct {
    int type;
    int x, y;
    int key;
} gui_event_t;

// --- Helpers ---
static void itoa(int val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char temp[12]; int len = 0;
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    while (val > 0) { temp[len++] = '0' + (val % 10); val /= 10; }
    int pos = 0;
    if (neg) buf[pos++] = '-';
    for (int i = len - 1; i >= 0; i--) buf[pos++] = temp[i];
    buf[pos] = '\0';
}

#define WIN_W   260
#define WIN_H   180

static int volume = 80;
static int dragging = 0;

// Slider geometry
#define SL_X    30
#define SL_Y    70
#define SL_W    200
#define SL_H    8
#define KNOB_R  10

static void draw_volume(int wid) {
    // Background
    sys_draw_rect(wid, 0, 0, WIN_W, WIN_H, 0x001A1A2E);
    
    // Title
    sys_draw_text(wid, WIN_W/2 - 52, 12, "Volume Control", 0x00FFFFFF);
    
    // Volume icon
    if (volume == 0) {
        sys_draw_text(wid, SL_X - 20, SL_Y - 6, "X", 0x00FF5F56);
    } else if (volume < 33) {
        sys_draw_text(wid, SL_X - 20, SL_Y - 6, ")", 0x0027C93F);
    } else if (volume < 66) {
        sys_draw_text(wid, SL_X - 20, SL_Y - 6, "))", 0x0027C93F);
    } else {
        sys_draw_text(wid, SL_X - 20, SL_Y - 6, ")))", 0x0027C93F);
    }
    
    // Slider track (dark bg)
    sys_draw_rect(wid, SL_X, SL_Y, SL_W, SL_H, 0x00333344);
    
    // Filled portion (green gradient feel)
    int fill_w = (volume * SL_W) / 100;
    if (fill_w > 0) {
        sys_draw_rect(wid, SL_X, SL_Y, fill_w, SL_H, 0x0027C93F);
    }
    
    // Knob
    int knob_x = SL_X + fill_w;
    int knob_y = SL_Y + SL_H / 2;
    // Draw a filled square as knob (no circle syscall)
    sys_draw_rect(wid, knob_x - KNOB_R, knob_y - KNOB_R, KNOB_R*2, KNOB_R*2, 0x00FFFFFF);
    sys_draw_rect(wid, knob_x - KNOB_R + 2, knob_y - KNOB_R + 2, KNOB_R*2 - 4, KNOB_R*2 - 4, 0x0027C93F);
    
    // Volume percentage text
    char buf[16];
    itoa(volume, buf);
    // Append %
    int i = 0;
    while (buf[i]) i++;
    buf[i] = '%'; buf[i+1] = '\0';
    
    sys_draw_text(wid, WIN_W/2 - 16, SL_Y + 28, buf, 0x00FFFFFF);
    
    // Buttons: [-] and [+]
    sys_draw_rect(wid, SL_X, SL_Y + 52, 40, 28, 0x00333344);
    sys_draw_text(wid, SL_X + 14, SL_Y + 58, "-", 0x00FF5F56);
    
    sys_draw_rect(wid, SL_X + SL_W - 40, SL_Y + 52, 40, 28, 0x00333344);
    sys_draw_text(wid, SL_X + SL_W - 26, SL_Y + 58, "+", 0x0027C93F);
    
    // Mute button
    sys_draw_rect(wid, WIN_W/2 - 30, SL_Y + 52, 60, 28, 0x00333344);
    if (volume == 0) {
        sys_draw_text(wid, WIN_W/2 - 26, SL_Y + 58, "Unmute", 0x00FFBD2E);
    } else {
        sys_draw_text(wid, WIN_W/2 - 20, SL_Y + 58, "Mute", 0x00FFBD2E);
    }
    
    sys_update_window(wid);
}

static void set_vol(int v) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    volume = v;
    sys_set_volume(volume);
}

void _start() {
    int wid = sys_create_window(300, 250, WIN_W, WIN_H, "Volume");
    if (wid < 0) sys_exit();
    
    volume = sys_get_volume();
    draw_volume(wid);
    
    gui_event_t ev;
    int prev_volume = 80; // For mute toggle
    
    while (1) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) {
                draw_volume(wid);
            } else if (ev.type == 2) {
                // Keyboard
                if (ev.key == 27) sys_exit();
                else if (ev.key == '+' || ev.key == '=') { set_vol(volume + 5); draw_volume(wid); }
                else if (ev.key == '-' || ev.key == '_') { set_vol(volume - 5); draw_volume(wid); }
                else if (ev.key == 'm' || ev.key == 'M') {
                    if (volume > 0) { prev_volume = volume; set_vol(0); }
                    else set_vol(prev_volume);
                    draw_volume(wid);
                }
            } else if (ev.type == 3) {
                // Mouse click
                int mx = ev.x;
                int my = ev.y;
                
                // Slider track area
                if (my >= SL_Y - KNOB_R && my <= SL_Y + SL_H + KNOB_R &&
                    mx >= SL_X && mx <= SL_X + SL_W) {
                    int new_vol = ((mx - SL_X) * 100) / SL_W;
                    set_vol(new_vol);
                    draw_volume(wid);
                }
                // [-] button
                else if (mx >= SL_X && mx <= SL_X + 40 &&
                         my >= SL_Y + 52 && my <= SL_Y + 80) {
                    set_vol(volume - 5);
                    sys_play_sound(400, 30);
                    draw_volume(wid);
                }
                // [+] button
                else if (mx >= SL_X + SL_W - 40 && mx <= SL_X + SL_W &&
                         my >= SL_Y + 52 && my <= SL_Y + 80) {
                    set_vol(volume + 5);
                    sys_play_sound(800, 30);
                    draw_volume(wid);
                }
                // Mute button
                else if (mx >= WIN_W/2 - 30 && mx <= WIN_W/2 + 30 &&
                         my >= SL_Y + 52 && my <= SL_Y + 80) {
                    if (volume > 0) { prev_volume = volume; set_vol(0); }
                    else set_vol(prev_volume);
                    draw_volume(wid);
                }
            } else if (ev.type == 4) { // Scroll wheel
                if (ev.key > 0) {
                    set_vol(volume + 5);
                } else if (ev.key < 0) {
                    set_vol(volume - 5);
                }
                draw_volume(wid);
            }
        }
        sys_yield();
    }
}
