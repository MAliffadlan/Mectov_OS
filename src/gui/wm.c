#include "../include/wm.h"
#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/taskbar.h"
#include "../include/serial.h"

#define SNAP_THRESHOLD  10      // px from edge to trigger snap
#define SNAP_AREA_W     (int)(fb_width / 2)  // half width for left/right snap

WmWin wm_wins[MAX_WINDOWS];
int   wm_focused = -1;
int   wm_zorder[MAX_WINDOWS];
int   wm_zcount = 0;
static int next_id = 1;

// Snap state constants
#define SNAP_NONE   0
#define SNAP_LEFT   1
#define SNAP_RIGHT  2
#define SNAP_TOP    3   // same as maximized

void wm_init() {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        wm_wins[i].visible = 0;
        wm_wins[i].id = 0;
        wm_wins[i].snap_state = SNAP_NONE;
        wm_wins[i].owner_task = -1;
    }
    wm_zcount = 0;
    wm_focused = -1;
}

// ---- Z-order helpers ----
static void z_remove(int idx) {
    for (int i = 0; i < wm_zcount; i++) {
        if (wm_zorder[i] == idx) {
            for (int j = i; j < wm_zcount - 1; j++) wm_zorder[j] = wm_zorder[j+1];
            wm_zcount--;
            return;
        }
    }
}

void wm_raise(int id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm_wins[i].visible && wm_wins[i].id == id) {
            z_remove(i);
            if (wm_zcount < MAX_WINDOWS) wm_zorder[wm_zcount++] = i;
            wm_focused = id;
            extern void mark_dirty(int, int, int, int);
            mark_dirty(0, 0, fb_width, fb_height); // Mark fullscreen dirty on focus change
            extern volatile int needs_redraw;
            needs_redraw = 1;
            return;
        }
    }
}

// ---- Minimize / restore ----
// These own BOTH halves of the operation: the state change and the damage it
// causes. Do not set .minimized directly from a call site — draw_one() skips
// minimized windows, so nothing will ever repaint over the pixels the window
// left behind and it stays painted on screen as a ghost.
void wm_minimize(int id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wm_wins[i].visible || wm_wins[i].id != id) continue;
        if (wm_wins[i].minimized) return; // already hidden, nothing to erase

        extern void mark_dirty(int, int, int, int);
        mark_dirty(wm_wins[i].x, wm_wins[i].y, wm_wins[i].w, wm_wins[i].h);
        wm_wins[i].minimized = 1;

        // Focus falls through to the topmost window still on screen
        wm_focused = -1;
        for (int zz = wm_zcount - 1; zz >= 0; zz--) {
            WmWin* nw = &wm_wins[wm_zorder[zz]];
            if (nw->visible && !nw->minimized) {
                wm_focused = nw->id;
                break;
            }
        }

        extern volatile int needs_redraw;
        needs_redraw = 1;
        return;
    }
}

void wm_restore(int id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm_wins[i].visible && wm_wins[i].id == id) {
            wm_wins[i].minimized = 0;
            wm_raise(id); // marks the screen dirty and sets needs_redraw
            return;
        }
    }
}

void wm_focus_next(void) {
    if (wm_zcount <= 1) return;
    
    // Circularly rotate z-order to cycle focus: move top to bottom
    int top_idx = wm_zorder[wm_zcount - 1];
    for (int i = wm_zcount - 1; i > 0; i--) {
        wm_zorder[i] = wm_zorder[i - 1];
    }
    wm_zorder[0] = top_idx;
    
    // Find the new top window index in wm_wins
    int new_top_idx = wm_zorder[wm_zcount - 1];
    wm_focused = wm_wins[new_top_idx].id;
    // Exempt from wm_restore(): that would call wm_raise(), which rebuilds the
    // z-order and would undo the rotation above. The fullscreen mark_dirty on
    // the next line covers the damage instead.
    wm_wins[new_top_idx].minimized = 0; // Restore if minimized
    extern void mark_dirty(int, int, int, int);
    mark_dirty(0, 0, fb_width, fb_height); // Mark fullscreen dirty on z-order shift
    extern volatile int needs_redraw;
    needs_redraw = 1;
}

int alt_tab_active = 0;
int alt_tab_selected_idx = 0;

void wm_alt_tab_start(void) {
    if (wm_zcount <= 1) return;
    alt_tab_active = 1;
    alt_tab_selected_idx = 1; // Highlight the second window by default
    extern void mark_dirty(int, int, int, int);
    mark_dirty(0, 0, fb_width, fb_height); // Show HUD card cleanly
}

void wm_alt_tab_next(void) {
    if (!alt_tab_active || wm_zcount <= 1) return;
    alt_tab_selected_idx = (alt_tab_selected_idx + 1) % wm_zcount;
}

