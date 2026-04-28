#include "../include/desktop.h"
#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/wm.h"
#include "../include/apps.h"
#include "../include/timer.h"
#include "../include/mem.h"
#include "../include/taskbar.h"
#include "../include/vfs.h"

// Forward declarations for GUI apps
void open_terminal_app();
void open_clock_app();
void open_sysinfo_app();

// Removed desktop cache logic
// ---- Icon definitions ----
typedef struct { int x, y; const char* label; uint32_t color; void (*action)(); } Icon;

static Icon icons[ICON_COUNT];
static uint32_t last_click_tick = 0;
static int      last_click_icon = -1;

extern int load_mct_app(const char*);
static void open_snake_wrapper() { start_ular(); }
static void open_gcalc_app() { load_mct_app("gcalc.mct"); }

static void save_desktop_icons() {
    int idx = vfs_find("icons.cfg");
    if (idx == -1) idx = vfs_create("icons.cfg");
    if (idx != -1) {
        int* data = (int*)fs[idx].data;
        for (int i = 0; i < ICON_COUNT; i++) {
            data[i*2]   = icons[i].x;
            data[i*2+1] = icons[i].y;
        }
        fs[idx].size = ICON_COUNT * 2 * sizeof(int);
        vfs_save();
    }
}

static void init_icons() {
    // MenuetOS-style vertical layout: left column, evenly spaced
    int col_x = 24, start_y = 40, gap_y = 76;
    icons[0] = (Icon){ col_x, start_y + 0*gap_y, "Terminal", 0, open_terminal_app  };
    icons[1] = (Icon){ col_x, start_y + 1*gap_y, "Browser",  0, open_browser_app   };
    icons[2] = (Icon){ col_x, start_y + 2*gap_y, "Explorer", 0, open_explorer_app  };
    icons[3] = (Icon){ col_x, start_y + 3*gap_y, "SysInfo",  0, open_sysinfo_app   };
    icons[4] = (Icon){ col_x, start_y + 4*gap_y, "Clock",    0, open_clock_app     };
    icons[5] = (Icon){ col_x, start_y + 5*gap_y, "PCI",      0, open_pci_app       };
    icons[6] = (Icon){ col_x, start_y + 6*gap_y, "Snake",    0, open_snake_wrapper };
    icons[7] = (Icon){ col_x, start_y + 7*gap_y, "Calc",     0, open_gcalc_app     };

    // Load saved positions
    int idx = vfs_find("icons.cfg");
    if (idx != -1 && fs[idx].size >= (int)(ICON_COUNT * 2 * sizeof(int))) {
        int* data = (int*)fs[idx].data;
        for (int i = 0; i < ICON_COUNT; i++) {
            icons[i].x = data[i*2];
            icons[i].y = data[i*2+1];
        }
    }
}

// --- Detailed pixel-art icon drawing (MenuetOS style, no background box) ---

static void draw_terminal_icon(int ix, int iy) {
    // Monitor body
    draw_rect(ix + 4, iy + 2, 40, 30, 0x00222222);
    draw_rect_border(ix + 4, iy + 2, 40, 30, 0x00555555);
    // Screen
    draw_rect(ix + 7, iy + 5, 34, 24, 0x00000000);
    draw_rect_border(ix + 7, iy + 5, 34, 24, 0x00333333);
    // Prompt text
    draw_string_px(ix + 10, iy + 9, ">_", 0x0000FF00, 0x00000000);
    // Stand
    draw_rect(ix + 18, iy + 32, 12, 4, 0x00444444);
    draw_rect(ix + 14, iy + 36, 20, 3, 0x00555555);
}

static void draw_browser_icon(int ix, int iy) {
    // Globe circle (approx with rects)
    draw_rect(ix + 12, iy + 2, 24, 36, 0x002266CC);
    draw_rect(ix + 8, iy + 6, 32, 28, 0x002266CC);
    draw_rect(ix + 6, iy + 10, 36, 20, 0x002266CC);
    // Horizontal lines (latitude)
    draw_rect(ix + 6, iy + 14, 36, 1, 0x004488EE);
    draw_rect(ix + 6, iy + 20, 36, 1, 0x004488EE);
    draw_rect(ix + 6, iy + 26, 36, 1, 0x004488EE);
    // Vertical line (meridian)
    draw_rect(ix + 23, iy + 2, 2, 36, 0x004488EE);
    // Ellipse meridian
    draw_rect(ix + 16, iy + 4, 2, 32, 0x004488EE);
    draw_rect(ix + 30, iy + 4, 2, 32, 0x004488EE);
    // Border highlight
    draw_rect_border(ix + 12, iy + 2, 24, 36, 0x003377DD);
}

