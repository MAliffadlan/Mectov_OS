#include "../include/taskbar.h"
#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/mem.h"
#include "../include/wm.h"
#include "../include/timer.h"
#include "../include/keyboard.h"
#include "../include/ata.h"
#include "../include/rtc.h"
#include "../include/apps.h"
#include "../include/desktop.h"
#include "../include/login.h"
#include "../include/sb16.h"

// ---------- Mouse hover tracking for taskbar ----------
static int hover_start_x = 0;   // start button
static int hover_win_idx = -1;   // which window button is hovered
static int hover_menu_idx = -1;  // which start menu item is hovered
static int mouse_x_tmp = 0, mouse_y_tmp = 0;

void taskbar_track_mouse(int mx, int my, int px, int py) {
    (void)px; (void)py;
    mouse_x_tmp = mx;
    mouse_y_tmp = my;
    
    int ty = (int)fb_height - TASKBAR_H_PX;
    if (my < ty) {
        // Check if mouse is over start menu
        if (start_menu_open) {
            int sm_h = 348, sm_w = 200;
            int sm_y = ty - sm_h;
            if (mx >= 2 && mx <= 2 + sm_w && my >= sm_y && my <= sm_y + sm_h) {
                if (my >= sm_y + 36) {
                    int rel_y = my - (sm_y + 36);
                    int item = rel_y / 28;
                    if (item >= 0 && item < 11) {
                        hover_menu_idx = item;
                        return;
                    }
                }
            }
        }
        hover_menu_idx = -1;
        return;
    }
    
    // Over taskbar
    hover_start_x = 0;
    hover_win_idx = -1;
    hover_menu_idx = -1;
    
    // Check start button
    if (mx >= 2 && mx <= 88) {
        hover_start_x = 1;
    }
    
    // Check window buttons
    int sep_x = 4 + 86 + 6;
    int wx = sep_x + 6;
    int tray_right = (int)fb_width - 200; // rough tray left edge
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wm_wins[i].visible) continue;
        int tlen = strlen(wm_wins[i].title);
        int text_px = tlen * 8;
        int btn_w = 34 + text_px; // 8px padding + 14px icon + 4px gap + text + 8px padding
        if (btn_w < TASKBAR_BTN_MIN_W) btn_w = TASKBAR_BTN_MIN_W;
        if (btn_w > TASKBAR_BTN_MAX_W) btn_w = TASKBAR_BTN_MAX_W;
        if (wx + btn_w + 2 > tray_right) break;
        if (mx >= wx && mx < wx + btn_w) {
            hover_win_idx = i;
            break;
        }
        wx += btn_w + 4;
    }
}