void wm_alt_tab_end(void) {
    if (!alt_tab_active) return;
    alt_tab_active = 0;
    
    extern void mark_dirty(int, int, int, int);
    mark_dirty(0, 0, fb_width, fb_height); // Erase HUD card cleanly

    if (wm_zcount <= 1) return;
    
    // Clamp selection index if z-count changed (e.g. windows closed)
    if (alt_tab_selected_idx >= wm_zcount) {
        alt_tab_selected_idx = wm_zcount - 1;
    }
    if (alt_tab_selected_idx < 0) {
        alt_tab_selected_idx = 0;
    }
    
    // Select the highlighted window: HUD lists windows from top to bottom
    int target_idx = wm_zorder[wm_zcount - 1 - alt_tab_selected_idx];
    wm_restore(wm_wins[target_idx].id);
}

static void draw_hud_icon(int ix, int iy, const char* title) {
    uint32_t bg_col = 0x00FFFFFF;
    if (strncmp(title, "Terminal", 8) == 0) bg_col = 0x002D3748;
    else if (strncmp(title, "File Expl", 9) == 0 || strncmp(title, "Explorer", 8) == 0) bg_col = 0x003182CE;
    else if (strncmp(title, "System In", 9) == 0 || strncmp(title, "SysInfo", 7) == 0) bg_col = 0x00E2E8F0;
    else if (strncmp(title, "Clock", 5) == 0) bg_col = 0x00FFFFFF;
    else if (strncmp(title, "PCI", 3) == 0) bg_col = 0x00DD6B20;
    else if (strncmp(title, "Mini Brow", 9) == 0 || strncmp(title, "Browser", 7) == 0) bg_col = 0x00319795;
    else if (strncmp(title, "Snake", 5) == 0) bg_col = 0x0038A169;
    else if (strncmp(title, "Calc", 4) == 0 || strncmp(title, "Calculator", 10) == 0) bg_col = 0x00718096;
    else if (strncmp(title, "Editor", 6) == 0 || strncmp(title, "Notepad", 7) == 0) bg_col = 0x00718096;
    else if (strncmp(title, "Task Mgr", 8) == 0 || strncmp(title, "Task Manager", 12) == 0) bg_col = 0x004A5568;
    else if (strncmp(title, "Flappy", 6) == 0) bg_col = 0x00ECC94B;
    else if (strncmp(title, "Media", 5) == 0 || strncmp(title, "Mplayer", 7) == 0) bg_col = 0x00D53F8C;
    else bg_col = 0x00718096;

    int size = 24;
    int cx = ix + 12;
    int cy = iy + 12;

    draw_rounded_rect(ix, iy, size, size, 6, bg_col);

    if (strncmp(title, "Terminal", 8) == 0) {
        draw_string_px(ix + 4, iy + 4, ">_", 0x0048BB78, 0xFFFFFFFF);
    } else if (strncmp(title, "File Expl", 9) == 0 || strncmp(title, "Explorer", 8) == 0) {
        draw_rect(cx - size/3, cy - size/4, size*2/3, size/2, 0x00FFFFFF);
        draw_rect(cx - size/3, cy - size/4 - 1, size/4, 2, 0x00EBF8FF);
    } else if (strncmp(title, "System In", 9) == 0 || strncmp(title, "SysInfo", 7) == 0) {
        draw_rect(cx - size/3, cy - size/4, size*2/3, size/2, 0x002D3748);
        draw_rect(cx - size/4, cy - size/6, size/2, size/3, 0x00A0AEC0);
    } else if (strncmp(title, "Clock", 5) == 0) {
        draw_circle(cx, cy, size/2 - 2, 0x002D3748);
        draw_line(cx, cy, cx, cy - size/4, 0x00E53E3E);
        draw_line(cx, cy, cx + size/6, cy + size/6, 0x002D3748);
    } else if (strncmp(title, "PCI", 3) == 0) {
        draw_rect(cx - size/3, cy - size/3, size*2/3, size*2/3, 0x00FFFFFF);
    } else if (strncmp(title, "Mini Brow", 9) == 0 || strncmp(title, "Browser", 7) == 0) {
        draw_circle(cx, cy, size/2 - 2, 0x00FFFFFF);
        draw_line(cx - size/2 + 2, cy, cx + size/2 - 2, cy, 0x00FFFFFF);
        draw_line(cx, cy - size/2 + 2, cx, cy + size/2 - 2, 0x00FFFFFF);
    } else if (strncmp(title, "Task Mgr", 8) == 0 || strncmp(title, "Task Manager", 12) == 0) {
        draw_rect(ix + 4, iy + 4, 16, 16, 0x00FFFFFF);
        draw_rect(ix + 7, iy + 7, 10, 3, 0x00CBD5E0);
        draw_rect(ix + 7, iy + 13, 10, 3, 0x00CBD5E0);
    } else if (strncmp(title, "Flappy", 6) == 0) {
        draw_rect(ix + 6, iy + 6, 12, 12, 0x00FFFFFF);
        draw_rect(ix + 14, iy + 10, 4, 4, 0x00E53E3E);
    } else if (strncmp(title, "Snake", 5) == 0) {
        draw_rect(cx - size/3, cy - 2, size/2, 4, 0x00FFFFFF);
        draw_rect(cx + 2, cy - size/3, 4, size/3, 0x00FFFFFF);
    } else if (strncmp(title, "Calc", 4) == 0 || strncmp(title, "Calculator", 10) == 0) {
        draw_rect(cx - 6, cy - 6, 12, 12, 0x00FFFFFF);
        draw_rect(cx - 4, cy - 4, 8, 2, 0x0011111B);
    } else if (strncmp(title, "Editor", 6) == 0 || strncmp(title, "Notepad", 7) == 0) {
        draw_rect(cx - 5, cy - 7, 10, 14, 0x00FFFFFF);
        draw_rect(cx - 3, cy - 3, 6, 1, 0x00888888);
        draw_rect(cx - 3, cy + 1, 6, 1, 0x00888888);
    } else if (strncmp(title, "Media", 5) == 0 || strncmp(title, "Mplayer", 7) == 0) {
        draw_line(cx - size/4, cy - size/4, cx - size/4, cy + size/4, 0x00FFFFFF);
        draw_line(cx - size/4, cy - size/4, cx + size/4, cy, 0x00FFFFFF);
        draw_line(cx - size/4, cy + size/4, cx + size/4, cy, 0x00FFFFFF);
    } else {
        char letter[2];
        letter[0] = title[0];
        letter[1] = '\0';
        uint32_t text_col = (bg_col == 0x00FFFFFF || bg_col == 0x00E2E8F0) ? 0x0011111B : 0x00FFFFFF;
        draw_string_px(ix + 8, iy + 5, letter, text_col, 0xFFFFFFFF);
    }
}