static void draw_explorer_icon(int ix, int iy) {
    // Folder tab
    draw_rect(ix + 4, iy + 8, 18, 8, 0x00DDAA00);
    draw_rect_border(ix + 4, iy + 8, 18, 8, 0x00AA8800);
    // Folder body
    draw_rect(ix + 4, iy + 14, 40, 24, 0x00FFCC33);
    draw_rect_border(ix + 4, iy + 14, 40, 24, 0x00CC9900);
    // Folder front flap
    draw_rect(ix + 6, iy + 18, 36, 18, 0x00FFDD55);
    draw_rect_border(ix + 6, iy + 18, 36, 18, 0x00DDAA22);
    // File peaking out
    draw_rect(ix + 14, iy + 10, 20, 8, 0x00FFFFFF);
    draw_rect_border(ix + 14, iy + 10, 20, 8, 0x00CCCCCC);
}

static void draw_sysinfo_icon(int ix, int iy) {
    // CPU chip body
    draw_rect(ix + 10, iy + 8, 28, 28, 0x00333333);
    draw_rect_border(ix + 10, iy + 8, 28, 28, 0x00666666);
    // CPU die center
    draw_rect(ix + 16, iy + 14, 16, 16, 0x00555555);
    draw_rect_border(ix + 16, iy + 14, 16, 16, 0x00888888);
    // "i" label on chip
    draw_rect(ix + 22, iy + 17, 4, 2, 0x0044BBFF);  // dot
    draw_rect(ix + 22, iy + 21, 4, 6, 0x0044BBFF);  // stem
    // Pins - left/right
    for (int p = 0; p < 4; p++) {
        draw_rect(ix + 4, iy + 12 + p*6, 6, 3, 0x00AAAAAA);
        draw_rect(ix + 38, iy + 12 + p*6, 6, 3, 0x00AAAAAA);
    }
    // Pins - top/bottom
    for (int p = 0; p < 4; p++) {
        draw_rect(ix + 14 + p*6, iy + 2, 3, 6, 0x00AAAAAA);
        draw_rect(ix + 14 + p*6, iy + 36, 3, 6, 0x00AAAAAA);
    }
}

static void draw_clock_icon(int ix, int iy) {
    // Clock face (circular approx)
    draw_rect(ix + 10, iy + 2, 28, 40, 0x00EEEEEE);
    draw_rect(ix + 6, iy + 6, 36, 32, 0x00EEEEEE);
    draw_rect(ix + 4, iy + 10, 40, 24, 0x00EEEEEE);
    // Border
    draw_rect_border(ix + 10, iy + 2, 28, 40, 0x00888888);
    draw_rect_border(ix + 6, iy + 6, 36, 32, 0x00888888);
    // Hour markers
    draw_rect(ix + 23, iy + 5, 2, 4, 0x00333333);   // 12
    draw_rect(ix + 23, iy + 35, 2, 4, 0x00333333);   // 6
    draw_rect(ix + 7, iy + 20, 4, 2, 0x00333333);    // 9
    draw_rect(ix + 37, iy + 20, 4, 2, 0x00333333);   // 3
    // Center dot
    draw_rect(ix + 22, iy + 20, 4, 4, 0x00CC0000);
    // Hour hand (pointing to ~10)
    draw_rect(ix + 18, iy + 12, 6, 2, 0x00222222);
    draw_rect(ix + 22, iy + 14, 2, 8, 0x00222222);
    // Minute hand (pointing to ~2)
    draw_rect(ix + 24, iy + 10, 2, 12, 0x00444444);
    draw_rect(ix + 26, iy + 10, 6, 2, 0x00444444);
}

static void draw_pci_icon(int ix, int iy) {
    // PCB Board
    draw_rect(ix + 4, iy + 6, 40, 24, 0x00115511);
    draw_rect_border(ix + 4, iy + 6, 40, 24, 0x00338833);
    // Main IC chip
    draw_rect(ix + 14, iy + 12, 20, 12, 0x00222222);
    draw_rect_border(ix + 14, iy + 12, 20, 12, 0x00444444);
    // Small components
    draw_rect(ix + 8, iy + 10, 4, 4, 0x00AA8800);
    draw_rect(ix + 8, iy + 18, 4, 4, 0x00888888);
    draw_rect(ix + 36, iy + 14, 4, 6, 0x00333333);
    // Gold contacts (PCI slot pins)
    for (int p = 0; p < 7; p++) {
        draw_rect(ix + 8 + p*5, iy + 30, 3, 6, 0x00FFD700);
    }
    // Edge connector
    draw_rect(ix + 6, iy + 30, 36, 2, 0x00CCAA00);
}

