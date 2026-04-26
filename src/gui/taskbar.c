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

static void draw_taskbar_icon(int ix, int iy, const char* title, int minimized) {
    // Dim colors if minimized
    uint32_t c_bg = minimized ? 0x00888888 : 0x00FFFFFF;
    uint32_t c_blk = minimized ? 0x00444444 : 0x00000000;
    uint32_t c_grn = minimized ? 0x00008800 : 0x0000FF00;

    if (strncmp(title, "Terminal", 8) == 0) {
        draw_rect(ix, iy, 16, 16, c_blk);
        draw_rect_border(ix, iy, 16, 16, 0x00444444);
        draw_string_px(ix + 2, iy + 4, ">_", c_grn, c_blk);
    } else if (strncmp(title, "File Expl", 9) == 0) {
        draw_rect(ix, iy + 4, 6, 4, minimized ? 0x00AA8800 : 0x00FFCC00);
        draw_rect(ix, iy + 8, 16, 8, minimized ? 0x00AA9922 : 0x00FFDD33);
        draw_rect_border(ix, iy + 8, 16, 8, 0x00CCAA00);
    } else if (strncmp(title, "System In", 9) == 0) {
        draw_rect(ix + 2, iy + 2, 12, 12, 0x00111111);
        draw_rect(ix + 4, iy, 2, 2, 0x00CCCCCC); draw_rect(ix + 10, iy, 2, 2, 0x00CCCCCC);
        draw_rect(ix + 4, iy + 14, 2, 2, 0x00CCCCCC); draw_rect(ix + 10, iy + 14, 2, 2, 0x00CCCCCC);
        draw_rect(ix, iy + 4, 2, 2, 0x00CCCCCC); draw_rect(ix, iy + 10, 2, 2, 0x00CCCCCC);
        draw_rect(ix + 14, iy + 4, 2, 2, 0x00CCCCCC); draw_rect(ix + 14, iy + 10, 2, 2, 0x00CCCCCC);
    } else if (strncmp(title, "Clock", 5) == 0) {
        draw_rect(ix, iy, 16, 16, c_bg);
        draw_rect_border(ix, iy, 16, 16, 0x00222222);
        draw_rect(ix + 7, iy + 3, 2, 5, c_blk);
        draw_rect(ix + 7, iy + 7, 5, 2, c_blk);
    } else if (strncmp(title, "PCI", 3) == 0) {
        draw_rect(ix, iy + 4, 16, 8, minimized ? 0x00002200 : 0x00004400);
        draw_rect(ix + 4, iy + 6, 8, 4, 0x00111111);
    } else if (strncmp(title, "Mini Brow", 9) == 0 || strncmp(title, "Browser", 7) == 0) {
        draw_rect(ix, iy, 16, 16, c_bg);
        draw_rect_border(ix, iy, 16, 16, 0x000000FF);
        draw_rect(ix + 4, iy + 4, 8, 2, 0x000000FF);
        draw_rect(ix + 4, iy + 8, 8, 2, 0x000000FF);
    } else if (strncmp(title, "Snake", 5) == 0) {
        draw_rect(ix + 4, iy + 4, 4, 4, c_blk);
        draw_rect(ix + 8, iy + 4, 4, 8, c_blk);
        draw_rect(ix + 4, iy + 12, 4, 4, minimized ? 0x00880000 : 0x00FF0000);
    } else {
        // Default window icon
        draw_rect(ix, iy, 16, 16, c_bg);
        draw_rect(ix, iy, 16, 4, 0x000000AA);
        draw_rect_border(ix, iy, 16, 16, 0x00000000);
    }
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
    unsigned char dow  = read_cmos(0x06); // Day of week (1-7)
    
    // Convert to WIB (UTC+7)
    int h = hour + 7;
    if (h >= 24) {
        dow++;
        if (dow > 7) dow = 1;
    }
    hour = h % 24;
    
    const char* days[] = {"???", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if (dow > 7) dow = 0;

    int cx2 = (int)(fb_width / 2) - 60;
    draw_string_px(cx2,      ty + 6, days[dow], GUI_TEXT_INV, GUI_TASKBAR);
    draw_num2(cx2 + 30, ty + 6, hour, GUI_TEXT_INV, GUI_TASKBAR);
    draw_string_px(cx2 + 46, ty + 6, ":", GUI_TEXT_INV, GUI_TASKBAR);
    draw_num2(cx2 + 52, ty + 6, min,  GUI_TEXT_INV, GUI_TASKBAR);
    draw_string_px(cx2 + 68, ty + 6, ":", GUI_TEXT_INV, GUI_TASKBAR);
    draw_num2(cx2 + 74, ty + 6, sec,  GUI_BORDER,   GUI_TASKBAR);

    // Window buttons
    int wx = 70;
    for (int i = 0; i < MAX_WINDOWS && wx < (int)fb_width - 200; i++) {
        if (!wm_wins[i].visible) continue;
        int focused = (wm_focused == wm_wins[i].id) && !wm_wins[i].minimized;
        uint32_t bg2 = wm_wins[i].minimized ? GUI_BORDER2 : (focused ? GUI_BTN_HOV : GUI_BTN);
        
        // Draw 32x24 button
        draw_rect(wx, ty + 2, 32, TASKBAR_H_PX - 4, bg2);
        draw_rect_border(wx, ty + 2, 32, TASKBAR_H_PX - 4, focused ? GUI_BORDER : GUI_BORDER2);
        
        // Draw 16x16 icon centered in the 32x24 button
        draw_taskbar_icon(wx + 8, ty + 6, wm_wins[i].title, wm_wins[i].minimized);
        
        wx += 38; // 32px width + 6px margin
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
        if (mx >= wx && mx < wx + 32) { // 32px width
            if (wm_wins[i].minimized) {
                // Restore from minimize
                wm_wins[i].minimized = 0;
                wm_raise(wm_wins[i].id);
            } else if (wm_focused == wm_wins[i].id) {
                // Click focused window again = minimize
                wm_wins[i].minimized = 1;
                wm_focused = -1;
                for (int zz = wm_zcount - 1; zz >= 0; zz--) {
                    WmWin* nw = &wm_wins[wm_zorder[zz]];
                    if (nw->visible && !nw->minimized) {
                        wm_focused = nw->id;
                        break;
                    }
                }
            } else {
                // Bring to front
                wm_raise(wm_wins[i].id);
            }
            return;
        }
        wx += 38; // 32px width + 6px margin
    }
}

void taskbar_tick() { /* clock updates on redraw */ }