static void wm_draw_alt_tab_hud(void) {
    if (!alt_tab_active || wm_zcount <= 1) return;

    // Clamp selection index if z-count changed (e.g. windows closed)
    if (alt_tab_selected_idx >= wm_zcount) {
        alt_tab_selected_idx = wm_zcount - 1;
    }
    if (alt_tab_selected_idx < 0) {
        alt_tab_selected_idx = 0;
    }

    int item_w = 56;
    int gap = 14;
    int padding = 20;
    
    int hud_w = padding * 2 + wm_zcount * item_w + (wm_zcount - 1) * gap;
    if (hud_w < 180) hud_w = 180;
    int hud_h = 96;
    
    int hx = ((int)fb_width - hud_w) / 2;
    int hy = ((int)fb_height - hud_h) / 2;
    
    // Background card (Catppuccin dark look)
    draw_rounded_rect(hx, hy, hud_w, hud_h, 12, 0x00181825);
    draw_rounded_rect_border(hx, hy, hud_w, hud_h, 12, 0x00313244);
    
    // Draw each visible window in switcher
    for (int i = 0; i < wm_zcount; i++) {
        int idx = wm_zorder[wm_zcount - 1 - i];
        int item_x = hx + padding + i * (item_w + gap);
        int item_y = hy + 16;
        
        int selected = (i == alt_tab_selected_idx);
        
        // Highlight box
        if (selected) {
            draw_rounded_rect(item_x - 4, item_y - 4, item_w + 8, item_w + 8, 8, 0x00313244);
            draw_rounded_rect_border(item_x - 4, item_y - 4, item_w + 8, item_w + 8, 8, 0x0089B4FA);
        }
        
        // Icon
        int icon_x = item_x + (item_w - 24) / 2;
        int icon_y = item_y + 6;
        draw_hud_icon(icon_x, icon_y, wm_wins[idx].title);
        
        // Short text label
        char short_title[6];
        int j = 0;
        for (; j < 4 && wm_wins[idx].title[j]; j++) {
            short_title[j] = wm_wins[idx].title[j];
        }
        if (strlen(wm_wins[idx].title) > 4) {
            short_title[j++] = '.';
        }
        short_title[j] = '\0';
        
        uint32_t text_col = selected ? 0x0089B4FA : 0x00A6ADC8;
        draw_string_px(item_x + (item_w - j*8)/2, item_y + 36, short_title, text_col, 0xFFFFFFFF);
    }
    
    // Selected window name at the bottom of the HUD card
    int active_idx = wm_zorder[wm_zcount - 1 - alt_tab_selected_idx];
    const char* full_title = wm_wins[active_idx].title;
    int title_len = strlen(full_title);
    draw_string_px(hx + (hud_w - title_len*8)/2, hy + 76, full_title, 0x00F5C2E7, 0xFFFFFFFF);
}