// ---------- Icon drawing (same as before but static helper) ----------
static void draw_app_icon(int ix, int iy, const char* title, int size) {
    int cx = ix + size/2;
    int cy = iy + size/2;
    
    uint32_t bg_col = 0x00FFFFFF;
    if (strncmp(title, "Terminal", 8) == 0) bg_col = 0x002D3748;
    else if (strncmp(title, "File Expl", 9) == 0) bg_col = 0x003182CE;
    else if (strncmp(title, "System In", 9) == 0) bg_col = 0x00E2E8F0;
    else if (strncmp(title, "Clock", 5) == 0) bg_col = 0x00FFFFFF;
    else if (strncmp(title, "PCI", 3) == 0) bg_col = 0x00DD6B20;
    else if (strncmp(title, "Mini Brow", 9) == 0 || strncmp(title, "Browser", 7) == 0) bg_col = 0x00319795;
    else if (strncmp(title, "Snake", 5) == 0) bg_col = 0x0038A169;
    else if (strncmp(title, "Calc", 4) == 0) bg_col = 0x00718096;
    else if (strncmp(title, "Editor", 6) == 0) bg_col = 0x00718096;
    else if (strncmp(title, "Task Mgr", 8) == 0) bg_col = 0x004A5568;
    else if (strncmp(title, "Flappy", 6) == 0) bg_col = 0x00ECC94B;
    else if (strncmp(title, "Media", 5) == 0) bg_col = 0x00D53F8C; // Pinkish red for media player
    else bg_col = 0x00718096;

    draw_rounded_rect(ix, iy, size, size, size/4, bg_col);

    if (strncmp(title, "Terminal", 8) == 0) {
        draw_string_px(ix + 4, iy + 4, ">_", 0x0048BB78, bg_col);
    } else if (strncmp(title, "File Expl", 9) == 0) {
        draw_rect(cx - size/3, cy - size/3, size*2/3, size/2, 0x00FFFFFF);
        draw_rect(cx - size/3, cy - size/3 - 1, size/4, 2, 0x00EBF8FF);
    } else if (strncmp(title, "System In", 9) == 0) {
        draw_rect(cx - size/3, cy - size/3, size*2/3, size/2, 0x002D3748);
        draw_rect(cx - size/4, cy - size/4, size/2, size/3, 0x00A0AEC0);
    } else if (strncmp(title, "Clock", 5) == 0) {
        draw_circle(cx, cy, size/2 - 1, 0x002D3748);
        draw_line(cx, cy, cx, cy - size/3, 0x00E53E3E);
        draw_line(cx, cy, cx + size/4, cy + size/4, 0x002D3748);
    } else if (strncmp(title, "PCI", 3) == 0) {
        draw_rect(cx - size/3, cy - size/3, size*2/3, size*2/3, 0x00FFFFFF);
    } else if (strncmp(title, "Mini Brow", 9) == 0 || strncmp(title, "Browser", 7) == 0) {
        draw_circle(cx, cy, size/2 - 1, 0x00FFFFFF);
        draw_line(cx - size/2, cy, cx + size/2, cy, 0x00FFFFFF);
        draw_line(cx, cy - size/2, cx, cy + size/2, 0x00FFFFFF);
    } else if (strncmp(title, "Task Mgr", 8) == 0) {
        draw_rect(ix + 2, iy + 2, 10, 10, 0x00FFFFFF);
        draw_rect(ix + 4, iy + 4, 6, 2, 0x00CBD5E0);
        draw_rect(ix + 4, iy + 8, 6, 2, 0x00CBD5E0);
    } else if (strncmp(title, "Flappy", 6) == 0) {
        draw_rect(ix + 4, iy + 4, 6, 6, 0x00FFFFFF);
        draw_rect(ix + 10, iy + 6, 2, 2, 0x00E53E3E);
    } else if (strncmp(title, "Snake", 5) == 0) {
        draw_rect(cx - size/3, cy - 2, size/2, 4, 0x00FFFFFF);
        draw_rect(cx + 2, cy - size/3, 4, size/3, 0x00FFFFFF);
    } else if (strncmp(title, "Calc", 4) == 0 || strncmp(title, "Editor", 6) == 0) {
        draw_rect(cx - size/3, cy - size/3, size*2/3, size*2/3, 0x00FFFFFF);
    } else if (strncmp(title, "Media", 5) == 0) {
        // Play button triangle
        draw_line(cx - size/4, cy - size/4, cx - size/4, cy + size/4, 0x00FFFFFF);
        draw_line(cx - size/4, cy - size/4, cx + size/4, cy, 0x00FFFFFF);
        draw_line(cx - size/4, cy + size/4, cx + size/4, cy, 0x00FFFFFF);
    } else {
        draw_rect(cx - size/4, cy - size/4, size/2, size/2, 0x00FFFFFF);
    }
}

int start_menu_open = 0;
int calendar_open = 0;
static int volume_popup_open = 0;
static int vol_icon_x_s = 0;  // saved for click handler
static int vol_icon_ty_s = 0;
static int prev_volume = 80;  // for mute toggle