static void draw_snake_icon(int ix, int iy) {
    // Green snake body segments
    draw_rect(ix + 8, iy + 28, 8, 8, 0x0022AA22);
    draw_rect(ix + 16, iy + 28, 8, 8, 0x0033CC33);
    draw_rect(ix + 24, iy + 28, 8, 8, 0x0022AA22);
    draw_rect(ix + 24, iy + 20, 8, 8, 0x0033CC33);
    draw_rect(ix + 24, iy + 12, 8, 8, 0x0022AA22);
    // Snake head
    draw_rect(ix + 32, iy + 10, 10, 10, 0x0044DD44);
    draw_rect_border(ix + 32, iy + 10, 10, 10, 0x00228822);
    // Eye
    draw_rect(ix + 37, iy + 13, 3, 3, 0x00FFFFFF);
    draw_rect(ix + 38, iy + 14, 1, 1, 0x00000000);
    // Tongue
    draw_rect(ix + 42, iy + 16, 4, 1, 0x00FF0000);
    draw_rect(ix + 44, iy + 15, 2, 1, 0x00FF0000);
    // Apple
    draw_rect(ix + 10, iy + 10, 8, 10, 0x00DD2222);
    draw_rect_border(ix + 10, iy + 10, 8, 10, 0x00AA1111);
    // Apple stem
    draw_rect(ix + 13, iy + 7, 2, 4, 0x00664400);
    // Apple leaf
    draw_rect(ix + 15, iy + 7, 4, 2, 0x0044AA22);
}

static void draw_calc_icon(int ix, int iy) {
    // Body
    draw_rect(ix + 6, iy + 2, 36, 40, 0x00444444);
    draw_rect_border(ix + 6, iy + 2, 36, 40, 0x00777777);
    // Display
    draw_rect(ix + 10, iy + 6, 28, 10, 0x00AACC88);
    draw_rect_border(ix + 10, iy + 6, 28, 10, 0x0088AA66);
    // Text on display
    draw_string_px(ix + 12, iy + 7, "123", 0x00000000, 0);
    // Keys (3x3 grid for simplicity)
    for(int r=0; r<3; r++) {
        for(int c=0; c<3; c++) {
            int kx = ix + 10 + c*10;
            int ky = iy + 19 + r*7;
            draw_rect(kx, ky, 8, 5, 0x00888888);
        }
    }
}

static void draw_icon(int i) {
    Icon* ic = &icons[i];
    int ix = ic->x;
    int iy = ic->y;

    // Draw icon based on type (no background box - MenuetOS style)
    if (strcmp(ic->label, "Terminal") == 0)       draw_terminal_icon(ix, iy);
    else if (strcmp(ic->label, "Browser") == 0)   draw_browser_icon(ix, iy);
    else if (strcmp(ic->label, "Explorer") == 0)  draw_explorer_icon(ix, iy);
    else if (strcmp(ic->label, "SysInfo") == 0)   draw_sysinfo_icon(ix, iy);
    else if (strcmp(ic->label, "Clock") == 0)     draw_clock_icon(ix, iy);
    else if (strcmp(ic->label, "PCI") == 0)       draw_pci_icon(ix, iy);
    else if (strcmp(ic->label, "Snake") == 0)     draw_snake_icon(ix, iy);
    else if (strcmp(ic->label, "Calc") == 0)      draw_calc_icon(ix, iy);

    // Draw label with text shadow for readability on any wallpaper
    int llen = strlen(ic->label);
    int lx = ix + (48 - llen * 8) / 2;
    draw_string_px(lx + 1, iy + 45, ic->label, 0x00000000, 0xFFFFFFFF); // Shadow
    draw_string_px(lx, iy + 44, ic->label, 0x00FFFFFF, 0xFFFFFFFF);     // White text
}

extern uint32_t _binary_obj_wallpaper_bin_start[];

