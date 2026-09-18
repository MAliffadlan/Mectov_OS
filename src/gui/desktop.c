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
static void open_elfdemo_wrapper() { load_mct_app("/apps/elfdemo.elf"); }

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
    // Grid layout: auto-arrange in a grid with ONE pitch for both axes, so the
    // horizontal and vertical spacing are identical (GRID_PITCH == hit-test
    // height ICON_H + 16, keeping click areas of adjacent rows non-overlapping).
    int grid_pitch = ICON_H + 16;
    int grid_cols = (fb_width - 40) / grid_pitch;
    if (grid_cols < 1) grid_cols = 1;
    int grid_gap_x = grid_pitch;
    int grid_gap_y = grid_pitch;
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

    // ELF Demo (real ELF32 binary — proves the ELF loader)
    icons[11] = (Icon){ start_x + 3 * grid_gap_x, start_y + 2 * grid_gap_y, "ELF Demo", open_elfdemo_wrapper };

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

// ---- Desktop Icons (v38.93: pixel-art two-tone glyphs on tinted tiles) ----
// Each icon is a 16x16 bitmap (src/include/icon_glyphs.h, 2 bits per pixel)
// blitted at 2x onto a 44px squircle. Tiles are dark, desaturated tints of
// each app's hue with a 2px bottom-right inner shade for depth — replacing
// the old saturated-tile + blocky-draw_rect glyphs. Glyphs are hand-designed
// in scripts/icon_preview.html (live HTML preview) and generated into the
// header, so what you see there is exactly what ships.
#include "../include/icon_glyphs.h"

typedef struct { const char* label; uint32_t tint; } icon_tile_t;
static const icon_tile_t icon_tiles[] = {
    { "Terminal", 0x0023272F },
    { "Browser",  0x00124A78 },
    { "Explorer", 0x001C4A66 },
    { "SysInfo",  0x00363E4A },
    { "Clock",    0x00363E4A },
    { "PCI",      0x00225044 },
    { "Snake",    0x001C5A40 },
    { "Calc",     0x003C3266 },
    { "Task Mgr", 0x00363E4A },
    { "Flappy",   0x00665224 },
    { "Notepad",  0x004A525E },
    { "ELF Demo", 0x00592B44 },
};

static const uint32_t* icon_glyph(const char* label) {
    if      (strcmp(label, "Terminal") == 0) return glyph_terminal;
    else if (strcmp(label, "Browser")  == 0) return glyph_browser;
    else if (strcmp(label, "Explorer") == 0) return glyph_explorer;
    else if (strcmp(label, "SysInfo")  == 0) return glyph_sysinfo;
    else if (strcmp(label, "Clock")    == 0) return glyph_clock;
    else if (strcmp(label, "PCI")      == 0) return glyph_pci;
    else if (strcmp(label, "Snake")    == 0) return glyph_snake;
    else if (strcmp(label, "Calc")     == 0) return glyph_calc;
    else if (strcmp(label, "Task Mgr") == 0) return glyph_task_mgr;
    else if (strcmp(label, "Flappy")   == 0) return glyph_flappy;
    else if (strcmp(label, "Notepad")  == 0) return glyph_notepad;
    else if (strcmp(label, "ELF Demo") == 0) return glyph_elf_demo;
    return glyph_generic;
}