// ---- Open / Close ----
int wm_open(int x, int y, int w, int h, const char* title,
            WinDrawFn draw_fn, WinKeyFn key_fn, WinTickFn tick_fn, WinMouseFn mouse_fn) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wm_wins[i].visible) {
            wm_wins[i].id        = next_id++;
            wm_wins[i].x         = x; wm_wins[i].y = y;
            wm_wins[i].w         = w; wm_wins[i].h = h;
            wm_wins[i].draw_fn   = draw_fn;
            wm_wins[i].key_fn    = key_fn;
            wm_wins[i].tick_fn   = tick_fn;
            wm_wins[i].mouse_fn  = mouse_fn;
            wm_wins[i].owner_ring = 0; // Default to kernel
            wm_wins[i].owner_task = -1; // Set by syscall for Ring 3 apps
            wm_wins[i].visible   = 1;
            wm_wins[i].dragging  = 0;
            wm_wins[i].resizing  = 0;
            wm_wins[i].minimized = 0;
            wm_wins[i].maximized = 0;
            wm_wins[i].snap_state = SNAP_NONE;
            wm_wins[i].content_buffer = NULL;
            wm_wins[i].buffer_dirty = 1;
            wm_wins[i].last_cw = 0;
            wm_wins[i].last_ch = 0;
            int k = 0;
            while (title[k] && k < 47) { wm_wins[i].title[k] = title[k]; k++; }
            wm_wins[i].title[k] = '\0';
            if (wm_zcount < MAX_WINDOWS) wm_zorder[wm_zcount++] = i;
            wm_focused = wm_wins[i].id;

            write_serial_string("WM_OPEN id=");
            write_serial_hex(wm_wins[i].id);
            write_serial_string(" &visible=");
            write_serial_hex((uint32_t)&wm_wins[i].visible);
            write_serial_string(" &id=");
            write_serial_hex((uint32_t)&wm_wins[i].id);
            write_serial('\n');

            extern volatile int needs_redraw;
            needs_redraw = 1;
            return wm_wins[i].id;
        }
    }
    return -1; // no slot
}

int wm_is_open(int id) {
    if (id < 0) return 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm_wins[i].visible && wm_wins[i].id == id) return 1;
    }
    return 0;
}

void wm_invalidate(int id) {
    if (id < 0) return;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm_wins[i].visible && wm_wins[i].id == id) {
            wm_wins[i].buffer_dirty = 1;
            return;
        }
    }
}

void wm_close(int id) {
    write_serial_string("WM_CLOSE called for id=");
    write_serial_hex(id);
    write_serial('\n');
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm_wins[i].visible && wm_wins[i].id == id) {
            extern void mark_dirty(int, int, int, int);
            mark_dirty(wm_wins[i].x, wm_wins[i].y, wm_wins[i].w, wm_wins[i].h); // Mark closed window area dirty
            // If owned by a user task, kill the task.
            // task_kill will automatically call wm_cleanup_task,
            // which will hide the window and remove it from z-order.
            if (wm_wins[i].owner_task > 0) {
                extern int task_kill(int tid);
                write_serial_string("Killing owner task: ");
                write_serial_hex(wm_wins[i].owner_task);
                write_serial('\n');
                task_kill(wm_wins[i].owner_task);
            } else {
                // Kernel window fallback
                wm_wins[i].visible = 0;
                if (wm_wins[i].content_buffer) {
                    extern void kfree(void*);
                    kfree(wm_wins[i].content_buffer);
                    wm_wins[i].content_buffer = NULL;
                }
                z_remove(i);
                wm_focused = (wm_zcount > 0) ? wm_wins[wm_zorder[wm_zcount-1]].id : -1;
            }
            extern volatile int needs_redraw;
            needs_redraw = 1;
            return;
        }
    }
}

// ---- Aero Snap logic ----
static void check_snap(int idx) {
    WmWin* w = &wm_wins[idx];
    if (w->maximized) return; // already fullscreen

    int old_snap = w->snap_state;
    int old_maximized = w->maximized;

    // Distance from edges
    int dist_left   = w->x;
    int dist_right  = (int)fb_width - (w->x + w->w);
    int dist_top    = w->y;

    // Save restore position BEFORE snap
    w->saved_x = w->x;
    w->saved_y = w->y;
    w->saved_w = w->w;
    w->saved_h = w->h;

    if (dist_top <= SNAP_THRESHOLD) {
        // Snap to top = maximize
        w->x = 0;
        w->y = 0;
        w->w = (int)fb_width;
        w->h = (int)fb_height - (int)TASKBAR_H_PX;
        w->maximized = 1;
        w->snap_state = SNAP_TOP;
    } else if (dist_left <= SNAP_THRESHOLD) {
        // Snap left: half screen left
        w->x = 0;
        w->y = 0;
        w->w = SNAP_AREA_W;
        w->h = (int)fb_height - (int)TASKBAR_H_PX;
        w->snap_state = SNAP_LEFT;
    } else if (dist_right <= SNAP_THRESHOLD) {
        // Snap right: half screen right
        w->x = (int)fb_width - SNAP_AREA_W;
        w->y = 0;
        w->w = SNAP_AREA_W;
        w->h = (int)fb_height - (int)TASKBAR_H_PX;
        w->snap_state = SNAP_RIGHT;
    } else {
        // No snap; restore if previously snapped via drag
        if (w->snap_state != SNAP_NONE) {
            // Only restore if user dragged away from snap region
            w->x = w->saved_x;
            w->y = w->saved_y;
            w->w = w->saved_w;
            w->h = w->saved_h;
            w->snap_state = SNAP_NONE;
        }
    }
    if (w->snap_state != old_snap || w->maximized != old_maximized) {
        extern void mark_dirty(int, int, int, int);
        mark_dirty(0, 0, fb_width, fb_height);
    }
}

