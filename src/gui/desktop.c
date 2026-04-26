#include "../include/desktop.h"
#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/wm.h"
#include "../include/apps.h"
#include "../include/timer.h"
#include "../include/mem.h"
#include "../include/taskbar.h"

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

static void open_snake_wrapper() { start_ular(); }

static void init_icons() {
    int base_x = 30, base_y = 60, gap = 90;
    icons[0] = (Icon){ base_x + 0*gap, base_y, "Terminal", 0x00005080, open_terminal_app  };
    icons[1] = (Icon){ base_x + 1*gap, base_y, "Snake",    0x00205000, open_snake_wrapper  };
    icons[2] = (Icon){ base_x + 2*gap, base_y, "Clock",    0x00402000, open_clock_app      };
    icons[3] = (Icon){ base_x + 3*gap, base_y, "SysInfo",  0x00500020, open_sysinfo_app    };
    icons[4] = (Icon){ base_x + 4*gap, base_y, "Explorer", 0x00FFBB55, open_explorer_app   };
    icons[5] = (Icon){ base_x + 0*gap, base_y + 90, "PCI",  0x00007ACC, open_pci_app        };
}

static void draw_icon(int i) {
    Icon* ic = &icons[i];
    int ix = ic->x;
    int iy = ic->y;
    
    // Draw soft shadow
    draw_rect(ix + 2, iy + 2, 48, 48, 0x00111111);
    
    // Draw icon base background
    draw_rect(ix, iy, 48, 48, ic->color);
    draw_rect_border(ix, iy, 48, 48, GUI_BORDER);

    // Draw procedural shapes based on label
    if (strcmp(ic->label, "Terminal") == 0) {
        draw_rect(ix + 6, iy + 6, 36, 36, 0x00000000); // Black screen
        draw_rect_border(ix + 6, iy + 6, 36, 36, 0x00444444);
        draw_string_px(ix + 10, iy + 10, ">_", 0x0000FF00, 0x00000000); // Green prompt
    }
    else if (strcmp(ic->label, "Snake") == 0) {
        draw_rect(ix + 12, iy + 12, 8, 8, 0x00000000); // Snake tail
        draw_rect(ix + 20, iy + 12, 16, 8, 0x00000000); // Snake body
        draw_rect(ix + 28, iy + 20, 8, 16, 0x00000000); // Snake head
        draw_rect(ix + 12, iy + 28, 8, 8, 0x00FF0000); // Apple
    }
    else if (strcmp(ic->label, "Clock") == 0) {
        draw_rect(ix + 8, iy + 8, 32, 32, 0x00FFFFFF); // White clock face
        draw_rect_border(ix + 8, iy + 8, 32, 32, 0x00222222);
        draw_rect(ix + 23, iy + 14, 2, 10, 0x00000000); // Hour hand
        draw_rect(ix + 23, iy + 23, 8, 2, 0x00000000); // Minute hand
    }
    else if (strcmp(ic->label, "SysInfo") == 0) {
        draw_rect(ix + 12, iy + 12, 24, 24, 0x00111111); // CPU chip
        // Pins
        for (int p=14; p<34; p+=6) {
            draw_rect(ix + 8, iy + p, 4, 2, 0x00CCCCCC); // Left pins
            draw_rect(ix + 36, iy + p, 4, 2, 0x00CCCCCC); // Right pins
            draw_rect(ix + p, iy + 8, 2, 4, 0x00CCCCCC); // Top pins
            draw_rect(ix + p, iy + 36, 2, 4, 0x00CCCCCC); // Bottom pins
        }
    }
    else if (strcmp(ic->label, "Explorer") == 0) {
        draw_rect(ix + 6, iy + 12, 16, 8, 0x00FFCC00); // Folder tab
        draw_rect(ix + 6, iy + 20, 36, 20, 0x00FFDD33); // Folder body
        draw_rect_border(ix + 6, iy + 20, 36, 20, 0x00CCAA00);
    }
    else if (strcmp(ic->label, "PCI") == 0) {
        draw_rect(ix + 8, iy + 10, 32, 24, 0x00004400); // PCB Board
        draw_rect(ix + 14, iy + 16, 20, 12, 0x00111111); // IC
        for(int p=10; p<36; p+=4) { // Gold contacts
            draw_rect(ix + p, iy + 34, 2, 4, 0x00FFD700);
        }
    }

    // Draw label
    int llen = strlen(ic->label);
    int lx = ix + (48 - llen * 8) / 2;
    draw_string_px(lx, iy + 52, ic->label, GUI_TEXT_INV, 0xFFFFFFFF);
}

