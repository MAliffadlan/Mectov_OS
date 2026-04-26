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

// Draw a single desktop icon (48x48 box + label)
static void draw_icon(int i) {
    Icon* ic = &icons[i];
    draw_rect(ic->x + 3, ic->y + 3, 48, 48, 0x00000000);
    draw_rect(ic->x, ic->y, 48, 48, ic->color);
    draw_rect_border(ic->x, ic->y, 48, 48, GUI_BORDER);
    draw_char_px(ic->x + 16, ic->y + 16, ic->label[0], GUI_TEXT_INV, 0xFFFFFFFF);
    draw_char_px(ic->x + 24, ic->y + 16, ic->label[1], GUI_TEXT_INV, 0xFFFFFFFF);
    int llen = strlen(ic->label);
    int lx = ic->x + (48 - llen * 8) / 2;
    draw_string_px(lx, ic->y + 52, ic->label, GUI_TEXT_INV, 0xFFFFFFFF);
}

void desktop_draw() {
    if (!is_vbe) return;

    // Fast solid background instead of gradient to avoid memcpy overhead
    uint32_t area_h = fb_height - TASKBAR_H_PX;
    draw_rect(0, 0, fb_width, area_h, GUI_DESKTOP);

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