static void draw_one(int idx) {
    WmWin* w = &wm_wins[idx];
    if (!w->visible || w->minimized) return;

    if (w->buffer_dirty) {
        extern void mark_dirty(int, int, int, int);
        mark_dirty(w->x, w->y, w->w, w->h);
    }
    
    extern int d_min_x, d_min_y, d_max_x, d_max_y;
    // Check if the window overlaps the active dirty rectangle OR if it has a dirty buffer.
    // If not, we don't need to redraw it because it is already correct in the back buffer!
    if (!w->buffer_dirty) {
        if (w->x + w->w <= d_min_x || w->x >= d_max_x || w->y + w->h <= d_min_y || w->y >= d_max_y) {
            return;
        }
    }

    int x = w->x, y = w->y, ww = w->w, wh = w->h;
    int focused = (wm_focused == w->id);
    int use_radius = (w->snap_state != SNAP_NONE || w->maximized) ? 4 : WIN_RADIUS;

    // ========== Window Body (rounded rect) ==========
    uint32_t body_color = focused ? GUI_BG : GUI_BORDER2;
    draw_rounded_rect(x, y + TITLEBAR_H, ww, wh - TITLEBAR_H, use_radius, body_color);

    // ========== Titlebar (rounded top corners + gradient) ==========
    if (focused) {
        draw_gradient_v(x, y, ww, TITLEBAR_H, GUI_TITLE_A, GUI_TITLE_B);
        // Subtle top highlight for depth
        draw_rect(x + use_radius, y, ww - 2 * use_radius, 1, 0x55FFFFFF);
    } else {
        draw_rounded_rect(x, y, ww, TITLEBAR_H, use_radius, GUI_TITLE_I);
    }
    // Bottom separator
    draw_rect(x, y + TITLEBAR_H - 1, ww, 1, 0x0011111B);

    // ========== Window border (rounded) ==========
    if (focused) {
        draw_rounded_rect_border(x, y, ww, wh, use_radius, GUI_BORDER);
    } else {
        draw_rounded_rect_border(x, y, ww, wh, use_radius, GUI_BORDER2);
    }

    // ========== Title text (Centered) ==========
    int tlen = strlen(w->title);
    int tx = x + (ww - tlen * 8) / 2;
    int tty = y + (TITLEBAR_H - 16) / 2;
    draw_string_px(tx, tty, w->title, GUI_TEXT, 0xFFFFFFFF);

    // ========== Titlebar buttons (left side): macOS style traffic lights ==========
    int btn_r = 6;           // circle radius
    int btn_y = y + TITLEBAR_H / 2;
    int btn_start_x = x + 12;

    int is_power_dialog = (strcmp(w->title, "Power Options") == 0);

    // Close button (Red)
    fill_circle(btn_start_x, btn_y, btn_r, focused ? GUI_CLOSE : GUI_DIM);
    if (focused) {
        draw_line(btn_start_x - 2, btn_y - 2, btn_start_x + 2, btn_y + 2, 0x00500000);
        draw_line(btn_start_x - 2, btn_y + 2, btn_start_x + 2, btn_y - 2, 0x00500000);
    }
    w->close_cx = btn_start_x; w->close_cy = btn_y; w->close_r = btn_r;

    if (!is_power_dialog) {
        // Minimize button (Yellow)
        int min_cx = btn_start_x + 18;
        fill_circle(min_cx, btn_y, btn_r, focused ? GUI_YELLOW : GUI_DIM);
        if (focused) {
            draw_rect(min_cx - 2, btn_y, 5, 1, 0x00593B00);
        }
        w->min_cx = min_cx; w->min_cy = btn_y; w->min_r = btn_r;

        // Maximize button (Green)
        int m_cx = min_cx + 18;
        fill_circle(m_cx, btn_y, btn_r, focused ? GUI_GREEN : GUI_DIM);
        if (focused) {
            draw_rect(m_cx - 2, btn_y, 5, 1, 0x00004000);
            draw_rect(m_cx, btn_y - 2, 1, 5, 0x00004000);
        }
        w->max_cx = m_cx; w->max_cy = btn_y; w->max_r = btn_r;
    } else {
        w->min_cx = -1; w->min_cy = -1; w->min_r = 0;
        w->max_cx = -1; w->max_cy = -1; w->max_r = 0;
    }

    // ========== Content area ==========
    // Subtle inner border
    draw_rect(x + 1, y + TITLEBAR_H + use_radius - 2, ww - 2, 1, 0x00252535);

    int cx2 = x + 1;
    int cy2 = y + TITLEBAR_H + 1;
    int cw2 = ww - 2;
    int ch2 = wh - TITLEBAR_H - 2;

    if (cw2 > 0 && ch2 > 0) {
        // 1. Dynamic allocation of content buffer if size changed or not allocated
        if (w->content_buffer == NULL || w->last_cw != cw2 || w->last_ch != ch2) {
            extern void* kmalloc(uint32_t);
            extern void kfree(void*);
            if (w->content_buffer) kfree(w->content_buffer);
            w->content_buffer = kmalloc(cw2 * ch2 * 4);
            w->last_cw = cw2;
            w->last_ch = ch2;
            w->buffer_dirty = 1;
        }

        // 2. Render to off-screen buffer if dirty
        if (w->content_buffer && w->buffer_dirty) {
            vga_set_render_target(w->content_buffer, cw2, ch2, cw2 * 4);
            vga_set_clip(0, 0, cw2, ch2);
            
            // Clear buffer to prevent uninitialized memory artifacts (skew/tearing) on resize
            draw_rect(0, 0, cw2, ch2, 0x001E1E2E); // Default dark theme background
            
            // Call app draw_fn with (0,0) as origin!
            if (w->draw_fn) {
                w->draw_fn(w->id, 0, 0, cw2, ch2);
            }
            
            vga_reset_clip();
            vga_set_render_target(NULL, 0, 0, 0); // Restore back buffer
            
            w->buffer_dirty = 0; // Clear dirty flag
        }

        // 3. Composite (blit) the off-screen buffer to the global back buffer
        if (w->content_buffer) {
            vga_blit_buffer(w->content_buffer, cw2, ch2, cx2, cy2);
        }
    }
}

