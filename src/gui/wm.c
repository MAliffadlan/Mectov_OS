#include "../include/wm.h"
#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/taskbar.h"

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
            return;
        }
    }
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
            wm_wins[i].visible   = 1;
            wm_wins[i].dragging  = 0;
            wm_wins[i].resizing  = 0;
            wm_wins[i].minimized = 0;
            wm_wins[i].maximized = 0;
            wm_wins[i].snap_state = SNAP_NONE;
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

void wm_close(int id) {
    write_serial_string("WM_CLOSE called from: ");
    write_serial_hex((uint32_t)__builtin_return_address(0));
    write_serial_string(" for id=");
    write_serial_hex(id);
    write_serial('\n');
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm_wins[i].visible && wm_wins[i].id == id) {
            // Notify terminal if this is the terminal window
            extern void on_terminal_close(void);
            extern int term_open;
            extern int get_term_win_id(void);
            if (term_open && id == get_term_win_id()) {
                on_terminal_close();
            }
            
            wm_wins[i].visible = 0;
            z_remove(i);
            wm_focused = (wm_zcount > 0) ? wm_wins[wm_zorder[wm_zcount-1]].id : -1;
            return;
        }
    }
}

// ---- Aero Snap logic ----
static void check_snap(int idx) {
    WmWin* w = &wm_wins[idx];
    if (w->maximized) return; // already fullscreen

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
}

// ---- Draw a single window ----
static void draw_one(int idx) {
    WmWin* w = &wm_wins[idx];
    if (!w->visible || w->minimized) return;
    int x = w->x, y = w->y, ww = w->w, wh = w->h;
    int focused = (wm_focused == w->id);
    int radius = WIN_RADIUS;

    // Skip border radius if snapped to edge (clean, modern look)
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

    // Call app draw_fn with content area coords
    if (w->draw_fn) {
        int cx2 = x + 1;
        int cy2 = y + TITLEBAR_H + 1;
        int cw2 = ww - 2;
        int ch2 = wh - TITLEBAR_H - 2;
        vga_set_clip(cx2, cy2, cw2, ch2);
        w->draw_fn(w->id, cx2, cy2, cw2, ch2);
        vga_reset_clip();
    }
}

void wm_draw_all() {
    for (int z = 0; z < wm_zcount; z++) draw_one(wm_zorder[z]);
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
                    return 1;
                }
                // Drag
                if (wm_wins[i].dragging) {
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
                    return 1;
                }
            }
        }
    }

    // ---- Release: finalize snap / restore / resizing ----
    if (release) {
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (wm_wins[i].visible) {
                wm_wins[i].dragging = 0;
                wm_wins[i].resizing = 0;
                wm_wins[i].resize_edge = 0;
            }
        }
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
                    return 1;
                }

                // Minimize button
                dx = mx - w->min_cx; dy = my - w->min_cy;
                dist2 = dx*dx + dy*dy;
                if (dist2 <= w->min_r * w->min_r + 4) {
                    w->minimized = 1;
                    wm_focused = -1;
                    for (int zz = wm_zcount - 1; zz >= 0; zz--) {
                        WmWin* nw = &wm_wins[wm_zorder[zz]];
                        if (nw->visible && !nw->minimized) {
                            wm_focused = nw->id;
                            break;
                        }
                    }
                    return 1;
                }

                // Titlebar drag
                w->dragging = 1;
                w->drag_mx = mx; w->drag_my = my;
                w->drag_wx = w->x; w->drag_wy = w->y;
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
                return 1;
            }

            // 3. Content area click
            if (w->mouse_fn) {
                w->mouse_fn(w->id, mx - w->x, my - (w->y + TITLEBAR_H), btn);
            }
            return 1;
        }
    }
    return 0;
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