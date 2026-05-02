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

// ---- Icon definitions ----
typedef struct { int x, y; const char* label; void (*action)(); } Icon;
static Icon icons[ICON_COUNT];
static uint32_t last_click_tick = 0;
static int      last_click_icon = -1;

extern int load_mct_app(const char*);
static void open_snake_wrapper() { start_ular(); }
static void open_calc_wrapper() { load_mct_app("apps/gcalc.mct"); }

#define ICON_W  72
#define ICON_H  80
#define ICON_PAD 12

static void save_desktop_icons() {
    int buf[ICON_COUNT * 2];
    for (int i = 0; i < ICON_COUNT; i++) {
        buf[i*2]   = icons[i].x;
        buf[i*2+1] = icons[i].y;
    }
    // Pastikan file icons.cfg ada, jika tidak, buat file-nya terlebih dahulu
    if (vfs_get_node("icons.cfg") < 0) {
        vfs_create_file("icons.cfg");
    }
    vfs_write_file("icons.cfg", (const char*)buf, ICON_COUNT * 2 * sizeof(int));
    vfs_save();
}

static void init_icons() {
    // Grid layout: auto-arrange in a grid
    int grid_cols = (fb_width - 40) / (ICON_W + ICON_PAD);
    if (grid_cols < 1) grid_cols = 1;
    int grid_gap_x = ICON_W + ICON_PAD;
    int grid_gap_y = ICON_H + 16;
    int start_x = 24;
    int start_y = 24;

    icons[0] = (Icon){ start_x + 0 * grid_gap_x, start_y, "Terminal",  open_terminal_app  };
    icons[1] = (Icon){ start_x + 1 * grid_gap_x, start_y, "Browser",   open_browser_app   };
    icons[2] = (Icon){ start_x + 2 * grid_gap_x, start_y, "Explorer",  open_explorer_app  };
    icons[3] = (Icon){ start_x + 3 * grid_gap_x, start_y, "SysInfo",   open_sysinfo_app   };
    icons[4] = (Icon){ start_x + 0 * grid_gap_x, start_y + 1 * grid_gap_y, "Clock",     open_clock_app     };
    icons[5] = (Icon){ start_x + 1 * grid_gap_x, start_y + 1 * grid_gap_y, "PCI",       open_pci_app       };
    icons[6] = (Icon){ start_x + 2 * grid_gap_x, start_y + 1 * grid_gap_y, "Snake",     open_snake_wrapper };
    icons[7] = (Icon){ start_x + 3 * grid_gap_x, start_y + 1 * grid_gap_y, "Calc",      open_calc_wrapper  };

    // Load saved positions
    int read_buf[ICON_COUNT * 2];
    int sz = vfs_read_file("icons.cfg", (char*)read_buf, sizeof(read_buf));
    if (sz >= (int)(ICON_COUNT * 2 * sizeof(int))) {
        for (int i = 0; i < ICON_COUNT; i++) {
            icons[i].x = read_buf[i*2];
            icons[i].y = read_buf[i*2+1];
        }
    }
}

