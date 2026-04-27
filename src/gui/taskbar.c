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
int calendar_open = 0;

static int get_dow(int d, int m, int y) {
    if (m < 3) { m += 12; y -= 1; }
    int k = y % 100;
    int j = y / 100;
    int h = (d + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    return (h + 6) % 7; // 0=Sun, 1=Mon, 2=Tue, 3=Wed, 4=Thu, 5=Fri, 6=Sat
}

static int days_in_month(int m, int y) {
    int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2) {
        if (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) return 29;
        return 28;
    }
    return days[m - 1];
}

void taskbar_draw() {
    if (!is_vbe) return;
    int ty = (int)fb_height - TASKBAR_H_PX;
    // Background + top border
    draw_rect(0, ty, fb_width, TASKBAR_H_PX, GUI_TASKBAR);
    draw_rect(0, ty, fb_width, 1, GUI_BORDER);

    // "MectovOS" logo button on the left
    uint32_t btn_bg = start_menu_open ? GUI_BTN_HOV : GUI_BTN;
    draw_rect(2, ty + 2, 76, TASKBAR_H_PX - 4, btn_bg);
    draw_rect_border(2, ty + 2, 76, TASKBAR_H_PX - 4, GUI_BORDER);
    draw_string_px(8, ty + 6, "MectovOS", GUI_TEXT, btn_bg);

    // System Tray (Right side before edge)
    int tray_end = (int)fb_width - 10;
    
    // 1. RAM Bar
    unsigned int used_kb = get_used_memory() / 1024;
    unsigned int tot_kb  = get_total_memory() / 1024;
    int ram_pct = (tot_kb > 0) ? (used_kb * 100 / tot_kb) : 0;
    
    int bar_w = 60;
    int bar_x = tray_end - bar_w;
    draw_rect(bar_x - 1, ty + 9, bar_w + 2, 10, GUI_BORDER2);
    if (tot_kb > 0) {
        int fill = (bar_w * ram_pct) / 100;
        if (fill > bar_w) fill = bar_w;
        draw_rect(bar_x, ty + 10, fill, 8, GUI_CYAN);
    }
    
    char pct_buf[16];
    int p_idx = 0;
    if (ram_pct >= 100) { pct_buf[p_idx++] = '1'; pct_buf[p_idx++] = '0'; pct_buf[p_idx++] = '0'; }
    else {
        if (ram_pct >= 10) pct_buf[p_idx++] = '0' + (ram_pct / 10);
        pct_buf[p_idx++] = '0' + (ram_pct % 10);
    }
    pct_buf[p_idx++] = '%'; 
    pct_buf[p_idx] = '\0';
    
    int ram_txt_x = bar_x - 40;
    draw_string_px(ram_txt_x, ty + 6, "RAM", GUI_TEXT_INV, GUI_TASKBAR);
    
    // 2. HDD Indicator
    int hdd_x = ram_txt_x - 50;
    draw_string_px(hdd_x, ty + 6, "HDD", GUI_TEXT_INV, GUI_TASKBAR);
    if (hdd_activity > 0) {
        draw_rect(hdd_x + 30, ty + 8, 10, 10, 0x00FF0000); // Red light
        hdd_activity--;
    } else {
        draw_rect(hdd_x + 30, ty + 8, 10, 10, 0x00222222); // Dark light
    }
    
    // Separator line
    draw_rect(hdd_x - 10, ty + 6, 1, 16, GUI_BORDER2);
    
    // 3. Caps Lock indicator
    int caps_x = hdd_x - 50;
    if (caps_a) {
        draw_rect(caps_x, ty + 6, 32, 16, 0x00FF8800); // Orange bg
        draw_string_px(caps_x + 4, ty + 10, "CAP", 0x00FFFFFF, 0x00FF8800);
    } else {
        draw_rect(caps_x, ty + 6, 32, 16, GUI_BORDER);
        draw_string_px(caps_x + 4, ty + 10, "cap", 0x00888888, GUI_BORDER);
    }

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
    uint32_t clk_bg = calendar_open ? GUI_BTN_HOV : GUI_TASKBAR;
    draw_rect(cx2 - 4, ty + 2, 106, TASKBAR_H_PX - 4, clk_bg); // Clock button background

    draw_string_px(cx2,      ty + 6, days[dow], GUI_TEXT_INV, clk_bg);
    draw_num2(cx2 + 30, ty + 6, hour, GUI_TEXT_INV, clk_bg);
    draw_string_px(cx2 + 46, ty + 6, ":", GUI_TEXT_INV, clk_bg);
    draw_num2(cx2 + 52, ty + 6, min,  GUI_TEXT_INV, clk_bg);
    draw_string_px(cx2 + 68, ty + 6, ":", GUI_TEXT_INV, clk_bg);
    draw_num2(cx2 + 74, ty + 6, sec,  GUI_BORDER,   clk_bg);

    // Window buttons
    int wx = 86;
    for (int i = 0; i < MAX_WINDOWS && wx < (int)fb_width - 200; i++) {
        if (!wm_wins[i].visible) continue;
        int focused = (wm_focused == wm_wins[i].id) && !wm_wins[i].minimized;
        uint32_t bg2 = wm_wins[i].minimized ? GUI_BORDER2 : (focused ? GUI_BTN_HOV : GUI_BTN);
        
        draw_rect(wx, ty + 2, 32, TASKBAR_H_PX - 4, bg2);
        draw_rect_border(wx, ty + 2, 32, TASKBAR_H_PX - 4, focused ? GUI_BORDER : GUI_BORDER2);
        draw_taskbar_icon(wx + 8, ty + 6, wm_wins[i].title, wm_wins[i].minimized);
        
        wx += 38;
    }

    // Draw Start Menu if open
    if (start_menu_open) {
        int sm_h = 267;
        int sm_w = 160;
        int sm_y = ty - sm_h;
        draw_rect(2, sm_y, sm_w, sm_h, GUI_BG);
        draw_rect_border(2, sm_y, sm_w, sm_h, GUI_BORDER);
        
        draw_rect(2, sm_y, sm_w, 24, GUI_BTN);
        draw_string_px(10, sm_y + 4, "MectovOS Apps", GUI_TEXT, GUI_BTN);
        draw_rect(2, sm_y + 24, sm_w, 1, GUI_BORDER);

        int oy = sm_y + 25;
        draw_string_px(14, oy + 4, "Terminal", GUI_TEXT, GUI_BG); oy += 24;
        draw_string_px(14, oy + 4, "Editor (Nano)", GUI_TEXT, GUI_BG); oy += 24;
        draw_string_px(14, oy + 4, "File Explorer", GUI_TEXT, GUI_BG); oy += 24;
        draw_string_px(14, oy + 4, "Mini Browser", GUI_TEXT, GUI_BG); oy += 24;
        draw_string_px(14, oy + 4, "System Info", GUI_TEXT, GUI_BG); oy += 24;
        draw_string_px(14, oy + 4, "Clock", GUI_TEXT, GUI_BG); oy += 24;
        draw_string_px(14, oy + 4, "PCI Manager", GUI_TEXT, GUI_BG); oy += 24;
        draw_string_px(14, oy + 4, "Snake Game", GUI_TEXT, GUI_BG); oy += 24;
        
        draw_rect(2, oy, sm_w, 1, GUI_BORDER); // Separator
        oy += 1;
        draw_string_px(14, oy + 4, "Shut Down", 0x00CC0000, GUI_BG); oy += 24;
        draw_string_px(14, oy + 4, "Restart", 0x00DD6600, GUI_BG);
    }

    // Draw Calendar if open
    if (calendar_open) {
        unsigned char day_cmos = bcd_to_bin(read_cmos(0x07));
        unsigned char mon_cmos = bcd_to_bin(read_cmos(0x08));
        unsigned char yr_cmos  = bcd_to_bin(read_cmos(0x09));
        int c_yr = 2000 + yr_cmos;
        
        int cal_w = 200;
        int cal_h = 160;
        int cal_x = cx2 - 40;
        int cal_y = ty - cal_h;
        
        draw_rect(cal_x, cal_y, cal_w, cal_h, GUI_BG);
        draw_rect_border(cal_x, cal_y, cal_w, cal_h, GUI_BORDER);
        
        draw_rect(cal_x, cal_y, cal_w, 24, GUI_BTN);
        
        const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char date_str[32];
        int idx = 0;
        if (day_cmos >= 10) date_str[idx++] = '0' + (day_cmos / 10);
        date_str[idx++] = '0' + (day_cmos % 10);
        date_str[idx++] = ' ';
        const char* ms = months[(mon_cmos - 1) % 12];
        while (*ms) date_str[idx++] = *ms++;
        date_str[idx++] = ' ';
        date_str[idx++] = '2'; date_str[idx++] = '0';
        date_str[idx++] = '0' + (yr_cmos / 10);
        date_str[idx++] = '0' + (yr_cmos % 10);
        date_str[idx] = '\0';
        
        draw_string_px(cal_x + 50, cal_y + 4, date_str, GUI_TEXT, GUI_BTN);
        
        // Days header
        const char* d_labels[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
        for (int i = 0; i < 7; i++) {
            draw_string_px(cal_x + 10 + i * 26, cal_y + 30, d_labels[i], 0x00888888, GUI_BG);
        }
        
        int start_dow = get_dow(1, mon_cmos, c_yr);
        int total_days = days_in_month(mon_cmos, c_yr);
        
        int row = 0;
        for (int d = 1; d <= total_days; d++) {
            int col = (start_dow + d - 1) % 7;
            char nbuf[3];
            if (d < 10) { nbuf[0] = ' '; nbuf[1] = '0' + d; }
            else { nbuf[0] = '0' + (d / 10); nbuf[1] = '0' + (d % 10); }
            nbuf[2] = '\0';
            
            uint32_t c_fg = GUI_TEXT;
            if (d == day_cmos) {
                draw_rect(cal_x + 6 + col * 26, cal_y + 50 + row * 20 - 2, 22, 18, GUI_BTN_HOV);
                c_fg = GUI_WHITE;
            }
            
            draw_string_px(cal_x + 10 + col * 26, cal_y + 50 + row * 20, nbuf, c_fg, (d == day_cmos) ? GUI_BTN_HOV : GUI_BG);
            
            if (col == 6) row++;
        }
    }
}

void taskbar_handle_click(int mx, int my) {
    int ty = (int)fb_height - TASKBAR_H_PX;
    if (my < ty) return;
    
    // MectovOS button click
    if (mx >= 2 && mx <= 78) {
        start_menu_open = !start_menu_open;
        if (start_menu_open) calendar_open = 0;
        return;
    }
    
    int cx2 = (int)(fb_width / 2) - 60;
    if (mx >= cx2 - 4 && mx <= cx2 + 102) {
        calendar_open = !calendar_open;
        if (calendar_open) start_menu_open = 0;
        return;
    }
    
    int wx = 86;
    for (int i = 0; i < MAX_WINDOWS && wx < (int)fb_width - 200; i++) {
        if (!wm_wins[i].visible) continue;
        if (mx >= wx && mx < wx + 32) {
            if (wm_wins[i].minimized) {
                wm_wins[i].minimized = 0;
                wm_raise(wm_wins[i].id);
            } else if (wm_focused == wm_wins[i].id) {
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
                wm_raise(wm_wins[i].id);
            }
            return;
        }
        wx += 38;
    }
}

void taskbar_tick() { }