void desktop_draw() {
    if (!is_vbe) return;

    uint32_t area_h = fb_height - TASKBAR_H_PX;

    // Cached gradient wallpaper (computed once, reused every frame)
    static uint32_t *wp_cache = NULL;
    static uint32_t wp_w = 0, wp_h = 0;

    if (!wp_cache || wp_w != fb_width || wp_h != area_h) {
        wp_w = fb_width;
        wp_h = area_h;
        if (wp_cache) kfree(wp_cache);
        wp_cache = (uint32_t*)kmalloc(wp_w * wp_h * 4);
        if (wp_cache) {
            for (uint32_t y = 0; y < wp_h; y++) {
                uint32_t t = (y * 255) / wp_h; // 0..255 top to bottom
                // Top: deep navy (10, 15, 40) -> Bottom: charcoal (30, 30, 35)
                uint32_t r = 10 + (t * 20) / 255;
                uint32_t g = 15 + (t * 15) / 255;
                uint32_t b = 40 + (t * (255 - 40)) / 510; // subtle fade to ~65

                // Add a subtle teal band in the middle
                if (t > 80 && t < 180) {
                    uint32_t band = (t < 130) ? (t - 80) : (180 - t);
                    g += band / 4;
                    b += band / 6;
                }

                uint32_t color = (r << 16) | (g << 8) | b;
                for (uint32_t x = 0; x < wp_w; x++) {
                    wp_cache[y * wp_w + x] = color;
                }
            }
        }
    }

    // Blit cached wallpaper
    if (wp_cache) {
        for (uint32_t y = 0; y < area_h; y++) {
            memcpy((void*)&back_buffer[y * fb_width],
                   (void*)&wp_cache[y * wp_w],
                   wp_w * 4);
        }
    } else {
        draw_rect(0, 0, fb_width, area_h, GUI_DESKTOP);
    }

    // Marquee scrolling text at the top
    static int marquee_x = 1024;
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

void desktop_handle_click(int mx, int my) {
    if (start_menu_open) {
        int sm_h = 160;
        int sm_w = 160;
        int sm_y = (int)fb_height - TASKBAR_H_PX - sm_h;
        if (mx >= 2 && mx <= 2 + sm_w && my >= sm_y && my <= sm_y + sm_h) {
            if (my >= sm_y + 24) {
                int item = (my - (sm_y + 24)) / 24;
                if (item == 0) open_terminal_app();
                else if (item == 1) st_ed("baru.txt");
                else if (item == 2) open_sysinfo_app();
                else if (item == 3) open_clock_app();
                else if (item == 4) open_power_app();
            }
        }
        start_menu_open = 0;
        return; // intercept click
    }

    if (!icons[0].label) return;
    uint32_t now = get_ticks();
    for (int i = 0; i < ICON_COUNT; i++) {
        Icon* ic = &icons[i];
        if (mx >= ic->x && mx < ic->x + 48 && my >= ic->y && my < ic->y + 64) {
            if (last_click_icon == i && (now - last_click_tick) < 30) {
                if (ic->action) ic->action();
                last_click_icon = -1;
            } else {
                last_click_icon = i;
                last_click_tick = now;
            }
            return;
        }
    }
}