static int get_dow(int d, int m, int y) {
    if (m < 3) { m += 12; y -= 1; }
    int k = y % 100;
    int j = y / 100;
    int h = (d + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    return (h + 6) % 7;
}

static int days_in_month(int m, int y) {
    int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    // Clamp: a dead CMOS battery can report month 0, which would index days[-1].
    if (m < 1 || m > 12) return 31;
    if (m == 2) {
        if (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) return 29;
        return 28;
    }
    return days[m - 1];
}

void taskbar_pre_draw() {
    if (!is_vbe) return;
    // Detect popup state changes and force fullscreen dirty mark to paint/erase popups cleanly
    // This must be called BEFORE desktop_draw() so the background can actually redraw over closed popups!
    static int last_sm = 0;
    static int last_cal = 0;
    static int last_vol = 0;
    if (start_menu_open != last_sm || calendar_open != last_cal || volume_popup_open != last_vol) {
        last_sm = start_menu_open;
        last_cal = calendar_open;
        last_vol = volume_popup_open;
        extern void mark_dirty(int, int, int, int);
        mark_dirty(0, 0, fb_width, fb_height);
    }
}

void taskbar_draw() {
    if (!is_vbe) return;

    int ty = (int)fb_height - TASKBAR_H_PX;
    
    extern int d_max_y;
    // Check if the taskbar overlaps the active dirty rectangle.
    // If not, we don't need to redraw it because it is already correct in the back buffer!
    if (d_max_y < ty) {
        return;
    }

    int tray_right = (int)fb_width; // right edge

    // ---------- Taskbar background (clean dark, slightly transparent feel) ----------
    draw_rect(0, ty, fb_width, TASKBAR_H_PX, GUI_TASKBAR);
    draw_rect(0, ty, fb_width, 1, GUI_BORDER);

    // ---------- Start button (modern pill, "Mectov OS") ----------
    int start_x = 4;
    int start_w = 86;
    int start_h = TASKBAR_H_PX - 8;
    int start_y = ty + 4;
    
    uint32_t start_bg = (start_menu_open || hover_start_x) ? 0x003B3B4F : 0x002B2B3C;
    draw_rounded_rect(start_x, start_y, start_w, start_h, 6, start_bg);
    if (hover_start_x) {
        draw_rounded_rect_border(start_x, start_y, start_w, start_h, 6, GUI_BORDER);
    }
    // Draw Mectov logo (small square icon before text)
    draw_rounded_rect(start_x + 6, start_y + 4, 14, 14, 3, GUI_BLUE);
    draw_string_px(start_x + 24, start_y + 3, "Mectov", 0x00FFFFFF, start_bg);
    
    // ---------- Separator ----------
    int sep_x = 4 + 86 + 6;
    draw_rect(sep_x, ty + 8, 1, TASKBAR_H_PX - 16, GUI_BORDER2);

    // ---------- Window buttons (toaruOS style: icon + title + background fill) ----------
    int wx = sep_x + 6;
    // Calculate tray left edge
    int tray_x_right = tray_right - 6; // power button reference
    int pwr_r = 7;
    int pwr_x = tray_x_right - pwr_r - 2;
    // Clock area
    int time_w = 8 * 8; // HH:MM:SS
    int date_est_w = 80; // approximate date width
    int tray_left_edge = pwr_x - pwr_r - 8 - 8 // separators
                         - time_w - 24        // time
                         - date_est_w - 16    // date
                         - 30 - 8;            // caps + hdd + wifi
    if (tray_left_edge < sep_x + 10) tray_left_edge = sep_x + 10;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wm_wins[i].visible) continue;
        
        // Calculate button width based on title text length
        int tlen = strlen(wm_wins[i].title);
        int text_px = tlen * 8;
        int btn_w = 34 + text_px; // 8px padding + 14px icon + 4px gap + text + 8px padding
        if (btn_w < TASKBAR_BTN_MIN_W) btn_w = TASKBAR_BTN_MIN_W;
        if (btn_w > TASKBAR_BTN_MAX_W) btn_w = TASKBAR_BTN_MAX_W;
        
        if (wx + btn_w + 2 > tray_left_edge) break;
        
        int focused = (wm_focused == wm_wins[i].id) && !wm_wins[i].minimized;
        int hovered = (hover_win_idx == i);
        uint32_t bg2;
        if (wm_wins[i].minimized) {
            bg2 = GUI_TASKBAR;
        } else if (focused) {
            bg2 = 0x003B3B4F; // brighter bg for active
        } else if (hovered) {
            bg2 = 0x002E2E42; // hover highlight
        } else {
            bg2 = 0x00222233; // inactive bg
        }

        // Rounded window button
        draw_rounded_rect(wx, ty + 3, btn_w, TASKBAR_H_PX - 6, BTN_RADIUS, bg2);
        if (focused) {
            // Active indicator: left accent bar using existing accent color
            // (no border for focused — left accent bar provides distinction)
            // Left accent bar
            draw_rect(wx, ty + 4, 3, TASKBAR_H_PX - 8, GUI_BLUE);
        } else if (hovered) {
            draw_rounded_rect_border(wx, ty + 3, btn_w, TASKBAR_H_PX - 6, BTN_RADIUS, GUI_BORDER2);
        }

        // Icon (14x14 small)
        draw_app_icon(wx + 8, ty + 9, wm_wins[i].title, 14);
        
        // Title text (truncated if too long)
        char title_buf[24];
        int ti = 0;
        const char* src = wm_wins[i].title;
        while (*src && ti < 23) title_buf[ti++] = *src++;
        title_buf[ti] = '\0';
        
        // Check if text fits
        if (ti * 8 > btn_w - 24) {
            // Truncate
            int max_chars = (btn_w - 24) / 8;
            if (max_chars > 3) {
                title_buf[max_chars - 1] = '.';
                title_buf[max_chars - 2] = '.';
                title_buf[max_chars] = '\0';
            }
        }
        
        draw_string_px(wx + 26, ty + 12, title_buf, focused ? GUI_WHITE : GUI_TEXT, bg2);
        
        wx += btn_w + 4;
    }

    // ---------- System Tray (right side) ----------
    int tray_x = tray_right - 6;

    // 1. Power button icon (rightmost)
    draw_circle(pwr_x, ty + TASKBAR_H_PX / 2, pwr_r, 0x00FF5555);
    draw_circle(pwr_x, ty + TASKBAR_H_PX / 2, pwr_r - 1, 0x00FF5555);
    draw_rect(pwr_x - 3, ty + TASKBAR_H_PX / 2 - pwr_r - 2, 7, 6, GUI_TASKBAR);
    draw_rect(pwr_x - 1, ty + TASKBAR_H_PX / 2 - pwr_r - 2, 3, pwr_r + 2, 0x00FF5555);
    tray_x = pwr_x - pwr_r - 8;

    // 2. Separator
    draw_rect(tray_x, ty + 8, 1, TASKBAR_H_PX - 16, GUI_BORDER2);
    tray_x -= 8;

    // 3. Clock: HH:MM:SS
    rtc_time_t tm = rtc_read_time();
    unsigned char sec  = tm.second;
    unsigned char min  = tm.minute;
    unsigned char hour = tm.hour;
    unsigned char dow  = tm.dow;
    unsigned char c_day = tm.day;
    unsigned char c_mon = tm.month;
    int h_tmp = hour + 7;
    if (h_tmp >= 24) { dow++; if (dow > 7) dow = 1; c_day++; }
    hour = h_tmp % 24;

    char time_str[9];
    time_str[0] = '0' + hour / 10; time_str[1] = '0' + hour % 10;
    time_str[2] = ':';
    time_str[3] = '0' + min / 10; time_str[4] = '0' + min % 10;
    time_str[5] = ':';
    time_str[6] = '0' + sec / 10; time_str[7] = '0' + sec % 10;
    time_str[8] = '\0';

    int time_w_actual = 8 * 8;
    int time_x = tray_x - time_w_actual;
    uint32_t clk_bg = calendar_open ? GUI_BTN_HOV : GUI_TASKBAR;
    draw_string_px(time_x, ty + 12, time_str, GUI_TEXT, clk_bg);
    tray_x = time_x - 10;

    // 4. Separator
    draw_rect(tray_x, ty + 8, 1, TASKBAR_H_PX - 16, GUI_BORDER2);
    tray_x -= 8;

    // 5. Date: "Wed  Apr 29"
    const char* day_names_short[] = {"???", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char* month_names[] = {"???","Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"};
    if (dow > 7) dow = 0;
    if (c_mon > 12) c_mon = 0;
    
    char date_str2[20];
    int di2 = 0;
    const char* dn = day_names_short[dow];
    while (*dn) date_str2[di2++] = *dn++;
    date_str2[di2++] = ' ';
    const char* mn = month_names[c_mon];
    while (*mn) date_str2[di2++] = *mn++;
    date_str2[di2++] = ' ';
    if (c_day >= 10) date_str2[di2++] = '0' + c_day / 10;
    date_str2[di2++] = '0' + c_day % 10;
    date_str2[di2] = '\0';

    int date_w = di2 * 8;
    int date_x = tray_x - date_w;
    draw_string_px(date_x, ty + 12, date_str2, GUI_DIM, clk_bg);
    int clk_x = date_x - 4;
    tray_x = date_x - 10;

    // 6. Caps Lock indicator (compact)
    if (caps_a) {
        int caps_x = tray_x - 26;
        draw_rounded_rect(caps_x, ty + 8, 22, 16, 3, GUI_CLOSE);
        draw_string_px(caps_x + 2, ty + 10, "CAP", 0x00FFFFFF, GUI_CLOSE);
        tray_x = caps_x - 6;
    }

    // 7. HDD activity dot
    int hdd_x = tray_x - 12;
    fill_circle(hdd_x + 5, ty + TASKBAR_H_PX / 2, 4, hdd_activity > 0 ? 0x00FF4444 : GUI_BORDER2);
    if (hdd_activity > 0) hdd_activity--;
    tray_x = hdd_x - 6;

    // 8. Volume icon (speaker glyph)
    int vol_icon_x = tray_x - 18;
    vol_icon_x_s = vol_icon_x;  // save for click handler
    vol_icon_ty_s = ty;
    int vol_icon_y = ty + 8;
    int vol = sb16_get_volume();
    // Speaker body
    draw_rect(vol_icon_x + 2, vol_icon_y + 4, 5, 8, 0x00AAAACC);
    draw_rect(vol_icon_x, vol_icon_y + 6, 2, 4, 0x00AAAACC);
    // Sound waves based on volume
    if (vol > 0) {
        draw_rect(vol_icon_x + 9, vol_icon_y + 3, 1, 10, 0x0027C93F);
    }
    if (vol > 33) {
        draw_rect(vol_icon_x + 12, vol_icon_y + 1, 1, 14, 0x001B9A2F);
    }
    if (vol > 66) {
        draw_rect(vol_icon_x + 15, vol_icon_y, 1, 16, 0x00145F1E);
    }
    if (vol == 0) {
        // Muted: X mark
        draw_rect(vol_icon_x + 10, vol_icon_y + 5, 6, 2, 0x00FF5555);
    }
    tray_x = vol_icon_x - 6;

    // ========== Draw Start Menu (toaruOS style) ==========
    if (start_menu_open) {
        int sm_h = 348;
        int sm_w = 200;
        int sm_y = ty - sm_h;
        
        // Main menu background
        draw_rounded_rect(2, sm_y, sm_w, sm_h, WIN_RADIUS, GUI_BG);
        draw_rounded_rect_border(2, sm_y, sm_w, sm_h, WIN_RADIUS, GUI_BORDER);
        
        // Header with avatar area (toaruOS style: user info)
        draw_gradient_v(2, sm_y, sm_w, 36, GUI_TITLE_A, GUI_TITLE_B);
        // Avatar circle placeholder
        fill_circle(20, sm_y + 18, 14, GUI_BLUE);
        draw_string_px(8, sm_y + 14, "M", GUI_WHITE, GUI_BLUE);
        // User name
        draw_string_px(38, sm_y + 10, "Mectov User", GUI_TEXT, GUI_TITLE_A);
        draw_string_px(38, sm_y + 22, "Professional Edition", GUI_DIM, GUI_TITLE_B);
        
        // Menu items with icons (icon on left, label on right)
        struct { const char* label; int len; uint32_t icon_bg; } items[] = {
            {"Terminal", 8, 0x002D3748},
            {"Notepad", 7, 0x00718096},
            {"File Explorer", 13, 0x003182CE},
            {"Mini Browser", 12, 0x00319795},
            {"System Info", 11, 0x00E2E8F0},
            {"Clock", 5, 0x00FFFFFF},
            {"PCI Manager", 11, 0x00DD6B20},
            {"Snake Game", 10, 0x0038A169},
            {"Task Manager", 12, 0x00805AD5},
            {"Logout", 6, 0x00000000},
            {"Power Off", 9, 0x00000000},
        };
        
        int oy = sm_y + 40; // after header
        for (int n = 0; n < 11; n++) {
            int item_y = oy;
            int item_h = 28;
            uint32_t item_bg = GUI_BG;
            uint32_t text_col = GUI_TEXT;
            
            if (hover_menu_idx == n) {
                item_bg = 0x002E2E42;
            }
            
            // Draw item background (highlighted row)
            if (hover_menu_idx == n) {
                draw_rounded_rect(4, item_y, sm_w - 8, item_h - 2, 4, item_bg);
            }
            
            if (n == 10) {
                // Power off: red accent
                if (hover_menu_idx == n) {
                    draw_rounded_rect(4, item_y, sm_w - 8, item_h - 2, 4, 0x00330000);
                }
                // Icon: power symbol
                int ic_x = 10;
                int ic_y = item_y + 6;
                fill_circle(ic_x + 8, ic_y + 7, 7, GUI_CLOSE);
                fill_circle(ic_x + 8, ic_y + 7, 5, 0x00330000);
                draw_rect(ic_x + 6, ic_y + 2, 4, 7, GUI_CLOSE);
                text_col = GUI_CLOSE;
                draw_string_px(ic_x + 18, item_y + 6, items[n].label, text_col, hover_menu_idx == n ? 0x00330000 : GUI_BG);
            } else if (n == 9) {
                // Restart: yellow accent
                if (hover_menu_idx == n) {
                    draw_rounded_rect(4, item_y, sm_w - 8, item_h - 2, 4, 0x00332200);
                }
                int ic_x = 10;
                draw_string_px(ic_x + 18, item_y + 6, items[n].label, GUI_YELLOW, hover_menu_idx == n ? 0x00332200 : GUI_BG);
                text_col = GUI_YELLOW;
            } else {
                // Normal item with icon
                int ic_x = 10;
                int ic_y = item_y + 5;
                draw_app_icon(ic_x, ic_y, items[n].label, 16);
                draw_string_px(ic_x + 20, item_y + 6, items[n].label, text_col, item_bg);
            }
            
            oy += item_h;
        }
    }

    // ========== Draw Calendar (compact, fully featured) ==========
    if (calendar_open) {
        unsigned char day_cmos = tm.day;
        unsigned char mon_cmos = tm.month;
        unsigned int  c_yr     = tm.year;

        int cal_w = 210;
        int cal_h = 170;
        int cal_x = clk_x - 10;
        int cal_y = ty - cal_h;

        if (cal_x + cal_w > (int)fb_width) cal_x = (int)fb_width - cal_w - 2;
        if (cal_x < 2) cal_x = 2;

        draw_rounded_rect(cal_x, cal_y, cal_w, cal_h, WIN_RADIUS, GUI_BG);
        draw_rounded_rect_border(cal_x, cal_y, cal_w, cal_h, WIN_RADIUS, GUI_BORDER);

        draw_gradient_v(cal_x, cal_y, cal_w, 26, GUI_TITLE_A, GUI_TITLE_B);
        const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
        char date_str[20];
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
        draw_string_px(cal_x + 55, cal_y + 5, date_str, GUI_TEXT, GUI_TITLE_A);

        const char* d_labels[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
        for (int i = 0; i < 7; i++)
            draw_string_px(cal_x + 10 + i * 26, cal_y + 30, d_labels[i], GUI_DIM, GUI_BG);

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
                fill_circle(cal_x + 14 + col * 26, cal_y + 48 + row * 18, 7, GUI_BTN_HOV);
                cf = GUI_WHITE;
            }
            draw_string_px(cal_x + 10 + col * 26, cal_y + 45 + row * 18, nb, cf, (d == day_cmos) ? GUI_BTN_HOV : GUI_BG);
            if (col == 6) row++;
        }
    }

    // ========== Volume Popup ==========
    if (volume_popup_open) {
        int vp_w = 160;
        int vp_h = 70;
        int vp_x = vol_icon_x - vp_w / 2 + 8;
        int vp_y = ty - vp_h - 4;
        if (vp_x + vp_w > (int)fb_width - 4) vp_x = (int)fb_width - vp_w - 4;
        if (vp_x < 4) vp_x = 4;

        draw_rounded_rect(vp_x, vp_y, vp_w, vp_h, 8, GUI_BG);
        draw_rounded_rect_border(vp_x, vp_y, vp_w, vp_h, 8, GUI_BORDER);

        // Title
        draw_string_px(vp_x + vp_w/2 - 24, vp_y + 6, "Volume", GUI_TEXT, GUI_BG);

        // Slider track
        int sl_x = vp_x + 12;
        int sl_y = vp_y + 28;
        int sl_w = vp_w - 24;
        int sl_h = 6;
        draw_rounded_rect(sl_x, sl_y, sl_w, sl_h, 3, 0x00333344);
        int fill_w = (vol * sl_w) / 100;
        if (fill_w > 0) {
            draw_rounded_rect(sl_x, sl_y, fill_w, sl_h, 3, 0x0027C93F);
        }
        // Knob
        int knob_x = sl_x + fill_w;
        fill_circle(knob_x, sl_y + sl_h/2, 5, 0x00FFFFFF);
        fill_circle(knob_x, sl_y + sl_h/2, 3, 0x0027C93F);

        // Volume % text
        char vbuf[8];
        int vi = 0;
        if (vol >= 100) { vbuf[vi++] = '1'; vbuf[vi++] = '0'; vbuf[vi++] = '0'; }
        else if (vol >= 10) { vbuf[vi++] = '0' + vol/10; vbuf[vi++] = '0' + vol%10; }
        else { vbuf[vi++] = '0' + vol; }
        vbuf[vi++] = '%'; vbuf[vi] = '\0';
        draw_string_px(vp_x + vp_w/2 - vi*4, vp_y + 42, vbuf, GUI_TEXT, GUI_BG);

        // Mute button
        int mute_x = vp_x + vp_w/2 - 20;
        int mute_y = vp_y + 54;
        draw_rounded_rect(mute_x, mute_y, 40, 14, 4, 0x00333344);
        draw_string_px(mute_x + 4, mute_y + 1, vol == 0 ? "Unmte" : " Mute", vol == 0 ? 0x00FF5555 : GUI_DIM, 0x00333344);
    }
}