void desktop_draw() {
    if (!is_vbe) return;

    uint32_t area_h = fb_height - TASKBAR_H_PX;

    // Blit wallpaper to back_buffer using fast memcpy (line by line)
    // Wallpaper is baked at 1024x768 by build_wallpaper.py
    uint32_t* wp_ptr = _binary_obj_wallpaper_bin_start;
    uint32_t wp_w = 1024, wp_h = 768;
    uint32_t copy_w = (fb_width < wp_w) ? fb_width : wp_w;
    uint32_t copy_h = (area_h < wp_h) ? area_h : wp_h;
    for (uint32_t y = 0; y < copy_h; y++) {
        memcpy(&back_buffer[y * fb_width], &wp_ptr[y * wp_w], copy_w * 4);
    }

    // Fill remaining edges if screen is larger than wallpaper
    if (fb_width > wp_w) {
        draw_rect(wp_w, 0, fb_width - wp_w, area_h, 0x00111122);
    }
    if (area_h > wp_h) {
        draw_rect(0, wp_h, fb_width, area_h - wp_h, 0x00111122);
    }

    // Marquee scrolling text at the top
    static int marquee_x = 1280;
    marquee_x -= 2;
    const char* marquee_msg = "Mectov OS is still in early development phase. Expect bugs, system crashes, and unhandled exceptions. Proceed with caution. Created by M Alif Fadlan.";
    int msg_len = strlen(marquee_msg);
    if (marquee_x < -(msg_len * 8)) marquee_x = fb_width;
    
    // Draw marquee bar
    draw_rect(0, 0, fb_width, 24, 0x00000000); // Black bar
    draw_rect(0, 24, fb_width, 1, GUI_BORDER);
    draw_string_px(marquee_x, 4, marquee_msg, 0x0000FF00, 0x00000000); // Hacker green text

    // Taskbar background
    draw_rect(0, fb_height - TASKBAR_H_PX, fb_width, TASKBAR_H_PX, GUI_TASKBAR);
    draw_rect(0, fb_height - TASKBAR_H_PX, fb_width, 1, GUI_BORDER);

    // Draw icons
    if (!icons[0].label) init_icons();
    for (int i = 0; i < ICON_COUNT; i++) draw_icon(i);
}

static int dragged_icon = -1;
static int drag_offset_x = 0;
static int drag_offset_y = 0;
static int drag_start_x = 0;
static int drag_start_y = 0;

void desktop_handle_mouse(int mx, int my, int btn, int pbtn) {
    if (calendar_open) {
        if (!btn && pbtn) calendar_open = 0;
        return;
    }

    if (start_menu_open) {
        if (!btn && pbtn) {
            int sm_h = 267;
            int sm_w = 160;
            int sm_y = (int)fb_height - TASKBAR_H_PX - sm_h;
            if (mx >= 2 && mx <= 2 + sm_w && my >= sm_y && my <= sm_y + sm_h) {
                if (my >= sm_y + 25) {
                    int rel_y = my - (sm_y + 25);
                    if (rel_y < 8 * 24) {
                        int item = rel_y / 24;
                        if (item == 0) open_terminal_app();
                        else if (item == 1) st_ed("baru.txt");
                        else if (item == 2) open_explorer_app();
                        else if (item == 3) open_browser_app();
                        else if (item == 4) open_sysinfo_app();
                        else if (item == 5) open_clock_app();
                        else if (item == 6) open_pci_app();
                        else if (item == 7) start_ular();
                    } else if (rel_y >= 8 * 24 + 1) {
                        int item = (rel_y - (8 * 24 + 1)) / 24;
                        if (item == 0) shutdown();
                        else if (item == 1) reboot();
                    }
                }
            }
            start_menu_open = 0;
        }
        return;
    }

    if (!icons[0].label) return;

    if (btn && !pbtn) {
        for (int i = 0; i < ICON_COUNT; i++) {
            Icon* ic = &icons[i];
            if (mx >= ic->x && mx < ic->x + 48 && my >= ic->y && my < ic->y + 64) {
                dragged_icon = i;
                drag_offset_x = mx - ic->x;
                drag_offset_y = my - ic->y;
                drag_start_x = ic->x;
                drag_start_y = ic->y;
                return;
            }
        }
    } else if (btn && pbtn) {
        if (dragged_icon != -1) {
            icons[dragged_icon].x = mx - drag_offset_x;
            icons[dragged_icon].y = my - drag_offset_y;
        }
    } else if (!btn && pbtn) {
        if (dragged_icon != -1) {
            uint32_t now = get_ticks();
            if (last_click_icon == dragged_icon && (now - last_click_tick) < 500) {
                if (icons[dragged_icon].action) icons[dragged_icon].action();
                last_click_icon = -1;
            } else {
                last_click_icon = dragged_icon;
                last_click_tick = now;
                // Only save to disk if the icon actually moved. 
                // vfs_save blocks CPU and ruins the double-click timing if run on every single click.
                if (icons[dragged_icon].x != drag_start_x || icons[dragged_icon].y != drag_start_y) {
                    save_desktop_icons(); 
                }
            }
            dragged_icon = -1;
        }
    }
}
