#ifndef WM_H
#define WM_H

#include "types.h"

#define MAX_WINDOWS  8
// TITLEBAR_H defined in theme.h (included via vga.h)

typedef void (*WinDrawFn)(int id, int cx, int cy, int cw, int ch);
typedef void (*WinKeyFn) (int id, char c, uint8_t sc);
typedef void (*WinTickFn)(int id);
typedef void (*WinMouseFn)(int id, int cx, int cy, int btn);

typedef struct {
    int id;
    int x, y, w, h;       // pixel coords; h includes titlebar
    char title[48];
    WinDrawFn  draw_fn;
    WinKeyFn   key_fn;
    WinTickFn  tick_fn;
    WinMouseFn mouse_fn;
    int visible;
    int dragging;
    int drag_mx, drag_my;  // mouse pos at drag start
    int drag_wx, drag_wy;  // window pos at drag start
    int minimized;         // 1 = hidden in taskbar
    int maximized;         // 1 = fullscreen
    int snap_state;                          // 0=none, 1=left, 2=right, 3=top (maximized)
    int saved_x, saved_y, saved_w, saved_h; // pre-snap/pre-maximize geometry
    // Resizing state
    int resizing;
    int resize_mx, resize_my;
    int resize_ww, resize_wh;
    int resize_edge; // 1:top, 2:bottom, 4:left, 8:right, and combinations
    // Titlebar button hit-test positions (set by draw_one)
    int close_cx, close_cy, close_r;
    int max_cx,   max_cy,   max_r;
    int min_cx,   min_cy,   min_r;
} WmWin;

extern WmWin wm_wins[MAX_WINDOWS];
extern int wm_zorder[MAX_WINDOWS];
extern int wm_zcount;
extern int wm_focused;

void wm_init();
void wm_raise(int id);
int  wm_open(int x, int y, int w, int h, const char* title,
             WinDrawFn draw_fn, WinKeyFn key_fn, WinTickFn tick_fn, WinMouseFn mouse_fn);
void wm_close(int id);
int  wm_is_open(int id);
void wm_draw_all();
int wm_handle_mouse(int mx, int my, int btn, int pbtn);
void wm_handle_key(char c, uint8_t sc);
void wm_tick_all();

#endif
