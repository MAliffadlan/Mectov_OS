#include "../include/wm.h"
#include "../include/vga.h"
#include "../include/utils.h"

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
            WinDrawFn draw_fn, WinKeyFn key_fn, WinTickFn tick_fn) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wm_wins[i].visible) {
            wm_wins[i].id       = next_id++;
            wm_wins[i].x       = x; wm_wins[i].y = y;
            wm_wins[i].w       = w; wm_wins[i].h = h;
            wm_wins[i].draw_fn = draw_fn;
            wm_wins[i].key_fn  = key_fn;
            wm_wins[i].tick_fn = tick_fn;
            wm_wins[i].visible = 1;
            wm_wins[i].dragging= 0;
            int k = 0;
            while (title[k] && k < 47) { wm_wins[i].title[k] = title[k]; k++; }
            wm_wins[i].title[k] = '\0';
            if (wm_zcount < MAX_WINDOWS) wm_zorder[wm_zcount++] = i;
            wm_focused = wm_wins[i].id;
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
    if (!w->visible) return;
    int x = w->x, y = w->y, ww = w->w, wh = w->h;
    int focused = (wm_focused == w->id);

    // Drop shadow
    draw_rect(x + 6, y + 6, ww, wh, 0x00000000);

    // Outer glow
    uint32_t border = focused ? GUI_BORDER : GUI_BORDER2;
    draw_rect(x - 1, y - 1, ww + 2, wh + 2, border);

    // Titlebar gradient (Mint gray)
    for (int row = 0; row < TITLEBAR_H; row++) {
        uint32_t t = (uint32_t)(row * 255) / TITLEBAR_H;
        uint32_t r, g, b;
        if (focused) {
            r = 217 + (t * 10) / 255; g = 217 + (t * 10) / 255; b = 217 + (t * 10) / 255;
        } else {
            r = 230 - (t * 10) / 255; g = 230 - (t * 10) / 255; b = 230 - (t * 10) / 255;
        }
        draw_rect(x, y + row, ww, 1, (r << 16) | (g << 8) | b);
    }
    // Titlebar bottom accent
    draw_rect(x, y + TITLEBAR_H - 1, ww, 1, border);

    // Window body
    draw_rect(x, y + TITLEBAR_H, ww, wh - TITLEBAR_H, GUI_BG);

    // Title text centered
    int tlen = strlen(w->title);
    int tx = x + (ww - tlen * 8) / 2;
    int ty = y + (TITLEBAR_H - 16) / 2;
    draw_string_px(tx, ty, w->title, GUI_TEXT, 0xFFFFFFFF); // Transparent bg

    // Close button [X] at top-right
    int cbx = x + ww - TITLEBAR_H;
    draw_rect(cbx, y, TITLEBAR_H, TITLEBAR_H - 1, GUI_CLOSE);
    draw_string_px(cbx + (TITLEBAR_H - 8) / 2, y + (TITLEBAR_H - 16) / 2, "x", GUI_WHITE, GUI_CLOSE);

    // Content area border
    draw_rect_border(x, y + TITLEBAR_H, ww, wh - TITLEBAR_H, border);

    // Call app draw_fn with content area coords
    if (w->draw_fn)
        w->draw_fn(w->id, x + 1, y + TITLEBAR_H + 1, ww - 2, wh - TITLEBAR_H - 2);
}

void wm_draw_all() {
    for (int z = 0; z < wm_zcount; z++) draw_one(wm_zorder[z]);
}

// ---- Mouse handling ----
void wm_handle_mouse(int mx, int my, int btn, int pbtn) {
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
                return;
            }
        }
    }
    if (release) {
        for (int i = 0; i < MAX_WINDOWS; i++) wm_wins[i].dragging = 0;
    }

    if (!click) return;

    // Hit test front-to-back
    for (int z = wm_zcount - 1; z >= 0; z--) {
        int idx = wm_zorder[z];
        WmWin* w = &wm_wins[idx];
        if (!w->visible) continue;
        if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) {
            wm_raise(w->id);
            // Close button?
            int cbx = w->x + w->w - TITLEBAR_H;
            if (my < w->y + TITLEBAR_H && mx >= cbx) {
                wm_close(w->id);
                return;
            }
            // Titlebar drag?
            if (my < w->y + TITLEBAR_H) {
                w->dragging = 1;
                w->drag_mx = mx; w->drag_my = my;
                w->drag_wx = w->x; w->drag_wy = w->y;
                return;
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
