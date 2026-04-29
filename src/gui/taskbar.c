#include "../include/taskbar.h"
#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/mem.h"
#include "../include/wm.h"
#include "../include/timer.h"
#include "../include/keyboard.h"
#include "../include/ata.h"
#include "../include/rtc.h"

// Helpers
static void draw_num2(int px, int py, unsigned char n, uint32_t fg, uint32_t bg) {
    char buf[3]; buf[0] = '0' + n/10; buf[1] = '0' + n%10; buf[2] = '\0';
    draw_string_px(px, py, buf, fg, bg);
}

// Modern squircle icon (16x16) for taskbar buttons
static void draw_taskbar_icon(int ix, int iy, const char* title, int minimized) {
    (void)minimized; // Button state is handled by the wrapper button
    int cx = ix + 8;
    int cy = iy + 8;
    
    // Base colors (must match desktop.c exactly)
    uint32_t bg_col = 0x00FFFFFF;
    if (strncmp(title, "Terminal", 8) == 0) bg_col = 0x002D3748;
    else if (strncmp(title, "File Expl", 9) == 0) bg_col = 0x003182CE;
    else if (strncmp(title, "System In", 9) == 0) bg_col = 0x00E2E8F0;
    else if (strncmp(title, "Clock", 5) == 0) bg_col = 0x00FFFFFF;
    else if (strncmp(title, "PCI", 3) == 0) bg_col = 0x00DD6B20;
    else if (strncmp(title, "Mini Brow", 9) == 0 || strncmp(title, "Browser", 7) == 0) bg_col = 0x00319795;
    else if (strncmp(title, "Snake", 5) == 0) bg_col = 0x0038A169;
    else if (strncmp(title, "Calc", 4) == 0) bg_col = 0x00718096;
    else bg_col = 0x002D3748;

    // Draw base squircle 16x16
    draw_rounded_rect(ix, iy, 16, 16, 4, bg_col);

    // Draw inner minimalist glyphs (scaled down from desktop.c)
    if (strncmp(title, "Terminal", 8) == 0) {
        draw_string_px(ix, iy + 4, ">_", 0x0048BB78, bg_col);
    } else if (strncmp(title, "File Expl", 9) == 0) {
        draw_rect(cx - 5, cy - 4, 10, 8, 0x00FFFFFF);
        draw_rect(cx - 5, cy - 5, 4, 1, 0x00EBF8FF);
        draw_rect(cx - 5, cy - 2, 10, 1, 0x0090CDF4); 
    } else if (strncmp(title, "System In", 9) == 0) {
        draw_rect(cx - 5, cy - 4, 10, 7, 0x002D3748);
        draw_rect(cx - 4, cy - 3, 8, 5, 0x00A0AEC0); 
        draw_rect(cx - 2, cy + 3, 4, 2, 0x002D3748); 
    } else if (strncmp(title, "Clock", 5) == 0) {
        draw_circle(cx, cy, 6, 0x002D3748);
        draw_line(cx, cy, cx, cy - 3, 0x00E53E3E); 
        draw_line(cx, cy, cx + 2, cy + 2, 0x002D3748); 
    } else if (strncmp(title, "PCI", 3) == 0) {
        draw_rect(cx - 4, cy - 4, 8, 8, 0x00FFFFFF);
        for(int i=0; i<2; i++) {
            draw_rect(cx - 6, cy - 2 + i*4, 2, 1, 0x00FFFFFF); 
            draw_rect(cx + 4, cy - 2 + i*4, 2, 1, 0x00FFFFFF); 
            draw_rect(cx - 2 + i*4, cy - 6, 1, 2, 0x00FFFFFF); 
            draw_rect(cx - 2 + i*4, cy + 4, 1, 2, 0x00FFFFFF); 
        }
    } else if (strncmp(title, "Mini Brow", 9) == 0 || strncmp(title, "Browser", 7) == 0) {
        draw_circle(cx, cy, 6, 0x00FFFFFF);
        draw_line(cx - 6, cy, cx + 6, cy, 0x00FFFFFF); 
        draw_line(cx, cy - 6, cx, cy + 6, 0x00FFFFFF); 
        draw_circle(cx, cy, 3, 0x00FFFFFF); 
    } else if (strncmp(title, "Snake", 5) == 0) {
        draw_rect(cx - 4, cy - 1, 6, 2, 0x00FFFFFF); 
        draw_rect(cx, cy - 4, 2, 3, 0x00FFFFFF); 
        draw_rect(cx - 4, cy + 1, 2, 3, 0x00FFFFFF); 
        put_pixel(cx + 1, cy - 3, 0x0038A169); 
    } else if (strncmp(title, "Calc", 4) == 0) {
        draw_rect(cx - 4, cy - 6, 8, 12, 0x00FFFFFF); 
        draw_rect(cx - 3, cy - 5, 6, 3, 0x00E2E8F0); 
        for(int r=0; r<2; r++) {
            for(int c=0; c<2; c++) {
                draw_rect(cx - 3 + c*4, cy - 1 + r*4, 2, 2, 0x00A0AEC0); 
            }
        }
    } else {
        // Fallback
        draw_rect(cx - 3, cy - 4, 6, 8, 0x00FFFFFF);
    }
}