void wm_draw_all() {
    for (int z = 0; z < wm_zcount; z++) draw_one(wm_zorder[z]);
    wm_draw_alt_tab_hud();
}

// ---- Mouse handling ----
int wm_handle_mouse(int mx, int my, int btn, int pbtn) {
    int click = btn && !pbtn;   // rising edge
    int release = !btn && pbtn; // falling edge

    // Handle dragging/resizing
    if (btn & 1) {
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (wm_wins[i].visible) {
                // Resize
                if (wm_wins[i].resizing) {
                    extern void mark_dirty(int, int, int, int);
                    mark_dirty(wm_wins[i].x, wm_wins[i].y, wm_wins[i].w, wm_wins[i].h); // Old bounds dirty
                    
                    int dx = mx - wm_wins[i].resize_mx;
                    int dy = my - wm_wins[i].resize_my;
                    
                    int new_x = wm_wins[i].drag_wx;
                    int new_y = wm_wins[i].drag_wy;
                    int new_w = wm_wins[i].resize_ww;
                    int new_h = wm_wins[i].resize_wh;

                    if (wm_wins[i].resize_edge & 8) new_w = wm_wins[i].resize_ww + dx;
                    if (wm_wins[i].resize_edge & 2) new_h = wm_wins[i].resize_wh + dy;
                    if (wm_wins[i].resize_edge & 4) {
                        new_x = wm_wins[i].drag_wx + dx;
                        new_w = wm_wins[i].resize_ww - dx;
                    }
                    if (wm_wins[i].resize_edge & 1) {
                        new_y = wm_wins[i].drag_wy + dy;
                        new_h = wm_wins[i].resize_wh - dy;
                    }

                    if (new_w < 220) {
                        new_w = 220;
                        if (wm_wins[i].resize_edge & 4) new_x = wm_wins[i].drag_wx + (wm_wins[i].resize_ww - 220);
                    }
                    if (new_h < 150) {
                        new_h = 150;
                        if (wm_wins[i].resize_edge & 1) new_y = wm_wins[i].drag_wy + (wm_wins[i].resize_wh - 150);
                    }

                    wm_wins[i].x = new_x;
                    wm_wins[i].y = new_y;
                    wm_wins[i].w = new_w;
                    wm_wins[i].h = new_h;
                    
                    mark_dirty(wm_wins[i].x, wm_wins[i].y, wm_wins[i].w, wm_wins[i].h); // New bounds dirty
                    extern volatile int needs_redraw;
                    needs_redraw = 1;
                    return 1;
                }
                // Drag
                if (wm_wins[i].dragging) {
                    extern void mark_dirty(int, int, int, int);
                    mark_dirty(wm_wins[i].x, wm_wins[i].y, wm_wins[i].w, wm_wins[i].h); // Old bounds dirty
                    
                    wm_wins[i].x = wm_wins[i].drag_wx + (mx - wm_wins[i].drag_mx);
                    wm_wins[i].y = wm_wins[i].drag_wy + (my - wm_wins[i].drag_my);

                    // ----- Aero Snap check during drag -----
                    check_snap(i);

                    // Clamp (only if not snapped)
                    if (wm_wins[i].snap_state == SNAP_NONE && !wm_wins[i].maximized) {
                        if (wm_wins[i].x < 0) wm_wins[i].x = 0;
                        if (wm_wins[i].y < 0) wm_wins[i].y = 0;
                        if (wm_wins[i].x + wm_wins[i].w > (int)fb_width)
                            wm_wins[i].x = (int)fb_width - wm_wins[i].w;
                    }
                    
                    mark_dirty(wm_wins[i].x, wm_wins[i].y, wm_wins[i].w, wm_wins[i].h); // New bounds dirty
                    extern volatile int needs_redraw;
                    needs_redraw = 1;
                    return 1;
                }
            }
        }
    }

    // ---- Release: finalize snap / restore / resizing ----
    if (release) {
        int handled_release = 0;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (wm_wins[i].visible && wm_wins[i].id == wm_focused && !wm_wins[i].minimized) {
                WmWin* w = &wm_wins[i];
                if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) {
                    if (w->mouse_fn && my >= w->y + TITLEBAR_H) {
                        w->mouse_fn(w->id, mx - w->x, my - (w->y + TITLEBAR_H), 0);
                        handled_release = 1;
                    }
                }
                break;
            }
        }
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (wm_wins[i].visible) {
                wm_wins[i].dragging = 0;
                wm_wins[i].resizing = 0;
                wm_wins[i].resize_edge = 0;
            }
        }
        extern volatile int needs_redraw;
        needs_redraw = 1;
        if (handled_release) return 1;
    }

    if (!click) return 0;

    // Hit test front-to-back
    for (int z = wm_zcount - 1; z >= 0; z--) {
        int idx = wm_zorder[z];
        WmWin* w = &wm_wins[idx];
        if (!w->visible) continue;
        if (w->minimized) continue;
        if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) {
            wm_raise(w->id);

            // 1. Titlebar buttons & dragging (highest priority so corners don't overlap)
            if (my < w->y + TITLEBAR_H) {
                // ---- Circle button hit test ----
                int dx, dy, dist2;

                // Close button
                dx = mx - w->close_cx; dy = my - w->close_cy;
                dist2 = dx*dx + dy*dy;
                if (dist2 <= w->close_r * w->close_r + 4) {
                    write_serial_string("WM_CLOSE via MOUSE!\n");
                    wm_close(w->id);
                    return 1;
                }

                // Maximize button
                dx = mx - w->max_cx; dy = my - w->max_cy;
                dist2 = dx*dx + dy*dy;
                if (dist2 <= w->max_r * w->max_r + 4) {
                    extern void mark_dirty(int, int, int, int);
                    mark_dirty(0, 0, fb_width, fb_height); // Mark fullscreen dirty for state transition
                    if (w->maximized || w->snap_state != SNAP_NONE) {
                        // Restore from snap/maximize
                        w->x = w->saved_x; w->y = w->saved_y;
                        w->w = w->saved_w; w->h = w->saved_h;
                        w->maximized = 0;
                        w->snap_state = SNAP_NONE;
                    } else {
                        w->saved_x = w->x; w->saved_y = w->y;
                        w->saved_w = w->w; w->saved_h = w->h;
                        w->x = 0; w->y = 0;
                        w->w = (int)fb_width;
                        w->h = (int)fb_height - (int)TASKBAR_H_PX;
                        w->maximized = 1;
                    }
                    mark_dirty(0, 0, fb_width, fb_height);
                    extern volatile int needs_redraw;
                    needs_redraw = 1;
                    return 1;
                }

                // Minimize button
                dx = mx - w->min_cx; dy = my - w->min_cy;
                dist2 = dx*dx + dy*dy;
                if (dist2 <= w->min_r * w->min_r + 4) {
                    wm_minimize(w->id);
                    return 1;
                }

                // Titlebar drag
                w->dragging = 1;
                w->drag_mx = mx; w->drag_my = my;
                w->drag_wx = w->x; w->drag_wy = w->y;
                extern volatile int needs_redraw;
                needs_redraw = 1;
                return 1;
            }

            // 2. Resize handle check (edges 8px)
            int edge_w = 8;
            int edge = 0;
            if (mx < w->x + edge_w) edge |= 4;         // Left
            else if (mx >= w->x + w->w - edge_w) edge |= 8; // Right
            if (my < w->y + edge_w) edge |= 1;         // Top
            else if (my >= w->y + w->h - edge_w) edge |= 2; // Bottom

            if (edge && w->snap_state == SNAP_NONE && !w->maximized) {
                w->resizing = 1;
                w->resize_edge = edge;
                w->resize_mx = mx; w->resize_my = my;
                w->drag_wx = w->x; w->drag_wy = w->y;
                w->resize_ww = w->w; w->resize_wh = w->h;
                extern volatile int needs_redraw;
                needs_redraw = 1;
                return 1;
            }

            // 3. Content area click
            if (w->mouse_fn) {
                w->mouse_fn(w->id, mx - w->x, my - (w->y + TITLEBAR_H), btn);
            }
            extern volatile int needs_redraw;
            needs_redraw = 1;
            return 1;
        }
    }
    return 0;
}