static void handle_start_menu_click(int item) {
    start_menu_open = 0;
    switch (item) {
        case 0: open_terminal_app(); break;
        case 1: { extern int load_mct_app(const char*); load_mct_app("apps/notepad.mct"); } break;
        case 2: open_explorer_app(); break;
        case 3: open_browser_app(); break;
        case 4: open_sysinfo_app(); break;
        case 5: open_clock_app(); break;
        case 6: open_pci_app(); break;
        case 7: start_ular(); break;
        case 8: open_taskmgr_app(); break;
        case 9: // Logout
            {
                extern volatile int pending_logout;
                start_menu_open = 0;
                calendar_open = 0;
                volume_popup_open = 0;
                pending_logout = 1;
            }
            break;
        case 10: open_power_app(); break;
    }
}

void taskbar_handle_click(int mx, int my) {
    int ty = (int)fb_height - TASKBAR_H_PX;
    
    // ---------- Start menu items hit test ----------
    if (start_menu_open && my < ty) {
        int sm_h = 348, sm_w = 200;
        int sm_y = ty - sm_h;
        if (mx >= 2 && mx <= 2 + sm_w && my >= sm_y && my <= sm_y + sm_h) {
            if (my >= sm_y + 36) {
                int rel_y = my - (sm_y + 36);
                int item = rel_y / 28;
                if (item >= 0 && item < 11) {
                    handle_start_menu_click(item);
                    return;
                }
            }
        }
        // Click outside start menu area closes it
        start_menu_open = 0;
        return;
    }
    
    // ---------- Volume popup hit test (above taskbar) ----------
    if (volume_popup_open && my < ty) {
        int vp_w = 160;
        int vp_h = 70;
        int vp_x = vol_icon_x_s - vp_w / 2 + 8;
        int vp_y = ty - vp_h - 4;
        if (vp_x + vp_w > (int)fb_width - 4) vp_x = (int)fb_width - vp_w - 4;
        if (vp_x < 4) vp_x = 4;
        
        if (mx >= vp_x && mx <= vp_x + vp_w && my >= vp_y && my <= vp_y + vp_h) {
            // Slider track area
            int sl_x = vp_x + 12;
            int sl_y = vp_y + 28;
            int sl_w = vp_w - 24;
            if (my >= sl_y - 8 && my <= sl_y + 14 && mx >= sl_x && mx <= sl_x + sl_w) {
                int new_vol = ((mx - sl_x) * 100) / sl_w;
                if (new_vol < 0) new_vol = 0;
                if (new_vol > 100) new_vol = 100;
                sb16_set_volume(new_vol);
                return;
            }
            // Mute button
            int mute_x = vp_x + vp_w/2 - 20;
            int mute_y = vp_y + 54;
            if (mx >= mute_x && mx <= mute_x + 40 && my >= mute_y && my <= mute_y + 14) {
                int cur = sb16_get_volume();
                if (cur > 0) { prev_volume = cur; sb16_set_volume(0); }
                else sb16_set_volume(prev_volume);
                return;
            }
            return; // clicked inside popup but not on control
        }
        // Clicked outside popup — close it
        volume_popup_open = 0;
        if (my >= ty) { /* fall through to taskbar handling */ }
        else return;
    }
    
    if (my < ty) return;

    // Start button hit test (wider hit area)
    if (mx >= 2 && mx <= 92) {
        start_menu_open = !start_menu_open;
        if (start_menu_open) { calendar_open = 0; volume_popup_open = 0; }
        return;
    }

    // Power button hit test
    int tray_x = (int)fb_width - 6;
    int pwr_r = 7;
    int pwr_x = tray_x - pwr_r - 2;
    int pwr_btn_radius = 10;
    int dx = mx - pwr_x;
    int dy = my - (ty + TASKBAR_H_PX / 2);
    if (dx*dx + dy*dy <= pwr_btn_radius * pwr_btn_radius) {
        extern void shutdown(void);
        shutdown();
        return;
    }

    // Volume icon hit test
    if (mx >= vol_icon_x_s && mx <= vol_icon_x_s + 18 && my >= ty) {
        volume_popup_open = !volume_popup_open;
        if (volume_popup_open) { start_menu_open = 0; calendar_open = 0; }
        return;
    }

    // Clock/date area hit test (approximate the right-side tray area)
    if (mx > (int)fb_width - 220 && mx < pwr_x - pwr_r - 10) {
        calendar_open = !calendar_open;
        if (calendar_open) { start_menu_open = 0; volume_popup_open = 0; }
        return;
    }

    // Window buttons hit test
    int sep_x = 4 + 86 + 6;
    int wx = sep_x + 6;
    int tray_left_edge = (int)fb_width - 220;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wm_wins[i].visible) continue;
        int tlen = strlen(wm_wins[i].title);
        int text_px = tlen * 8;
        int btn_w = 34 + text_px; // 8px padding + 14px icon + 4px gap + text + 8px padding
        if (btn_w < TASKBAR_BTN_MIN_W) btn_w = TASKBAR_BTN_MIN_W;
        if (btn_w > TASKBAR_BTN_MAX_W) btn_w = TASKBAR_BTN_MAX_W;
        if (wx + btn_w + 2 > tray_left_edge) break;

        if (mx >= wx && mx < wx + btn_w) {
            if (wm_wins[i].minimized) {
                wm_restore(wm_wins[i].id);
            } else if (wm_focused == wm_wins[i].id) {
                wm_minimize(wm_wins[i].id);
            } else {
                wm_raise(wm_wins[i].id);
            }
            return;
        }
        wx += btn_w + 4;
    }
}

void taskbar_tick() { }

int taskbar_volume_popup_open(void) {
    return volume_popup_open;
}