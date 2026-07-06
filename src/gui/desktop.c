#include "../include/desktop.h"
#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/wm.h"
#include "../include/apps.h"
#include "../include/timer.h"
#include "../include/mem.h"
#include "../include/taskbar.h"
#include "../include/vfs.h"
#include "../include/mouse.h"

// Forward declarations for GUI apps
void open_terminal_app();
void open_clock_app();
void open_sysinfo_app();

// ---- Icon definitions ----
typedef struct { int x, y; const char* label; void (*action)(); } Icon;
static Icon icons[ICON_COUNT];

extern int load_mct_app(const char*);
static void open_calc_wrapper() { load_mct_app("/apps/gcalc.mct"); }
static void open_snake_wrapper() { load_mct_app("/apps/snake.mct"); }
static void open_sysinfo_wrapper() { load_mct_app("/apps/sysinfo.mct"); }
static void open_pci_wrapper() { load_mct_app("/apps/pci.mct"); }
static void open_explorer_wrapper() { load_mct_app("/apps/explorer.mct"); }
static void open_browser_wrapper() { load_mct_app("/apps/browser.mct"); }
static void open_taskmgr_wrapper() { load_mct_app("/apps/taskmgr.mct"); }
static void open_flappy_wrapper() { load_mct_app("/apps/flappy.mct"); }
static void open_notepad_wrapper() { load_mct_app("/apps/notepad.mct"); }

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
    icons[1] = (Icon){ start_x + 1 * grid_gap_x, start_y, "Browser",   open_browser_wrapper   };
    icons[2] = (Icon){ start_x + 2 * grid_gap_x, start_y, "Explorer",  open_explorer_wrapper  };
    icons[3] = (Icon){ start_x + 3 * grid_gap_x, start_y, "SysInfo",   open_sysinfo_wrapper   };
    icons[4] = (Icon){ start_x + 0 * grid_gap_x, start_y + 1 * grid_gap_y, "Clock",     open_clock_app     };
    icons[5] = (Icon){ start_x + 1 * grid_gap_x, start_y + 1 * grid_gap_y, "PCI",       open_pci_wrapper       };
    icons[6] = (Icon){ start_x + 2 * grid_gap_x, start_y + 1 * grid_gap_y, "Snake",     open_snake_wrapper };
    icons[7] = (Icon){ start_x + 3 * grid_gap_x, start_y + 1 * grid_gap_y, "Calc",      open_calc_wrapper  };

    // Task Manager kini Ring 3
    icons[8] = (Icon){ start_x + 0 * grid_gap_x, start_y + 2 * grid_gap_y, "Task Mgr",  open_taskmgr_wrapper };
    
    // Flappy Bird
    icons[9] = (Icon){ start_x + 1 * grid_gap_x, start_y + 2 * grid_gap_y, "Flappy",    open_flappy_wrapper };

    // Notepad
    icons[10] = (Icon){ start_x + 2 * grid_gap_x, start_y + 2 * grid_gap_y, "Notepad",  open_notepad_wrapper };

    // Load saved positions (with validation to prevent corrupt data)
    int read_buf[ICON_COUNT * 2];
    int sz = vfs_read_file("icons.cfg", (char*)read_buf, sizeof(read_buf));
    if (sz >= 8) { // Minimal 1 icon (2 * sizeof(int) = 8 bytes)
        int saved_count = sz / (2 * sizeof(int));
        if (saved_count > ICON_COUNT) saved_count = ICON_COUNT;
        int valid = 1;
        for (int i = 0; i < saved_count; i++) {
            // Reject if any icon is out of screen bounds
            if (read_buf[i*2] < 0 || read_buf[i*2] >= (int)fb_width ||
                read_buf[i*2+1] < 0 || read_buf[i*2+1] >= (int)fb_height) {
                valid = 0;
                break;
            }
        }
        if (valid) {
            for (int i = 0; i < saved_count; i++) {
                icons[i].x = read_buf[i*2];
                icons[i].y = read_buf[i*2+1];
            }
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
    else if (strcmp(label, "Browser") == 0) bg_col = 0x00D69E2E; // Gold/Yellow
    else if (strcmp(label, "Task Mgr") == 0) bg_col = 0x004A5568; // Gray
    else if (strcmp(label, "Snake") == 0) bg_col = 0x0038A169; // Green
    else if (strcmp(label, "Flappy") == 0) bg_col = 0x00ECC94B; // Yellow
    else if (strcmp(label, "Notepad") == 0) bg_col = 0x00E2E8F0; // Light gray
    else bg_col = 0x00718096; // Default Gray

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
    } else if (strcmp(label, "Task Mgr") == 0) {
        // Simple list/graph icon
        draw_rect(cx - 10, cy - 8, 20, 16, 0x00FFFFFF);
        draw_rect(cx - 8, cy - 6, 16, 3, 0x00CBD5E0);
        draw_rect(cx - 8, cy - 1, 16, 3, 0x00CBD5E0);
        draw_rect(cx - 8, cy + 4, 16, 3, 0x00CBD5E0);
        draw_rect(cx - 8, cy - 6, 4, 3, 0x00E53E3E); // red dot
    } else if (strcmp(label, "Flappy") == 0) {
        // Bird glyph
        draw_rect(cx - 6, cy - 6, 12, 12, 0x00FFFFFF); // Body
        draw_rect(cx + 2, cy - 4, 2, 2, 0x00000000); // Eye
        draw_rect(cx + 6, cy, 4, 4, 0x00E53E3E); // Beak (Reddish)
        draw_rect(cx - 10, cy, 4, 4, 0x00FFFFFF); // Wing
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
    extern int d_min_x, d_min_y, d_max_x, d_max_y;
    // Check overlap with dirty rect (icon box is roughly ic->x to ic->x + 80, ic->y to ic->y + 90)
    if (ic->x + 80 <= d_min_x || ic->x >= d_max_x || ic->y + 90 <= d_min_y || ic->y >= d_max_y) {
        return;
    }
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

static int ctx_menu_open = 0;
static int ctx_menu_x = 0;
static int ctx_menu_y = 0;

void desktop_draw() {
    if (!is_vbe) return;

    uint32_t area_h = fb_height - TASKBAR_H_PX;

    // Blit wallpaper clipped to dirty rect
    uint32_t* wp_ptr = _binary_obj_wallpaper_bin_start;
    uint32_t wp_w = 1024, wp_h = 768;
    uint32_t copy_w = (fb_width < wp_w) ? fb_width : wp_w;
    uint32_t copy_h = (area_h < wp_h) ? area_h : wp_h;
    
    extern int d_min_x, d_min_y, d_max_x, d_max_y;
    int start_y = d_min_y < 0 ? 0 : d_min_y;
    int end_y = d_max_y > (int)copy_h ? (int)copy_h : d_max_y;
    int start_x = d_min_x < 0 ? 0 : d_min_x;
    int end_x = d_max_x > (int)copy_w ? (int)copy_w : d_max_x;

    if (start_y < end_y && start_x < end_x) {
        uint32_t copy_bytes = (end_x - start_x) * 4;
        for (int y = start_y; y < end_y; y++) {
            memcpy(&back_buffer[y * fb_width + start_x], &wp_ptr[y * wp_w + start_x], copy_bytes);
        }
    }

    // Fill remaining edges if screen is larger than wallpaper (only if dirty rect overlaps them)
    if (fb_width > wp_w && d_max_x > (int)wp_w) {
        draw_rect(wp_w, 0, fb_width - wp_w, area_h, 0x00111122);
    }
    if (area_h > wp_h && d_max_y > (int)wp_h) {
        draw_rect(0, wp_h, fb_width, area_h - wp_h, 0x00111122);
    }

    // Draw desktop icons (grid, modern style)
    if (!icons[0].label) init_icons();
    for (int i = 0; i < ICON_COUNT; i++) draw_icon(i);

    // Draw Right-Click Context Menu
    if (ctx_menu_open) {
        int dx = ctx_menu_x;
        int dy = ctx_menu_y;
        int dw = 130;
        int dh = 78;
        
        // Background card
        draw_rect(dx, dy, dw, dh, 0x00181825);
        
        // Border lines
        draw_rect(dx, dy, dw, 1, 0x00313244);
        draw_rect(dx, dy + dh - 1, dw, 1, 0x00313244);
        draw_rect(dx, dy, 1, dh, 0x00313244);
        draw_rect(dx + dw - 1, dy, 1, dh, 0x00313244);
        
        const char* menu_items[] = {
            "Open Terminal",
            "Open Explorer",
            "System Info",
            "Refresh"
        };
        
        for (int i = 0; i < 4; i++) {
            int iy = dy + 3 + i * 18;
            // Check hover based on global mouse_x / mouse_y
            if (mouse_x >= dx && mouse_x < dx + dw && mouse_y >= iy && mouse_y < iy + 18) {
                draw_rect(dx + 2, iy, dw - 4, 16, 0x0089B4FA); // blue hover
                draw_string_px(dx + 8, iy + 4, menu_items[i], 0x0011111B, 0xFFFFFFFF);
            } else {
                draw_string_px(dx + 8, iy + 4, menu_items[i], 0x00CDD6F4, 0xFFFFFFFF);
            }
        }
    }
}

static int dragged_icon = -1;
static int drag_offset_x = 0;
static int drag_offset_y = 0;
static int drag_start_x = 0;
static int drag_start_y = 0;

static int last_clicked_icon = -1;
static uint32_t last_click_tick = 0;

void desktop_handle_mouse(int mx, int my, int btn, int pbtn) {
    int ty = (int)fb_height - TASKBAR_H_PX;
    if (my >= ty) return; // taskbar handles its own clicks

    // --- Right-click Context Menu logic ---
    if (ctx_menu_open) {
        if ((btn & 1) && !(pbtn & 1)) {
            // Left click: check context menu bounds
            int dx = ctx_menu_x;
            int dy = ctx_menu_y;
            int dw = 130;
            int dh = 78;
            if (mx >= dx && mx < dx + dw && my >= dy && my < dy + dh) {
                int item = (my - dy - 3) / 18;
                if (item == 0) {
                    open_terminal_app();
                } else if (item == 1) {
                    open_explorer_wrapper();
                } else if (item == 2) {
                    open_sysinfo_wrapper();
                } else if (item == 3) {
                    vfs_load();
                    init_icons();
                }
            }
            ctx_menu_open = 0;
            extern void mark_dirty(int, int, int, int);
            mark_dirty(dx, dy, dw, dh); // Erase context menu cleanly
            extern int needs_redraw;
            needs_redraw = 1;
            return;
        } else if ((btn & 2) && !(pbtn & 2)) {
            // Right click while menu open: reposition it
            ctx_menu_open = 1;
            ctx_menu_x = mx;
            ctx_menu_y = my;
            if (ctx_menu_x + 130 > (int)fb_width) ctx_menu_x = fb_width - 130;
            if (ctx_menu_y + 78 > ty) ctx_menu_y = ty - 78;
            extern void mark_dirty(int, int, int, int);
            mark_dirty(0, 0, fb_width, fb_height); // Refresh screen for reposition
            extern int needs_redraw;
            needs_redraw = 1;
            return;
        } else {
            // Hover check: if mouse moves over context menu, refresh it
            int dx = ctx_menu_x;
            int dy = ctx_menu_y;
            int dw = 130;
            int dh = 78;
            if (mx >= dx && mx < dx + dw && my >= dy && my < dy + dh) {
                extern void mark_dirty(int, int, int, int);
                mark_dirty(dx, dy, dw, dh);
                extern int needs_redraw;
                needs_redraw = 1;
            }
        }
        return;
    }

    if ((btn & 2) && !(pbtn & 2)) {
        // Right click: open context menu
        ctx_menu_open = 1;
        ctx_menu_x = mx;
        ctx_menu_y = my;
        if (ctx_menu_x + 130 > (int)fb_width) ctx_menu_x = fb_width - 130;
        if (ctx_menu_y + 78 > ty) ctx_menu_y = ty - 78;
        extern void mark_dirty(int, int, int, int);
        mark_dirty(ctx_menu_x, ctx_menu_y, 130, 78);
        extern int needs_redraw;
        needs_redraw = 1;
        return;
    }

    // Popup dismissal is now handled in kernel.c before this function is called

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

                // Double click detection directly on mouse down to bypass drag coordinate jumps
                uint32_t now = get_ticks();
                if (i == last_clicked_icon && (now - last_click_tick) < 800) {
                    if (ic->action) {
                        ic->action();
                    }
                    last_clicked_icon = -1;
                    last_click_tick = 0;
                    dragged_icon = -1; // Cancel drag on double-click
                } else {
                    last_clicked_icon = i;
                    last_click_tick = now;
                }
                return;
            }
        }
    } else if (btn && pbtn) {
        if (dragged_icon != -1) {
            extern void mark_dirty(int, int, int, int);
            // Mark old position dirty to erase
            mark_dirty(icons[dragged_icon].x - 4, icons[dragged_icon].y - 4, 88, 104);

            int new_x = mx - drag_offset_x;
            int new_y = my - drag_offset_y;

            // Clamp to desktop bounds to prevent icons from getting lost off-screen
            if (new_x < 10) new_x = 10;
            if (new_x > (int)fb_width - ICON_W - 10) new_x = fb_width - ICON_W - 10;
            if (new_y < 10) new_y = 10;
            if (new_y > (int)fb_height - TASKBAR_H_PX - ICON_H - 26) new_y = fb_height - TASKBAR_H_PX - ICON_H - 26;

            icons[dragged_icon].x = new_x;
            icons[dragged_icon].y = new_y;

            // Mark new position dirty to draw
            mark_dirty(icons[dragged_icon].x - 4, icons[dragged_icon].y - 4, 88, 104);

            extern int needs_redraw;
            needs_redraw = 1;
        }
    } else if (!btn && pbtn) {
        if (dragged_icon != -1) {
            int dx = icons[dragged_icon].x - drag_start_x;
            int dy = icons[dragged_icon].y - drag_start_y;
            int dist_sq = dx * dx + dy * dy;

            // Save new position only if dragged significantly (>= 10 pixels)
            if (dist_sq >= 100) {
                save_desktop_icons();
            } else {
                // Snap back to start position to prevent accidental shifts on single click
                extern void mark_dirty(int, int, int, int);
                mark_dirty(icons[dragged_icon].x - 4, icons[dragged_icon].y - 4, 88, 104);
                icons[dragged_icon].x = drag_start_x;
                icons[dragged_icon].y = drag_start_y;
                mark_dirty(icons[dragged_icon].x - 4, icons[dragged_icon].y - 4, 88, 104);
                
                extern int needs_redraw;
                needs_redraw = 1;
            }
            dragged_icon = -1;
        }
    }
}