// ---- Scroll wheel handling ----
void wm_handle_scroll(int mx, int my, int delta) {
    // Hit test front-to-back: find the window under the cursor
    for (int z = wm_zcount - 1; z >= 0; z--) {
        int idx = wm_zorder[z];
        WmWin* w = &wm_wins[idx];
        if (!w->visible) continue;
        if (w->minimized) continue;
        if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) {
            if (w->mouse_fn) {
                // Encode scroll as special btn values:
                // 0x10 = scroll up, 0x20 = scroll down
                int scroll_btn = (delta > 0) ? 0x10 : 0x20;
                w->mouse_fn(w->id, mx - w->x, my - (w->y + TITLEBAR_H), scroll_btn);
            }
            return;
        }
    }
}

void wm_handle_key(char c, uint8_t sc) {
    if (wm_focused < 0) return;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm_wins[i].visible && wm_wins[i].id == wm_focused && wm_wins[i].key_fn)
            wm_wins[i].key_fn(wm_wins[i].id, c, sc);
    }
}

void wm_tick_all() {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wm_wins[i].visible && wm_wins[i].tick_fn)
            wm_wins[i].tick_fn(wm_wins[i].id);
}

// Close all windows owned by a specific task (crash recovery)
void wm_cleanup_task(int tid) {
    extern volatile int doom_fullscreen;
    doom_fullscreen = 0; // Restore normal rendering if DOOM task exits

    write_serial_string("[WM] cleanup_task tid=");
    write_serial_hex(tid);
    write_serial('\n');
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm_wins[i].visible && wm_wins[i].owner_task == tid) {
            write_serial_string("[WM] closing zombie window: ");
            write_serial_string(wm_wins[i].title);
            write_serial('\n');
            // Notify terminal if this is the terminal window
            extern void on_terminal_close(void);
            extern int term_open;
            extern int get_term_win_id(void);
            if (term_open && wm_wins[i].id == get_term_win_id()) {
                on_terminal_close();
            }
            
            extern void mark_dirty(int, int, int, int);
            mark_dirty(wm_wins[i].x, wm_wins[i].y, wm_wins[i].w, wm_wins[i].h); // Mark bounds dirty before hiding
            wm_wins[i].visible = 0;
            if (wm_wins[i].content_buffer) {
                extern void kfree(void*);
                kfree(wm_wins[i].content_buffer);
                wm_wins[i].content_buffer = NULL;
            }
            z_remove(i);
        }
    }
    // Update focus
    wm_focused = (wm_zcount > 0) ? wm_wins[wm_zorder[wm_zcount-1]].id : -1;
    extern volatile int needs_redraw;
    needs_redraw = 1;
}

