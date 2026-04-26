#include "../include/taskbar.h"
#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/mem.h"
#include "../include/wm.h"
#include "../include/timer.h"
#include "../include/keyboard.h"
#include "../include/ata.h"

// Helpers
static void draw_num2(int px, int py, unsigned char n, uint32_t fg, uint32_t bg) {
    char buf[3]; buf[0] = '0' + n/10; buf[1] = '0' + n%10; buf[2] = '\0';
    draw_string_px(px, py, buf, fg, bg);
}

int start_menu_open = 0;

void taskbar_draw() {
    if (!is_vbe) return;
    int ty = (int)fb_height - TASKBAR_H_PX;
    // Background + top border
    draw_rect(0, ty, fb_width, TASKBAR_H_PX, GUI_TASKBAR);
    draw_rect(0, ty, fb_width, 1, GUI_BORDER);

    // "Mectov" logo button on the left
    uint32_t btn_bg = start_menu_open ? GUI_BTN_HOV : GUI_BTN;
    draw_rect(2, ty + 2, 60, TASKBAR_H_PX - 4, btn_bg);
    draw_rect_border(2, ty + 2, 60, TASKBAR_H_PX - 4, GUI_BORDER);
    draw_string_px(8, ty + 6, "Mectov", GUI_TEXT, btn_bg);

    // RAM bar (right side)
    unsigned int used_kb = get_used_memory() / 1024;
    unsigned int tot_kb  = get_total_memory() / 1024;
    draw_string_px((int)fb_width - 140, ty + 6, "RAM:", GUI_TEXT_INV, GUI_TASKBAR);
    int bar_x = (int)fb_width - 108, bar_w = 80;
    draw_rect(bar_x - 1, ty + 9, bar_w + 2, 10, GUI_BORDER2);
    if (tot_kb > 0) {
        int fill = (int)(bar_w * (used_kb / (tot_kb / 100 + 1)) / 100);
        if (fill > bar_w) fill = bar_w;
        draw_rect(bar_x, ty + 10, fill, 8, GUI_CYAN);
    }

    // System Tray (Left of the Clock)
    int tray_x = (int)fb_width - 240;
    
    // Caps Lock indicator
    if (caps_a) {
        draw_rect(tray_x, ty + 6, 40, 16, 0x00FF8800); // Orange bg
        draw_string_px(tray_x + 4, ty + 10, "CAPS", 0x00FFFFFF, 0x00FF8800);
    } else {
        draw_rect(tray_x, ty + 6, 40, 16, GUI_BORDER);
        draw_string_px(tray_x + 4, ty + 10, "caps", 0x00888888, GUI_BORDER);
    }
    
    // HDD indicator
    if (hdd_activity > 0) {
        draw_rect(tray_x + 46, ty + 8, 12, 12, 0x00FF0000); // Red light
        hdd_activity--;
    } else {
        draw_rect(tray_x + 46, ty + 8, 12, 12, 0x00222222); // Dark light
    }
    draw_string_px(tray_x + 62, ty + 10, "HDD", GUI_TEXT_INV, GUI_TASKBAR);

    // Clock (center)
    unsigned char sec  = bcd_to_bin(read_cmos(0x00));
    unsigned char min  = bcd_to_bin(read_cmos(0x02));
    unsigned char hour = bcd_to_bin(read_cmos(0x04));
    hour = (hour + 7) % 24; // WIB (UTC+7)
    
    int cx2 = (int)(fb_width / 2) - 36;
    draw_num2(cx2,      ty + 6, hour, GUI_TEXT_INV, GUI_TASKBAR);
    draw_string_px(cx2 + 16, ty + 6, ":", GUI_TEXT_INV, GUI_TASKBAR);
    draw_num2(cx2 + 22, ty + 6, min,  GUI_TEXT_INV, GUI_TASKBAR);
    draw_string_px(cx2 + 38, ty + 6, ":", GUI_TEXT_INV, GUI_TASKBAR);
    draw_num2(cx2 + 44, ty + 6, sec,  GUI_BORDER,   GUI_TASKBAR);

    // Window buttons
    int wx = 70;
    for (int i = 0; i < MAX_WINDOWS && wx < (int)fb_width - 200; i++) {
        if (!wm_wins[i].visible) continue;
        int focused = (wm_focused == wm_wins[i].id);
        uint32_t bg2 = focused ? GUI_BTN_HOV : GUI_BTN;
        draw_rect(wx, ty + 2, 90, TASKBAR_H_PX - 4, bg2);
        draw_rect_border(wx, ty + 2, 90, TASKBAR_H_PX - 4, focused ? GUI_BORDER : GUI_BORDER2);
        char buf[10]; int k;
        for (k = 0; k < 9 && wm_wins[i].title[k]; k++) buf[k] = wm_wins[i].title[k];
        buf[k] = '\0';
        draw_string_px(wx + 4, ty + 6, buf, focused ? GUI_TEXT : GUI_DIM, bg2);
        wx += 96;
    }

    // Draw Start Menu if open
    if (start_menu_open) {
        int sm_h = 160;
        int sm_w = 160;
        int sm_y = ty - sm_h;
        draw_rect(2, sm_y, sm_w, sm_h, GUI_BG);
        draw_rect_border(2, sm_y, sm_w, sm_h, GUI_BORDER);
        
        draw_rect(2, sm_y, sm_w, 24, GUI_BTN);
        draw_string_px(10, sm_y + 4, "Mectov OS", GUI_TEXT, GUI_BTN);
        draw_rect(2, sm_y + 24, sm_w, 1, GUI_BORDER);

        draw_string_px(14, sm_y + 34, "Terminal", GUI_TEXT, GUI_BG);
        draw_string_px(14, sm_y + 58, "Editor", GUI_TEXT, GUI_BG);
        draw_string_px(14, sm_y + 82, "System Info", GUI_TEXT, GUI_BG);
        draw_string_px(14, sm_y + 106, "Clock", GUI_TEXT, GUI_BG);
        draw_string_px(14, sm_y + 130, "Power", 0x00FF4444, GUI_BG); // Red power text
    }
}

void taskbar_handle_click(int mx, int my) {
    int ty = (int)fb_height - TASKBAR_H_PX;
    if (my < ty) return;
    
    // Mectov button click
    if (mx >= 2 && mx <= 62) {
        start_menu_open = !start_menu_open;
        return;
    }
    
    int wx = 70;
    for (int i = 0; i < MAX_WINDOWS && wx < (int)fb_width - 200; i++) {
        if (!wm_wins[i].visible) continue;
        if (mx >= wx && mx < wx + 90) { wm_raise(wm_wins[i].id); return; }
        wx += 96;
    }
}

void taskbar_tick() { /* clock updates on redraw */ }