// ---- Modern Professional Icons (Squircle/iOS style) ----
static void draw_pro_icon(int ix, int iy, const char* label) {
    int cx = ix + ICON_W / 2;
    int cy = iy + ICON_W / 2 - 6; // Center of the icon background
    int bg_size = 44;
    int bg_x = cx - bg_size / 2;
    int bg_y = cy - bg_size / 2;
    int radius = 10;

    // Base colors for different apps
    uint32_t bg_col = 0x00FFFFFF;
    if (strcmp(label, "Terminal") == 0) bg_col = 0x002D3748; // Dark slate
    else if (strcmp(label, "Explorer") == 0) bg_col = 0x003182CE; // Vibrant Blue
    else if (strcmp(label, "SysInfo") == 0) bg_col = 0x00E2E8F0; // Light silver
    else if (strcmp(label, "Clock") == 0) bg_col = 0x00FFFFFF; // Pure white
    else if (strcmp(label, "PCI") == 0) bg_col = 0x00DD6B20; // Vibrant orange
    else if (strcmp(label, "Browser") == 0) bg_col = 0x00319795; // Teal
    else if (strcmp(label, "Snake") == 0) bg_col = 0x0038A169; // Green
    else if (strcmp(label, "Calc") == 0) bg_col = 0x00718096; // Slate gray

    // Draw base rounded squircle
    draw_rounded_rect(bg_x, bg_y, bg_size, bg_size, radius, bg_col);

    // Draw inner glyphs (Minimalist & Crisp)
    if (strcmp(label, "Terminal") == 0) {
        draw_string_px(cx - 8, cy - 4, ">_", 0x0048BB78, bg_col);
    } else if (strcmp(label, "Explorer") == 0) {
        // Folder glyph
        draw_rect(cx - 12, cy - 10, 24, 18, 0x00FFFFFF);
        draw_rect(cx - 12, cy - 12, 10, 2, 0x00EBF8FF);
        draw_rect(cx - 12, cy - 6, 24, 2, 0x0090CDF4); // Inner line detail
    } else if (strcmp(label, "SysInfo") == 0) {
        // Monitor glyph
        draw_rect(cx - 12, cy - 10, 24, 16, 0x002D3748);
        draw_rect(cx - 10, cy - 8, 20, 12, 0x00A0AEC0); // Screen
        draw_rect(cx - 4, cy + 6, 8, 4, 0x002D3748); // Stand
        draw_rect(cx - 8, cy + 10, 16, 2, 0x002D3748); // Base
    } else if (strcmp(label, "Clock") == 0) {
        // Clock glyph
        draw_circle(cx, cy, 14, 0x002D3748);
        draw_circle(cx, cy, 13, 0x002D3748);
        draw_line(cx, cy, cx, cy - 8, 0x00E53E3E); // Red minute hand
        draw_line(cx, cy, cx + 6, cy + 6, 0x002D3748); // Dark hour hand
        fill_circle(cx, cy, 2, 0x002D3748); // Center pivot
    } else if (strcmp(label, "PCI") == 0) {
        // Microchip glyph
        draw_rect(cx - 10, cy - 10, 20, 20, 0x00FFFFFF);
        for(int i=0; i<3; i++) {
            draw_rect(cx - 14, cy - 6 + i*6, 4, 2, 0x00FFFFFF); // Left pins
            draw_rect(cx + 10, cy - 6 + i*6, 4, 2, 0x00FFFFFF); // Right pins
            draw_rect(cx - 6 + i*6, cy - 14, 2, 4, 0x00FFFFFF); // Top pins
            draw_rect(cx - 6 + i*6, cy + 10, 2, 4, 0x00FFFFFF); // Bottom pins
        }
    } else if (strcmp(label, "Browser") == 0) {
        // Globe glyph
        draw_circle(cx, cy, 14, 0x00FFFFFF);
        draw_circle(cx, cy, 13, 0x00FFFFFF);
        draw_line(cx - 14, cy, cx + 14, cy, 0x00FFFFFF); // Equator
        draw_line(cx, cy - 14, cx, cy + 14, 0x00FFFFFF); // Prime meridian
        draw_circle(cx, cy, 7, 0x00FFFFFF); // Inner lat/long illusion
    } else if (strcmp(label, "Snake") == 0) {
        // Snake glyph
        draw_rect(cx - 10, cy - 4, 16, 6, 0x00FFFFFF); // Body horizontal
        draw_rect(cx + 2, cy - 10, 6, 8, 0x00FFFFFF); // Head
        draw_rect(cx - 10, cy + 2, 6, 6, 0x00FFFFFF); // Tail drop
        draw_rect(cx + 4, cy - 8, 2, 2, 0x0038A169); // Eye (green to match bg)
    } else if (strcmp(label, "Calc") == 0) {
        // Calculator glyph
        draw_rect(cx - 10, cy - 14, 20, 28, 0x00FFFFFF); // Body
        draw_rect(cx - 8, cy - 12, 16, 6, 0x00E2E8F0); // Screen
        for(int r=0; r<3; r++) {
            for(int c=0; c<3; c++) {
                draw_rect(cx - 8 + c*6, cy - 3 + r*6, 4, 4, 0x00A0AEC0); // Buttons
            }
        }
    } else {
        // Generic App glyph
        draw_rect(cx - 8, cy - 10, 16, 20, 0x002D3748);
        draw_rect(cx - 4, cy - 6, 8, 2, 0x00A0AEC0);
        draw_rect(cx - 4, cy - 2, 8, 2, 0x00A0AEC0);
        draw_rect(cx - 4, cy + 2, 8, 2, 0x00A0AEC0);
    }
}