int start_menu_open = 0;
int calendar_open = 0;

static int get_dow(int d, int m, int y) {
    if (m < 3) { m += 12; y -= 1; }
    int k = y % 100;
    int j = y / 100;
    int h = (d + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    return (h + 6) % 7; // 0=Sun, 1=Mon, ... 6=Sat
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

    // ---------- Taskbar background ----------
    draw_rect(0, ty, fb_width, TASKBAR_H_PX, GUI_TASKBAR);
    draw_rect(0, ty, fb_width, 1, GUI_BORDER);

    // ---------- Start button (Modern Flat, Mectov OS) ----------
    int start_x = 4;
    int start_w = 86; // Kept the same width so hit-boxes in desktop.c stay valid
    int start_h = TASKBAR_H_PX - 8;
    int start_y = ty + 4;
    
    // Active state uses a slightly lighter color
    uint32_t btn_bg = start_menu_open ? 0x003B3B4F : 0x002B2B3C;
    draw_rounded_rect(start_x, start_y, start_w, start_h, 6, btn_bg);
    
    // Subtle border for definition
    draw_rounded_rect_border(start_x, start_y, start_w, start_h, 6, 0x001A1A2A);

    // Text "Mectov OS"
    uint32_t text_col = 0x00FFFFFF; // Crisp white
    int t_len = 9 * 8; // Mectov OS = 9 chars * 8 px width
    int tx = start_x + (start_w - t_len) / 2;
    int ty_text = start_y + (start_h - 16) / 2;
    
    draw_string_px(tx, ty_text, "Mectov OS", text_col, btn_bg);

    // ---------- System Tray (right side, ToaruOS style) ----------
    int tray_x = (int)fb_width - 6;
    int tray_cy = ty + TASKBAR_H_PX / 2; // center Y

    // 1. Power button icon (rightmost)
    int pwr_r = 7;
    int pwr_x = tray_x - pwr_r - 2;
    draw_circle(pwr_x, tray_cy, pwr_r, 0x00FF5555);
    draw_circle(pwr_x, tray_cy, pwr_r - 1, 0x00FF5555); // make it slightly thicker
    // erase the top to create the gap
    draw_rect(pwr_x - 3, tray_cy - pwr_r - 2, 7, 6, GUI_TASKBAR);
    // draw the vertical line inside the gap
    draw_rect(pwr_x - 1, tray_cy - pwr_r - 2, 3, pwr_r + 2, 0x00FF5555);
    tray_x = pwr_x - pwr_r - 8;

    // 2. Separator
    draw_rect(tray_x, ty + 7, 1, TASKBAR_H_PX - 14, GUI_BORDER2);
    tray_x -= 8;

    // 3. Clock: HH:MM:SS
    rtc_time_t tm = rtc_read_time();
    unsigned char sec  = tm.second;
    unsigned char min  = tm.minute;
    unsigned char hour = tm.hour;
    unsigned char dow  = tm.dow;
    unsigned char c_day = tm.day;
    unsigned char c_mon = tm.month;
    unsigned int  c_yr  = tm.year;
    // Convert to WIB (UTC+7)
    int h_tmp = hour + 7;
    if (h_tmp >= 24) { dow++; if (dow > 7) dow = 1; c_day++; }
    hour = h_tmp % 24;

    // Time string "HH:MM:SS"
    char time_str[9];
    time_str[0] = '0' + hour / 10; time_str[1] = '0' + hour % 10;
    time_str[2] = ':';
    time_str[3] = '0' + min / 10; time_str[4] = '0' + min % 10;
    time_str[5] = ':';
    time_str[6] = '0' + sec / 10; time_str[7] = '0' + sec % 10;
    time_str[8] = '\0';

    int time_w = 8 * 8; // 8 chars * 8px
    int time_x = tray_x - time_w;
    int clk_bg = calendar_open ? GUI_BTN_HOV : GUI_TASKBAR;
    draw_string_px(time_x, ty + 6, time_str, GUI_TEXT, clk_bg);
    tray_x = time_x - 10;

    // 4. Separator
    draw_rect(tray_x, ty + 7, 1, TASKBAR_H_PX - 14, GUI_BORDER2);
    tray_x -= 8;

    // 5. Date: "DayName  Month Day"  (e.g. "Wednesday  April 29")
    const char* day_names[] = {"???", "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    const char* month_names[] = {"???","January","February","March","April","May","June",
                                  "July","August","September","October","November","December"};
    if (dow > 7) dow = 0;
    if (c_mon > 12) c_mon = 0;
    
    // Build date string: "DayName  Month Day"
    char date_str2[48];
    int di2 = 0;
    const char* dn = day_names[dow];
    while (*dn) date_str2[di2++] = *dn++;
    date_str2[di2++] = ' '; date_str2[di2++] = ' ';
    const char* mn = month_names[c_mon];
    while (*mn) date_str2[di2++] = *mn++;
    date_str2[di2++] = ' ';
    if (c_day >= 10) date_str2[di2++] = '0' + c_day / 10;
    date_str2[di2++] = '0' + c_day % 10;
    date_str2[di2] = '\0';

    int date_w = di2 * 8;
    int date_x = tray_x - date_w;
    draw_string_px(date_x, ty + 6, date_str2, GUI_TEXT_INV, clk_bg);
    // Save clk_x for calendar click detection
    int clk_x = date_x - 4;
    int clk_w = tray_x + 8 - date_x + time_w + 18;
    tray_x = date_x - 10;

    // 6. Separator
    draw_rect(tray_x, ty + 7, 1, TASKBAR_H_PX - 14, GUI_BORDER2);
    tray_x -= 8;

    // 7. Caps Lock indicator
    int caps_x = tray_x - 26;
    if (caps_a) {
        draw_rounded_rect(caps_x, ty + 6, 24, 16, 3, GUI_CLOSE);
        draw_string_px(caps_x + 4, ty + 8, "CAP", 0x00FFFFFF, GUI_CLOSE);
    } else {
        draw_rounded_rect(caps_x, ty + 6, 24, 16, 3, GUI_BORDER2);
        draw_string_px(caps_x + 4, ty + 8, "cap", GUI_DIM, GUI_BORDER2);
    }
    tray_x = caps_x - 6;

    // 8. HDD activity dot
    int hdd_x = tray_x - 12;
    fill_circle(hdd_x + 5, tray_cy, 4, hdd_activity > 0 ? 0x00FF4444 : GUI_BORDER2);
    if (hdd_activity > 0) hdd_activity--;
    tray_x = hdd_x - 6;

    // 9. WiFi Icon (replacing RAM usage)
    int wifi_w = 16;
    int wifi_x = tray_x - wifi_w;
    
    // Classic 16x14 WiFi Symbol (3 arcs + dot)
    uint16_t wifi_icon[14] = {
        0b0000000000000000,
        0b0000111111110000, // Outer arc top
        0b0011111111111100,
        0b0111000000001110, // Outer arc tapers
        0b1100011111100011, // Middle arc top
        0b1000111111110001,
        0b0001110000111000, // Middle arc tapers
        0b0000001111000000, // Inner arc top
        0b0000111111110000,
        0b0000111001110000, // Inner arc tapers
        0b0000000000000000, // Gap
        0b0000000110000000, // Dot top
        0b0000001111000000, // Dot middle
        0b0000000110000000  // Dot bottom
    };
    
    for(int r = 0; r < 14; r++) {
        for(int c = 0; c < 16; c++) {
            if(wifi_icon[r] & (0x8000 >> c)) {
                put_pixel(wifi_x + c, ty + 7 + r, 0x00FFFFFF);
            }
        }
    }
    tray_x = wifi_x - 10;


    // ---------- Separator between Start and Apps ----------
    int sep_x = 4 + 86 + 6; // start_x + start_w + padding
    draw_rect(sep_x, ty + 7, 1, TASKBAR_H_PX - 14, 0x00AAAAAA); // Brighter white/gray

    // ---------- Window buttons ----------
    int wx = sep_x + 8;
    int btn_w = 34;
    int btn_h = TASKBAR_H_PX - 6;
    for (int i = 0; i < MAX_WINDOWS && wx + btn_w + 2 < tray_x; i++) {
        if (!wm_wins[i].visible) continue;
        int focused = (wm_focused == wm_wins[i].id) && !wm_wins[i].minimized;
        uint32_t bg2 = wm_wins[i].minimized ? GUI_TASKBAR : (focused ? GUI_BTN_HOV : GUI_BTN);

        // Rounded window button
        draw_rounded_rect(wx, ty + 3, btn_w, btn_h, BTN_RADIUS, bg2);
        draw_rounded_rect_border(wx, ty + 3, btn_w, btn_h, BTN_RADIUS, focused ? GUI_BORDER : GUI_BORDER2);

        // Icon
        draw_taskbar_icon(wx + (btn_w - 16) / 2, ty + 5, wm_wins[i].title, wm_wins[i].minimized);

        wx += btn_w + 4;
    }

    // ========== Draw Start Menu (drop-up, rounded) ==========
    if (start_menu_open) {
        int sm_h = 304;
        int sm_w = 170;
        int sm_y = ty - sm_h;
        // Shadow
        draw_soft_shadow(2, sm_y, sm_w, sm_h, 6, 180);
        // Main menu background
        draw_rounded_rect(2, sm_y, sm_w, sm_h, WIN_RADIUS, GUI_BG);
        draw_rounded_rect_border(2, sm_y, sm_w, sm_h, WIN_RADIUS, GUI_BORDER);

        // Header
        draw_gradient_v(2, sm_y, sm_w, 28, GUI_TITLE_A, GUI_TITLE_B);
        draw_string_px(10, sm_y + 6, "MectovOS", GUI_TEXT, GUI_TITLE_A);

        // Menu items
        struct { const char* label; int len; } items[] = {
            {"Terminal", 8}, {"Editor (Nano)", 13}, {"File Explorer", 13},
            {"Mini Browser", 12}, {"System Info", 11}, {"Clock", 5},
            {"PCI Manager", 11}, {"Snake Game", 10}, {"Restart", 7}, {"Power Off", 9},
        };
        int oy = sm_y + 32;
        for (int n = 0; n < 10; n++) {
            uint32_t item_bg = GUI_BG;
            // Highlight on hover simulation (none since no mouse tracking here)
            if (n == 9) { // Power off item
                draw_rect(4, oy, sm_w - 8, 22, 0x00220000);
                draw_string_px(12, oy + 3, items[n].label, GUI_CLOSE, 0x00220000);
            } else if (n == 8) { // Restart item
                draw_rect(4, oy, sm_w - 8, 22, 0x00221100);
                draw_string_px(12, oy + 3, items[n].label, GUI_YELLOW, 0x00221100);
            } else {
                draw_string_px(12, oy + 3, items[n].label, GUI_TEXT, item_bg);
            }
            oy += 24;
        }
    }

    // ========== Draw Calendar (drop-up, rounded) ==========
    if (calendar_open) {
        unsigned char day_cmos = tm.day;
        unsigned char mon_cmos = tm.month;
        unsigned int  c_yr     = tm.year;

        int cal_w = 220;
        int cal_h = 180;
        int cal_x = clk_x - 20;
        int cal_y = ty - cal_h;

        // Shadow
        draw_soft_shadow(cal_x, cal_y, cal_w, cal_h, 6, 180);
        // Main background
        draw_rounded_rect(cal_x, cal_y, cal_w, cal_h, WIN_RADIUS, GUI_BG);
        draw_rounded_rect_border(cal_x, cal_y, cal_w, cal_h, WIN_RADIUS, GUI_BORDER);

        // Header
        draw_gradient_v(cal_x, cal_y, cal_w, 28, GUI_TITLE_A, GUI_TITLE_B);
        const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
        char date_str[32];
        int di = 0;
        if (day_cmos >= 10) date_str[di++] = '0' + (day_cmos / 10);
        date_str[di++] = '0' + (day_cmos % 10);
        date_str[di++] = ' ';
        const char* ms = months[(mon_cmos - 1) % 12];
        while (*ms) date_str[di++] = *ms++;
        date_str[di++] = ' ';
        date_str[di++] = '0' + (c_yr / 1000);
        date_str[di++] = '0' + ((c_yr / 100) % 10);
        date_str[di++] = '0' + ((c_yr / 10) % 10);
        date_str[di++] = '0' + (c_yr % 10);
        date_str[di] = '\0';
        draw_string_px(cal_x + 60, cal_y + 6, date_str, GUI_TEXT, GUI_TITLE_A);

        // Day headers
        const char* d_labels[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
        for (int i = 0; i < 7; i++)
            draw_string_px(cal_x + 10 + i * 28, cal_y + 34, d_labels[i], GUI_DIM, GUI_BG);

        int start_dow = get_dow(1, mon_cmos, c_yr);
        int total = days_in_month(mon_cmos, c_yr);
        int row = 0;
        for (int d = 1; d <= total; d++) {
            int col = (start_dow + d - 1) % 7;
            char nb[3];
            if (d < 10) { nb[0] = ' '; nb[1] = '0' + d; }
            else { nb[0] = '0' + (d/10); nb[1] = '0' + (d%10); }
            nb[2] = '\0';
            uint32_t cf = GUI_TEXT;
            if (d == day_cmos) {
                fill_circle(cal_x + 14 + col * 28, cal_y + 55 + row * 20, 8, GUI_BTN_HOV);
                cf = GUI_WHITE;
            }
            draw_string_px(cal_x + 10 + col * 28, cal_y + 52 + row * 20, nb, cf, (d == day_cmos) ? GUI_BTN_HOV : GUI_BG);
            if (col == 6) row++;
        }

        // Fill remaining cells for consistent look
        int end_col = (start_dow + total - 1) % 7;
        if (end_col < 6) {
            for (int i = end_col + 1; i <= 6; i++)
                draw_string_px(cal_x + 10 + i * 28, cal_y + 52 + row * 20, "  ", GUI_DIM, GUI_BG);
        }
    }
}

void taskbar_handle_click(int mx, int my) {
    int ty = (int)fb_height - TASKBAR_H_PX;
    if (my < ty) return;

    // Start button hit test
    if (mx >= 2 && mx <= 88) {
        start_menu_open = !start_menu_open;
        if (start_menu_open) calendar_open = 0;
        return;
    }

    // Power button hit test
    int tray_x = (int)fb_width - 6;
    int pwr_r = 7;
    int pwr_x = tray_x - pwr_r - 2;
    if (mx >= pwr_x - pwr_r && mx <= pwr_x + pwr_r) {
        extern void shutdown(void);
        shutdown();
        return;
    }

    // Clock hit test (right side)
    int clk_w = 84;
    int clk_x = tray_x - 220; // approximate, enough to catch calendar
    if (mx >= clk_x && mx <= pwr_x - pwr_r - 10) {
        calendar_open = !calendar_open;
        if (calendar_open) start_menu_open = 0;
        return;
    }

    // Window buttons hit test
    int sep_x = 4 + 86 + 6;
    int wx = sep_x + 8;
    int btn_w = 34;
    for (int i = 0; i < MAX_WINDOWS && wx + btn_w + 2 < clk_x; i++) {
        if (!wm_wins[i].visible) continue;
        if (mx >= wx && mx < wx + btn_w) {
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
        wx += btn_w + 4;
    }
}

void taskbar_tick() { }