void wm_reset_session(void) {
    // First close windows owned by user tasks so their lifecycle cleanup runs
    // through the same path used by task_exit()/task_kill().
    int cleaned_tasks[MAX_WINDOWS];
    int cleaned_count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wm_wins[i].visible || wm_wins[i].owner_task <= 0) continue;
        int seen = 0;
        for (int j = 0; j < cleaned_count; j++) {
            if (cleaned_tasks[j] == wm_wins[i].owner_task) {
                seen = 1;
                break;
            }
        }
        if (!seen && cleaned_count < MAX_WINDOWS) {
            cleaned_tasks[cleaned_count++] = wm_wins[i].owner_task;
            wm_cleanup_task(wm_wins[i].owner_task);
        }
    }

    // Then clear any remaining kernel-owned windows directly.
    extern void mark_dirty(int, int, int, int);
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wm_wins[i].visible) continue;
        mark_dirty(wm_wins[i].x, wm_wins[i].y, wm_wins[i].w, wm_wins[i].h);
        wm_wins[i].visible = 0;
        wm_wins[i].dragging = 0;
        wm_wins[i].resizing = 0;
        wm_wins[i].minimized = 0;
        wm_wins[i].maximized = 0;
        wm_wins[i].snap_state = SNAP_NONE;
        wm_wins[i].owner_task = -1;
        if (wm_wins[i].content_buffer) {
            extern void kfree(void*);
            kfree(wm_wins[i].content_buffer);
            wm_wins[i].content_buffer = NULL;
        }
    }

    wm_focused = -1;
    wm_zcount = 0;
    alt_tab_active = 0;
    alt_tab_selected_idx = 0;

    extern volatile int needs_redraw;
    needs_redraw = 1;
}