static void draw_icon(int i) {
    Icon* ic = &icons[i];
    draw_pro_icon(ic->x, ic->y, ic->label);

    // Label below icon: modern pill-shaped background
    int llen = strlen(ic->label);
    int lw = llen * 8 + 10;   // padding 5px each side
    int lx = ic->x + (ICON_W - lw) / 2;
    int ly = ic->y + ICON_W - 4;
    int lh = 14;               // label height

    // White text centered under icon
    int tx = lx + (lw - llen * 8) / 2;
    int ty = ly + (lh - 8) / 2;
    
    // Draw text shadow for readability
    draw_string_px(tx + 1, ty + 1, ic->label, 0x00000000, 0xFFFFFFFF);
    // Draw white text (0xFFFFFFFF background is transparent in vga.c)
    draw_string_px(tx, ty, ic->label, 0x00FFFFFF, 0xFFFFFFFF);
}

extern uint32_t _binary_obj_wallpaper_bin_start[];

void desktop_draw() {
    if (!is_vbe) return;

    uint32_t area_h = fb_height - TASKBAR_H_PX;

    // Blit wallpaper
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

    // Draw desktop icons (grid, modern style)
    if (!icons[0].label) init_icons();
    for (int i = 0; i < ICON_COUNT; i++) draw_icon(i);
}

static int dragged_icon = -1;
static int drag_offset_x = 0;
static int drag_offset_y = 0;
static int drag_start_x = 0;
static int drag_start_y = 0;

void desktop_handle_mouse(int mx, int my, int btn, int pbtn) {
    int ty = (int)fb_height - TASKBAR_H_PX;
    if (my >= ty) return; // taskbar handles its own clicks

    if (calendar_open) {
        if (!btn && pbtn) calendar_open = 0;
        return;
    }

    if (start_menu_open) {
        if (!btn && pbtn) {
            int sm_h = 320;
            int sm_w = 200;
            int sm_y = ty - sm_h;
            if (mx >= 2 && mx <= 2 + sm_w && my >= sm_y && my <= sm_y + sm_h) {
                if (my >= sm_y + 36) {
                    int rel_y = my - (sm_y + 36);
                    int item = rel_y / 28;
                    if (item == 0) open_terminal_app();
                    else if (item == 1) st_ed("baru.txt");
                    else if (item == 2) open_explorer_app();
                    else if (item == 3) open_browser_app();
                    else if (item == 4) open_sysinfo_app();
                    else if (item == 5) open_clock_app();
                    else if (item == 6) open_pci_app();
                    else if (item == 7) start_ular();
                    else if (item == 8) { extern volatile int pending_logout; pending_logout = 1; }
                    else if (item == 9) open_power_dialog();
                }
            }
            start_menu_open = 0;
        }
        return;
    }

    if (!icons[0].label) return;

    // Icon hit test (within rounded rect)
    if (btn && !pbtn) {
        for (int i = 0; i < ICON_COUNT; i++) {
            Icon* ic = &icons[i];
            if (mx >= ic->x && mx < ic->x + ICON_W && my >= ic->y && my < ic->y + ICON_H + 16) {
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
                if (icons[dragged_icon].x != drag_start_x || icons[dragged_icon].y != drag_start_y) {
                    save_desktop_icons();
                }
            }
            dragged_icon = -1;
        }
    }
}