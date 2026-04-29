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

// Modern flat icon (16x16) for taskbar buttons
static void draw_taskbar_icon(int ix, int iy, const char* title, int minimized) {
    uint32_t fill = minimized ? GUI_BORDER2 : GUI_BTN_HOV;

    if (strncmp(title, "Terminal", 8) == 0) {
        // Terminal icon: black screen with ">_" prompt
        draw_rounded_rect(ix, iy, 16, 16, 2, 0x001E1E2E);
        draw_rounded_rect_border(ix, iy, 16, 16, 2, GUI_BORDER);
        draw_string_px(ix + 2, iy + 4, ">_", GUI_GREEN, 0x001E1E2E);
    } else if (strncmp(title, "File Expl", 9) == 0) {
        // Folder icon
        draw_rect(ix, iy + 4, 6, 4, GUI_YELLOW);
        draw_rect(ix, iy + 8, 16, 8, 0x00E5A50A);
        draw_rounded_rect_border(ix, iy + 8, 16, 8, 2, 0x00CC8800);
    } else if (strncmp(title, "System In", 9) == 0) {
        // Monitor icon
        draw_rounded_rect(ix + 1, iy + 1, 14, 10, 2, GUI_BLUE);
        draw_rect(ix + 5, iy + 11, 6, 3, GUI_TEXT);
        draw_rect(ix + 3, iy + 14, 10, 2, GUI_TEXT);
    } else if (strncmp(title, "Clock", 5) == 0) {
        fill_circle(ix + 8, iy + 8, 7, GUI_YELLOW);
        fill_circle(ix + 8, iy + 8, 5, 0x001E1E2E);
        draw_line(ix + 8, iy + 8, ix + 8, iy + 4, GUI_YELLOW);
        draw_line(ix + 8, iy + 8, ix + 12, iy + 8, GUI_YELLOW);
    } else if (strncmp(title, "PCI", 3) == 0) {
        // Chip icon
        draw_rect(ix + 2, iy + 2, 12, 12, 0x00333344);
        draw_rect(ix + 4, iy, 8, 2, 0x00888888);
        draw_rect(ix + 4, iy + 14, 8, 2, 0x00888888);
        draw_rect(ix, iy + 4, 2, 8, 0x00888888);
        draw_rect(ix + 14, iy + 4, 2, 8, 0x00888888);
    } else if (strncmp(title, "Mini Brow", 9) == 0 || strncmp(title, "Browser", 7) == 0) {
        // Globe icon
        fill_circle(ix + 8, iy + 8, 7, GUI_BLUE);
        draw_circle(ix + 8, iy + 8, 7, 0x004477CC);
        draw_line(ix + 3, iy + 7, ix + 13, iy + 7, 0x004477CC);
        draw_line(ix + 3, iy + 9, ix + 13, iy + 9, 0x004477CC);
        draw_rect(ix + 6, iy + 2, 4, 12, 0x004477CC);
    } else if (strncmp(title, "Snake", 5) == 0) {
        // Snake icon
        draw_rect(ix + 6, iy + 2, 4, 4, GUI_GREEN);
        draw_rect(ix + 10, iy + 6, 4, 4, GUI_GREEN);
        draw_rect(ix + 6, iy + 10, 4, 4, GUI_GREEN);
        draw_rect(ix + 2, iy + 6, 4, 4, GUI_GREEN);
        fill_circle(ix + 2, iy + 2, 2, 0x00FF4400); // head
    } else if (strncmp(title, "Power", 5) == 0) {
        // Power icon
        draw_line(ix + 8, iy + 2, ix + 8, iy + 8, 0x00FF4444);
        draw_circle(ix + 8, iy + 10, 6, 0x00FF4444);
    } else {
        // Default app icon: rounded rect with accent
        draw_rounded_rect(ix, iy, 16, 16, 3, GUI_ICON_BG);
        draw_rounded_rect_border(ix, iy, 16, 16, 3, GUI_BORDER);
        draw_rect(ix + 2, iy + 2, 12, 3, GUI_BLUE);
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

    // ---------- Start button (rounded) ----------
    int start_x = 4;
    int start_w = 70;
    int start_h = TASKBAR_H_PX - 6;
    int start_y = ty + 3;
    uint32_t sbg = start_menu_open ? GUI_BTN_HOV : GUI_BTN;
    draw_rounded_rect(start_x, start_y, start_w, start_h, BTN_RADIUS, sbg);
    draw_rounded_rect_border(start_x, start_y, start_w, start_h, BTN_RADIUS, GUI_BORDER);
    // Logo text
    draw_string_px(start_x + 8, start_y + (start_h - 16) / 2, "MectovOS", GUI_TEXT, sbg);

    // ---------- System Tray (right side) ----------
    int tray_x = (int)fb_width - 8;
    int tray_entry_h = TASKBAR_H_PX - 4;

    // Helper: tray section
    #define TRAY_SPACING(gap) tray_x -= (gap)

    // 1. Clock (rightmost)
    rtc_time_t tm = rtc_read_time();
    unsigned char sec  = tm.second;
    unsigned char min  = tm.minute;
    unsigned char hour = tm.hour;
    unsigned char dow  = tm.dow;
    // Convert to WIB (UTC+7)
    int h_tmp = hour + 7;
    if (h_tmp >= 24) { dow++; if (dow > 7) dow = 1; }
    hour = h_tmp % 24;
    const char* days[] = {"???", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if (dow > 7) dow = 0;

    int clk_bg = calendar_open ? GUI_BTN_HOV : GUI_TASKBAR;
    // Clock background button
    int clk_w = 84;
    int clk_x = tray_x - clk_w - 2;
    draw_rounded_rect(clk_x, ty + 3, clk_w, TASKBAR_H_PX - 6, BTN_RADIUS, clk_bg);
    draw_rounded_rect_border(clk_x, ty + 3, clk_w, TASKBAR_H_PX - 6, BTN_RADIUS, calendar_open ? GUI_BORDER : GUI_BORDER2);
    int clk_tx = clk_x + 4;
    draw_string_px(clk_tx, ty + 6, days[dow], GUI_TEXT_INV, clk_bg);
    draw_num2(clk_tx + 28, ty + 6, hour, GUI_TEXT, clk_bg);
    draw_string_px(clk_tx + 44, ty + 6, ":", GUI_TEXT, clk_bg);
    draw_num2(clk_tx + 50, ty + 6, min,  GUI_TEXT, clk_bg);
    tray_x = clk_x - 4;

    // 2. Separator
    draw_rect(tray_x - 1, ty + 8, 1, 12, GUI_BORDER2);
    tray_x -= 8;

    // 3. RAM indicator (compact)
    unsigned int used_kb = get_used_memory() / 1024;
    unsigned int tot_kb  = get_total_memory() / 1024;
    int ram_pct = (tot_kb > 0) ? (used_kb * 100 / tot_kb) : 0;
    int ram_w = 44;
    int ram_x = tray_x - ram_w;
    // RAM bar background
    draw_rounded_rect(ram_x, ty + 10, ram_w, 8, 2, GUI_BORDER2);
    if (tot_kb > 0) {
        int fill = (ram_w * ram_pct) / 100;
        if (fill > ram_w) fill = ram_w;
        if (fill > 0)
            draw_rounded_rect(ram_x, ty + 10, fill, 8, 2, ram_pct > 80 ? 0x00FF5555 : GUI_TEAL);
    }
    // RAM percentage text
    char pct_buf[8];
    int idx = 0;
    if (ram_pct >= 100) { pct_buf[idx++] = '1'; pct_buf[idx++] = '0'; pct_buf[idx++] = '0'; }
    else {
        if (ram_pct >= 10) pct_buf[idx++] = '0' + (ram_pct / 10);
        pct_buf[idx++] = '0' + (ram_pct % 10);
    }
    pct_buf[idx++] = '%'; pct_buf[idx] = '\0';
    // Draw small text over RAM bar
    draw_string_px(ram_x + 2, ty + 9, pct_buf, 0x00FFFFFF, GUI_BORDER2);
    tray_x = ram_x - 6;

    // 4. HDD activity dot
    int hdd_x = tray_x - 14;
    fill_circle(hdd_x + 5, ty + TASKBAR_H_PX / 2, 4, hdd_activity > 0 ? 0x00FF4444 : GUI_BORDER2);
    if (hdd_activity > 0) hdd_activity--;
    tray_x = hdd_x - 8;

    // 5. Caps Lock indicator
    int caps_x = tray_x - 26;
    if (caps_a) {
        draw_rounded_rect(caps_x, ty + 6, 24, 16, 3, GUI_CLOSE);
        draw_string_px(caps_x + 4, ty + 10, "CAP", 0x00FFFFFF, GUI_CLOSE);
    } else {
        draw_rounded_rect(caps_x, ty + 6, 24, 16, 3, GUI_BORDER2);
        draw_string_px(caps_x + 4, ty + 10, "cap", GUI_DIM, GUI_BORDER2);
    }

    // ---------- Window buttons ----------
    int wx = start_x + start_w + 8;
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
        int sm_h = 280;
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
            {"PCI Manager", 11}, {"Snake Game", 10}, {"Power Off", 8},
        };
        int oy = sm_y + 32;
        for (int n = 0; n < 9; n++) {
            uint32_t item_bg = GUI_BG;
            // Highlight on hover simulation (none since no mouse tracking here)
            if (n == 8) { // Power off item
                draw_rect(4, oy, sm_w - 8, 22, 0x00220000);
                draw_string_px(12, oy + 3, items[n].label, GUI_CLOSE, 0x00220000);
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
    if (mx >= 4 && mx <= 74) {
        start_menu_open = !start_menu_open;
        if (start_menu_open) calendar_open = 0;
        return;
    }

    // Clock hit test (right side)
    int tray_x = (int)fb_width - 8;
    int clk_w = 84;
    int clk_x = tray_x - clk_w - 2;
    if (mx >= clk_x && mx <= clk_x + clk_w) {
        calendar_open = !calendar_open;
        if (calendar_open) start_menu_open = 0;
        return;
    }

    // Window buttons hit test
    int wx = 4 + 70 + 8;
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