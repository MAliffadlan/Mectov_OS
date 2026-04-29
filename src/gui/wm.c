#include "../include/wm.h"
#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/taskbar.h"

WmWin wm_wins[MAX_WINDOWS];
int   wm_focused = -1;
int   wm_zorder[MAX_WINDOWS];
int   wm_zcount = 0;
static int next_id = 1;

void wm_init() {
    for (int i = 0; i < MAX_WINDOWS; i++) { wm_wins[i].visible = 0; wm_wins[i].id = 0; }
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
            wm_wins[i].id       = next_id++;
            wm_wins[i].x       = x; wm_wins[i].y = y;
            wm_wins[i].w       = w; wm_wins[i].h = h;
            wm_wins[i].draw_fn = draw_fn;
            wm_wins[i].key_fn  = key_fn;
            wm_wins[i].tick_fn = tick_fn;
            wm_wins[i].mouse_fn= mouse_fn;
            wm_wins[i].visible = 1;
            wm_wins[i].dragging= 0;
            wm_wins[i].minimized = 0;
            wm_wins[i].maximized = 0;
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
            wm_wins[i].visible = 0;
            z_remove(i);
            wm_focused = (wm_zcount > 0) ? wm_wins[wm_zorder[wm_zcount-1]].id : -1;
            return;
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

    // ========== Soft Drop Shadow ==========
    if (!w->maximized) {
        draw_soft_shadow(x, y, ww, wh, 6, 255);
    }

    // ========== Window Body (rounded rect) ==========
    uint32_t body_color = focused ? GUI_BG : GUI_BORDER2;
    draw_rounded_rect(x, y + TITLEBAR_H, ww, wh - TITLEBAR_H, radius, body_color);

    // ========== Titlebar (rounded top corners + gradient) ==========
    if (focused) {
        draw_gradient_v(x, y, ww, TITLEBAR_H, GUI_TITLE_A, GUI_TITLE_B);
    } else {
        draw_rounded_rect(x, y, ww, TITLEBAR_H, radius, GUI_TITLE_I);
    }

    // Round the top-left corner of body so it merges with titlebar
    // (titlebar covers upper portion; body gets the bottom corners only)
    // The body already has full rounded rect, so the top corners of body
    // will be behind the titlebar. That's fine.

    // ========== Window border (rounded) ==========
    if (focused) {
        draw_rounded_rect_border(x, y, ww, wh, radius, GUI_BORDER);
    } else {
        draw_rounded_rect_border(x, y, ww, wh, radius, GUI_BORDER2);
    }

    // ========== Title text ==========
    int tlen = strlen(w->title);
    int btn_spacing = 14; // space reserved for 3 small circle buttons + padding
    int tx = x + 8;
    int tty = y + (TITLEBAR_H - 16) / 2;
    draw_string_px(tx, tty, w->title, GUI_TEXT, focused ? GUI_TITLE_A : GUI_TITLE_I);

    // ========== Titlebar buttons (right side): small circles ==========
    int btn_r = 5;           // circle radius
    int btn_y = y + TITLEBAR_H / 2;

    // Close button (rightmost)
    int c_cx = x + ww - (btn_r + 8);
    fill_circle(c_cx, btn_y, btn_r, focused ? GUI_CLOSE : GUI_DIM);
    // X mark
    draw_string_px(c_cx - 3, btn_y - 4, "x", GUI_WHITE, focused ? GUI_CLOSE : GUI_DIM);
    w->close_cx = c_cx; w->close_cy = btn_y; w->close_r = btn_r;

    // Maximize button
    int m_cx = c_cx - (btn_r * 2 + 6);
    fill_circle(m_cx, btn_y, btn_r, focused ? GUI_GREEN : GUI_DIM);
    draw_string_px(m_cx - 3, btn_y - 4, "O", GUI_WHITE, focused ? GUI_GREEN : GUI_DIM);
    w->max_cx = m_cx; w->max_cy = btn_y; w->max_r = btn_r;

    // Minimize button
    int min_cx = m_cx - (btn_r * 2 + 6);
    fill_circle(min_cx, btn_y, btn_r, focused ? GUI_YELLOW : GUI_DIM);
    draw_string_px(min_cx - 3, btn_y - 4, "_", GUI_WHITE, focused ? GUI_YELLOW : GUI_DIM);
    w->min_cx = min_cx; w->min_cy = btn_y; w->min_r = btn_r;

    // ========== Content area ==========
    // Subtle inner border
    draw_rect(x + 1, y + TITLEBAR_H + radius - 2, ww - 2, 1, 0x00252535);

    // Call app draw_fn with content area coords
    if (w->draw_fn) {
        int cx2 = x + 1;
        int cy2 = y + TITLEBAR_H + 1;
        int cw2 = ww - 2;
        int ch2 = wh - TITLEBAR_H - 2;
        w->draw_fn(w->id, cx2, cy2, cw2, ch2);
    }
}

void wm_draw_all() {
    for (int z = 0; z < wm_zcount; z++) draw_one(wm_zorder[z]);
}

// ---- Mouse handling ----
int wm_handle_mouse(int mx, int my, int btn, int pbtn) {
    int click = btn && !pbtn;   // rising edge
    int release = !btn && pbtn; // falling edge

    // Handle dragging (check before hit test so drag continues across windows)
    if (btn & 1) {
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (wm_wins[i].visible && wm_wins[i].dragging) {
                wm_wins[i].x = wm_wins[i].drag_wx + (mx - wm_wins[i].drag_mx);
                wm_wins[i].y = wm_wins[i].drag_wy + (my - wm_wins[i].drag_my);
                // Clamp
                if (wm_wins[i].x < 0) wm_wins[i].x = 0;
                if (wm_wins[i].y < 0) wm_wins[i].y = 0;
                if (wm_wins[i].x + wm_wins[i].w > (int)fb_width)
                    wm_wins[i].x = (int)fb_width - wm_wins[i].w;
                return 1;
            }
        }
    }
    if (release) {
        for (int i = 0; i < MAX_WINDOWS; i++) wm_wins[i].dragging = 0;
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
            if (my < w->y + TITLEBAR_H) {
                // ---- Circle button hit test ----
                int dx, dy, dist2;

                // Close button (rightmost circle)
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
                    if (w->maximized) {
                        w->x = w->saved_x; w->y = w->saved_y;
                        w->w = w->saved_w; w->h = w->saved_h;
                        w->maximized = 0;
                    } else {
                        w->saved_x = w->x; w->saved_y = w->y;
                        w->saved_w = w->w; w->saved_h = w->h;
                        w->x = 0; w->y = 0;
                        w->w = (int)fb_width;
                        w->h = (int)fb_height - TASKBAR_H_PX;
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
                // Titlebar drag (only if not maximized)
                if (!w->maximized) {
                    w->dragging = 1;
                    w->drag_mx = mx; w->drag_my = my;
                    w->drag_wx = w->x; w->drag_wy = w->y;
                }
                return 1;
            }
            // Content area click
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
