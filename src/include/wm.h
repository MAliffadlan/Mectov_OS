#ifndef WM_H
#define WM_H

#include "types.h"

// Window table (v38.44): 32 slots — the per-window draw canvases in
// syscall.c grow with this (~147 KB BSS each), still fine at 128 MB RAM.
#define MAX_WINDOWS  32
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
    int content_drag;      // 1 = mouse button held on this window's content
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
    // Titlebar button hit-test top-left corners (set by draw_one; button size
    // is the shared WM_BTN_W/WM_BTN_H constants in wm.c)
    int close_cx, close_cy;
    int max_cx,   max_cy;
    int min_cx,   min_cy;
    int owner_ring; // 0 for kernel, 3 for user mode apps
    int owner_task; // task ID that created this window (-1 = kernel)
    // v38.103 game input: raw scancodes + relative-motion mouse capture.
    int want_scancodes;             // 1 = key_fn gets press AND release scancodes
    int capture_mouse;              // 1 = this window owns relative mouse motion
    int cap_saved_x, cap_saved_y;   // cursor position to restore on release
    
    // v38.110 perf accounting: draw_one() times this window's app draw and
    // content blit when set (the q3arena game window); the driver reads the
    // accumulated microseconds through wm_q3_times().
    int is_q3_game;
    // Composite WM state
    uint32_t* content_buffer; // Off-screen canvas for window content
    int       content_cap;    // Allocated canvas size (pixels) — grow-only, so
                              // live resize doesn't kmalloc/kfree per mouse move
    int       buffer_dirty;   // 1 = Needs redraw by app
    int       last_cw;        // Track buffer width
    int       last_ch;        // Track buffer height
} WmWin;

extern WmWin wm_wins[MAX_WINDOWS];
extern int wm_zorder[MAX_WINDOWS];
extern int wm_zcount;
extern int wm_focused;

void wm_init();
void wm_raise(int id);
// Minimize/restore. Always go through these — they mark the damaged region
// dirty, which setting WmWin.minimized by hand does not.
void wm_minimize(int id);
void wm_restore(int id);
void wm_focus_next(void);

extern int alt_tab_active;
extern int alt_tab_selected_idx;
void wm_alt_tab_start(void);
void wm_alt_tab_next(void);
void wm_alt_tab_end(void);
int  wm_open(int x, int y, int w, int h, const char* title,
             WinDrawFn draw_fn, WinKeyFn key_fn, WinTickFn tick_fn, WinMouseFn mouse_fn);
void wm_close(int id);
int  wm_is_open(int id);
void wm_invalidate(int id);
void wm_draw_all();
/* v38.110: MILLISECOND cost of the last full composite pass (wm_draw_all),
 * of blitting the q3arena game window's content buffer into the back buffer,
 * and of that window's app draw callback (q3ref_blit + HUD) — kernel tick
 * clock, not rdtsc (see wm.c). The renderer task samples these once per
 * sampled frame for the perf breakdown. */
void wm_q3_times(int *pass_ms, int *blit_ms, int *draw_ms);
/* v38.110: the q3arena driver tags its window, opting it into the per-window
 * game accounting in draw_one(). */
void wm_tag_q3_game(int id);
void wm_track_mouse(int mx, int my); // Pure-move hover tracking (titlebar buttons)
int wm_handle_mouse(int mx, int my, int btn, int pbtn);
void wm_handle_scroll(int mx, int my, int delta);
void wm_handle_key(char c, uint8_t sc);

// ---- v38.103: game input (raw scancodes + mouse capture) ----
// A window can ask for raw scancode delivery: while it holds focus, the
// desktop loop routes BOTH the press and the release (0x80 bit intact) to its
// key_fn instead of the character path. Games need key-up events (movement
// keys held down) and non-character keys (ESC, arrows, ctrl).
void wm_request_scancodes(int id, int on);
// id of the focused window receiving raw scancodes, or -1.
int  wm_scancode_focus(void);
void wm_handle_scancode(uint8_t sc, char c);

// Mouse capture for FPS-style look: the capturing window stops receiving
// absolute hover/click coordinates and instead gets mouse_fn(id, dx, dy, btn)
// with RELATIVE motion on every packet — no button needed, no cursor
// dead-ends at the screen edge. The desktop arrow is pinned inside the window
// and hidden while the capture lasts; the WM stops routing hover, drag,
// resize, taskbar and desktop mouse events.
// Returns 1 on success (0 = no such visible window).
int  wm_capture_mouse(int id, int on);
int  wm_capture_owner(void);                        // capturing window id, or -1
int  wm_capture_event(int dx, int dy, int btn);     // deliver relative motion
int  wm_capture_center(int *x, int *y);             // where the arrow is pinned
void wm_tick_all();
void wm_cleanup_task(int tid);  // Close all windows owned by task tid
void wm_reset_session(void);     // Close every window and reset WM state

#endif