static void draw_pro_icon(int ix, int iy, const char* label) {
    int cx = ix + ICON_W / 2;
    int cy = iy + ICON_W / 2 - 6;
    int bg_size = 44;
    int bg_x = cx - bg_size / 2;
    int bg_y = cy - bg_size / 2;
    int radius = 10;

    // Tile: single flat rounded rect in the app's tint. No shadow, no
    // offset dark edge (v38.94) — cheapest possible tile, zero extra fills.
    uint32_t tint = 0x00363E4A;
    for (unsigned i = 0; i < sizeof(icon_tiles)/sizeof(icon_tiles[0]); i++) {
        if (strcmp(label, icon_tiles[i].label) == 0) { tint = icon_tiles[i].tint; break; }
    }
    draw_rounded_rect(bg_x, bg_y, bg_size, bg_size, radius, tint);

    // Glyph: 16x16 two-tone bitmap at 2x (32px) centered on the tile.
    // 0 = transparent, 1 = foreground (light), 2 = inner (dark recess).
    // v38.94: run-length blit — consecutive same-color pixels coalesce into
    // one draw_rect (2x scale means a 16px row is at most 8 rects per color;
    // typical glyphs issue ~10-16 rects per row instead of up to 16).
    static const uint32_t FG = 0x00E8EDF3, IN = 0x002E3440;
    const uint32_t* g = icon_glyph(label);
    int ox = cx - 16, oy = cy - 16;
    for (int y = 0; y < 16; y++) {
        uint32_t row = g[y];
        if (!row) continue;
        int x = 0;
        while (x < 16) {
            uint32_t v = (row >> (2 * x)) & 3;
            if (v == 0) { x++; continue; }
            uint32_t col = (v == 1) ? FG : IN;
            int run = 1;
            while (x + run < 16 && ((row >> (2 * (x + run))) & 3) == v) run++;
            draw_rect(ox + 2*x, oy + 2*y, 2*run, 2, col);
            x += run;
        }
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

    // Label below the icon — plain white text with a 1px shadow (readable
    // on any wallpaper, no chip). The glyph font is 8x16 (font8x16_data),
    // NOT 8x8, so the label box must be 16px tall — a 14px box clipped the
    // bottom half of long labels that take the vga_set_clip() path below
    // (glyph rows 11-15 landed outside the clip rect).
    int llen = strlen(ic->label);
    int lw = llen * 8 + 10;   // padding 5px each side
    int lx = ic->x + (ICON_W - lw) / 2;
    int ly = ic->y + ICON_W - 4;
    int lh = 16;               // label height (matches the 8x16 font)

    // White text centered under icon
    int tx = lx + (lw - llen * 8) / 2;
    int ty = ly;               // glyph rows fill the 16px label box exactly

    // Clip the label to the icon cell width ONLY when it overflows the cell,
    // so a long label never paints over the neighbouring icon.
    if (lw > ICON_W) {
        vga_set_clip(ic->x, ly, ICON_W, lh);
        // Draw text shadow for readability
        draw_string_px(tx + 1, ty + 1, ic->label, 0x00000000, 0xFFFFFFFF);
        // Draw white text (0xFFFFFFFF background is transparent in vga.c)
        draw_string_px(tx, ty, ic->label, 0x00FFFFFF, 0xFFFFFFFF);
        vga_reset_clip();
    } else {
        // Draw text shadow for readability
        draw_string_px(tx + 1, ty + 1, ic->label, 0x00000000, 0xFFFFFFFF);
        // Draw white text (0xFFFFFFFF background is transparent in vga.c)
        draw_string_px(tx, ty, ic->label, 0x00FFFFFF, 0xFFFFFFFF);
    }
}

#include "../include/assets.h"   // on-demand wallpaper (debloat v38.81)

static int ctx_menu_open = 0;
static int ctx_menu_x = 0;
static int ctx_menu_y = 0;
static int ctx_menu_last_hover = -1;

void desktop_draw() {
    if (!is_vbe) return;

    uint32_t area_h = fb_height - TASKBAR_H_PX;

    // Blit wallpaper clipped to dirty rect (on-demand, debloat v38.81).
    // Without the blob the whole area is a flat fill so the desktop stays
    // usable on images that were never seeded.
    extern int d_min_x, d_min_y, d_max_x, d_max_y;
    const uint32_t* wp_ptr = assets_wallpaper();
    uint32_t wp_w = 1024, wp_h = 768;
    if (!wp_ptr) {
        if (d_max_x > 0 && d_max_y > 0)
            draw_rect(0, 0, fb_width, area_h, 0x00111122);
    } else {
    uint32_t copy_w = (fb_width < wp_w) ? fb_width : wp_w;
    uint32_t copy_h = (area_h < wp_h) ? area_h : wp_h;
    
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

// Double-click window in PIT ticks: 800 ms of wall time. The PIT is nominally
// 1000 Hz but under QEMU TCG the emulated clock runs faster than real time
// (measured up to ~3x locally, more under CI), so a fixed 800 ticks would be
// far shorter in real seconds. Scale by the calibrated ticks_per_sec.
static uint32_t double_click_window_ticks(void) {
    extern volatile uint32_t ticks_per_sec;
    uint32_t w = (ticks_per_sec * 8) / 10;   // 800 ms
    return (w >= 200) ? w : 800;             // sane floor / uncalibrated fallback
}

// True while an icon drag is in flight. The kernel uses this to keep routing
// mouse events to the desktop even when the cursor crosses into the taskbar,
// so the release is handled here (snap-back/save) instead of being eaten by
// a taskbar button click.
int desktop_drag_active(void) { return dragged_icon != -1; }

void desktop_handle_mouse(int mx, int my, int btn, int pbtn) {
    int ty = (int)fb_height - TASKBAR_H_PX;
    if (my >= ty && !desktop_drag_active()) return; // taskbar handles its own clicks

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
            // Hover check: refresh only when the hovered item CHANGES (enter,
            // leave, or switch). Redrawing on every move was fine, but NOT
            // marking when the mouse leaves the menu left the blue highlight
            // stuck on the last item.
            int hover = -1;
            if (mx >= ctx_menu_x && mx < ctx_menu_x + 130 &&
                my >= ctx_menu_y && my < ctx_menu_y + 78) {
                hover = (my - ctx_menu_y - 3) / 18;
                if (hover < 0 || hover > 3) hover = -1;
            }
            if (hover != ctx_menu_last_hover) {
                ctx_menu_last_hover = hover;
                extern void mark_dirty(int, int, int, int);
                mark_dirty(ctx_menu_x, ctx_menu_y, 130, 78);
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
                uint32_t now = get_ticks();
                dragged_icon = i;
                drag_offset_x = mx - ic->x;
                drag_offset_y = my - ic->y;
                drag_start_x = ic->x;
                drag_start_y = ic->y;

                // Double click detection directly on mouse down to bypass drag coordinate jumps
                if (i == last_clicked_icon && (now - last_click_tick) < double_click_window_ticks()) {
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