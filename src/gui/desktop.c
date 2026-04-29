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

// ---- Glass-morphism icons (Frosted Glass style) ----
// Semi-transparent dark bg + white border + subtle shadow + white glyph
static void draw_modern_icon(int ix, int iy, const char* label, uint32_t accent) {
    int irad = 14;
    
    // Drop shadow behind icon
    draw_soft_shadow(ix - 2, iy - 2, ICON_W + 4, ICON_W + 4, 4, 3);
    
    // Frosted glass bg: semi-transparent dark + subtle accent tint
    uint32_t glass_bg = 0x55222233; // translucent dark purple-ish
    draw_rounded_rect(ix, iy, ICON_W, ICON_W, irad, glass_bg);
    
    // White border, thin (1px), slightly transparent for elegance
    draw_rounded_rect_border(ix, iy, ICON_W, ICON_W, irad, 0x33FFFFFF);
    // Inner brighter border (1px inset)
    draw_rounded_rect_border(ix + 1, iy + 1, ICON_W - 2, ICON_W - 2, irad - 1, 0x18FFFFFF);
    
    // Center coordinates
    int cx = ix + ICON_W/2;
    int cy = iy + ICON_W/2;

    // White glyph
    uint32_t glyph_col = 0x00FFFFFF;

    if (strcmp(label, "Terminal") == 0) {
        draw_string_px(cx - 10, cy - 5, ">_", glyph_col, 0x00000000);
    } else if (strcmp(label, "Browser") == 0) {
        draw_circle(cx, cy - 1, 14, glyph_col);
        draw_line(cx - 12, cy - 1, cx + 12, cy - 1, glyph_col);
        draw_line(cx - 10, cy - 7, cx + 10, cy - 7, glyph_col);
        draw_line(cx - 10, cy + 5, cx + 10, cy + 5, glyph_col);
        draw_line(cx, cy - 15, cx, cy + 13, glyph_col);
    } else if (strcmp(label, "Explorer") == 0) {
        draw_rect(cx - 14, cy - 4, 28, 18, glyph_col);
        draw_rounded_rect(cx - 12, cy - 6, 14, 6, 2, glyph_col);
        draw_rect(cx - 12, cy - 6, 14, 6, glyph_col);
    } else if (strcmp(label, "SysInfo") == 0) {
        draw_circle(cx, cy - 1, 14, glyph_col);
        draw_string_px(cx - 3, cy - 6, "i", 0x00222233, 0x00000000);
    } else if (strcmp(label, "Clock") == 0) {
        draw_circle(cx, cy - 1, 14, glyph_col);
        draw_line(cx, cy - 1, cx, cy - 9, glyph_col);
        draw_line(cx, cy - 1, cx + 6, cy - 1, glyph_col);
    } else if (strcmp(label, "PCI") == 0) {
        draw_rounded_rect(cx - 12, cy - 10, 24, 20, 3, glyph_col);
        for (int p = -2; p <= 2; p++) {
            draw_rect(cx - 10 + p*5, cy - 15, 3, 5, glyph_col);
            draw_rect(cx - 10 + p*5, cy + 10, 3, 5, glyph_col);
        }
    } else if (strcmp(label, "Snake") == 0) {
        draw_rounded_rect(cx - 8, cy - 4, 16, 8, 4, glyph_col);
        draw_rounded_rect(cx + 4, cy - 4, 8, 8, 3, glyph_col);
        draw_rounded_rect(cx + 4, cy - 12, 8, 8, 3, glyph_col);
        draw_rounded_rect(cx - 4, cy - 12, 8, 8, 3, 0x00FFFFFF);
    } else if (strcmp(label, "Calc") == 0) {
        draw_rounded_rect(cx - 12, cy - 14, 24, 28, 4, glyph_col);
        draw_rect(cx - 10, cy - 12, 20, 8, 0x00222233);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                draw_rounded_rect(cx - 10 + c*7, cy - 2 + r*7, 6, 6, 2, 0x00222233);
    }
}

static uint32_t get_accent(const char* label) {
    if (strcmp(label, "Terminal") == 0) return GUI_GREEN;
    if (strcmp(label, "Browser") == 0)  return GUI_BLUE;
    if (strcmp(label, "Explorer") == 0) return GUI_YELLOW;
    if (strcmp(label, "SysInfo") == 0)  return 0x0044BBFF;
    if (strcmp(label, "Clock") == 0)    return GUI_YELLOW;
    if (strcmp(label, "PCI") == 0)      return 0x00448888;
    if (strcmp(label, "Snake") == 0)    return GUI_GREEN;
    if (strcmp(label, "Calc") == 0)     return 0x00AAAAAA;
    return GUI_BLUE;
}

static void draw_icon(int i) {
    Icon* ic = &icons[i];
    uint32_t accent = get_accent(ic->label);
    draw_modern_icon(ic->x, ic->y, ic->label, accent);

    // Label below icon: centered, with subtle shadow
    int llen = strlen(ic->label);
    int lx = ic->x + (ICON_W - llen * 8) / 2;
    draw_string_px(lx + 1, ic->y + ICON_W + 3, ic->label, 0x00222233, 0x00000000);
    draw_string_px(lx, ic->y + ICON_W + 2, ic->label, GUI_TEXT, 0x00000000);
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
            int sm_h = 280;
            int sm_w = 170;
            int sm_y = ty - sm_h;
            if (mx >= 2 && mx <= 2 + sm_w && my >= sm_y && my <= sm_y + sm_h) {
                if (my >= sm_y + 30) {
                    int rel_y = my - (sm_y + 30);
                    int item = rel_y / 24;
                    if (item == 0) open_terminal_app();
                    else if (item == 1) st_ed("baru.txt");
                    else if (item == 2) open_explorer_app();
                    else if (item == 3) open_browser_app();
                    else if (item == 4) open_sysinfo_app();
                    else if (item == 5) open_clock_app();
                    else if (item == 6) open_pci_app();
                    else if (item == 7) start_ular();
                    else if (item == 8) shutdown();
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