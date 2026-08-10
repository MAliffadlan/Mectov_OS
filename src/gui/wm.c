#include "../include/wm.h"
#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/taskbar.h"
#include "../include/serial.h"
#include "../include/spinlock.h"
#include "../include/task.h"   // get_current_task / task_get_cid

// ---- Reentrant irqsave lock (kernel locking audit v38.4) ----
//
// Protects the window table (wm_wins[]), focus, and z-order from concurrent
// access: the main loop draws/raises/focuses, GUI syscalls open/close windows,
// and task teardown calls wm_cleanup_task() — potentially on different cores.
// Public functions call each other and the draw path re-enters via
// win_draw_cb -> get_win_index, so the lock is reentrant (owner cpu,tid + depth).
// Ordering: task_lock > wm_lock > gui_lock.
static spinlock_t wm_lock = SPINLOCK_INIT;
static uint32_t wm_eflags;
static int wm_lock_owner = -1;
static int wm_lock_depth = 0;

void wm_lock_acquire(void) {
    int tid = get_current_task();
    int key = (task_get_cid() << 16) | (tid & 0xFFFF);
    if (wm_lock_owner == key) { wm_lock_depth++; return; }
    wm_eflags = spin_lock_irqsave(&wm_lock);
    wm_lock_owner = key;
    wm_lock_depth = 1;
}

void wm_lock_release(void) {
    if (wm_lock_depth > 1) { wm_lock_depth--; return; }
    wm_lock_depth = 0;
    wm_lock_owner = -1;
    spin_unlock_irqrestore(&wm_lock, wm_eflags);
}

#define SNAP_THRESHOLD  10      // px from edge to trigger snap
#define SNAP_AREA_W     (int)(fb_width / 2)  // half width for left/right snap

// Titlebar button size (ToaruOS-style glyph buttons). Shared by draw_one
// (rendering), wm_track_mouse (hover) and wm_handle_mouse (hit test) so they
// can never drift apart. The 20px hit box is taller than the 16px visual so
// clicks near the titlebar edge still register comfortably.
#define WM_BTN_W 20
#define WM_BTN_H 16
#define WM_BTN_VIS_W 16  // rounded hover/pressed background visual width

// Titlebar button hover state, tracked by wm_track_mouse() on pure mouse
// moves and read by draw_one(). Indexed by window slot, then which button:
// 0 = close, 1 = maximize, 2 = minimize.
static int wm_btn_hover[MAX_WINDOWS][3];

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

// Deferred window-buffer frees. Window buffers are freed ONLY from the main
// loop (task 0) context: close paths (wm_close kernel branch, wm_cleanup_task)
// can run from other preemptable tasks while draw_one() is mid-render on the
// very buffer, so they NULL the field and park the pointer here.
// 64 slots: a full list means >64 kernel windows closed within one frame,
// which never happens in practice; anything beyond that is logged and leaked
// rather than freed from the wrong context.
static void* wm_free_list[64];
static int wm_free_count = 0;

static void wm_defer_free_unlocked(void* p) {
    if (!p) return;
    if (wm_free_count >= 64) {
        write_serial_string("[WM] wm_free_list full, leaking a window buffer\n");
        return;
    }
    wm_free_list[wm_free_count++] = p;
}
void wm_defer_free(void* p) {
    wm_lock_acquire();
    wm_defer_free_unlocked(p);
    wm_lock_release();
}

static void wm_flush_frees_unlocked(void) {
    extern void kfree(void*);
    for (int i = 0; i < wm_free_count; i++) kfree(wm_free_list[i]);
    wm_free_count = 0;
}
void wm_flush_frees(void) {
    wm_lock_acquire();
    wm_flush_frees_unlocked();
    wm_lock_release();
}

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

static void wm_raise_unlocked(int id) {
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
void wm_raise(int id) {
    wm_lock_acquire();
    wm_raise_unlocked(id);
    wm_lock_release();
}

// ---- Minimize / restore ----
// These own BOTH halves of the operation: the state change and the damage it
// causes. Do not set .minimized directly from a call site — draw_one() skips
// minimized windows, so nothing will ever repaint over the pixels the window
// left behind and it stays painted on screen as a ghost.
static void wm_minimize_unlocked(int id) {
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
void wm_minimize(int id) {
    wm_lock_acquire();
    wm_minimize_unlocked(id);
    wm_lock_release();
}

static void wm_restore_unlocked(int id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm_wins[i].visible && wm_wins[i].id == id) {
            wm_wins[i].minimized = 0;
            wm_raise(id); // marks the screen dirty and sets needs_redraw
            return;
        }
    }
}
void wm_restore(int id) {
    wm_lock_acquire();
    wm_restore_unlocked(id);
    wm_lock_release();
}

static void wm_focus_next_unlocked(void) {
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
void wm_focus_next(void) {
    wm_lock_acquire();
    wm_focus_next_unlocked();
    wm_lock_release();
}

int alt_tab_active = 0;
int alt_tab_selected_idx = 0;

static void wm_alt_tab_start_unlocked(void) {
    if (wm_zcount <= 1) return;
    alt_tab_active = 1;
    alt_tab_selected_idx = 1; // Highlight the second window by default
    extern void mark_dirty(int, int, int, int);
    mark_dirty(0, 0, fb_width, fb_height); // Show HUD card cleanly
}
void wm_alt_tab_start(void) {
    wm_lock_acquire();
    wm_alt_tab_start_unlocked();
    wm_lock_release();
}

static void wm_alt_tab_next_unlocked(void) {
    if (!alt_tab_active || wm_zcount <= 1) return;
    alt_tab_selected_idx = (alt_tab_selected_idx + 1) % wm_zcount;
}
void wm_alt_tab_next(void) {
    wm_lock_acquire();
    wm_alt_tab_next_unlocked();
    wm_lock_release();
}

static void wm_alt_tab_end_unlocked(void) {
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
void wm_alt_tab_end(void) {
    wm_lock_acquire();
    wm_alt_tab_end_unlocked();
    wm_lock_release();
}

// Alt-tab HUD icon. Delegates to the shared draw_app_icon() (from taskbar.c)
// so the HUD, taskbar and titlebar all show identical, correctly-matched
// icons for the same window title.
static void draw_hud_icon(int ix, int iy, const char* title) {
    draw_app_icon(ix, iy, title, 24);
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
    
    // Background card (ToaruOS dark)
    draw_rect(hx, hy, hud_w, hud_h, 0x00383838);
    draw_rect_border(hx, hy, hud_w, hud_h, TOARU_BORDER);
    
    // Draw each visible window in switcher
    for (int i = 0; i < wm_zcount; i++) {
        int idx = wm_zorder[wm_zcount - 1 - i];
        int item_x = hx + padding + i * (item_w + gap);
        int item_y = hy + 16;
        
        int selected = (i == alt_tab_selected_idx);
        
        // Highlight box (ToaruOS selection blue)
        if (selected) {
            draw_rect(item_x - 4, item_y - 4, item_w + 8, item_w + 8, 0x003C5C8E);
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
        
        uint32_t text_col = selected ? 0x00FFFFFF : 0x00999999;
        draw_string_px(item_x + (item_w - j*8)/2, item_y + 36, short_title, text_col, 0xFFFFFFFF);
    }
    
    // Selected window name at the bottom of the HUD card
    int active_idx = wm_zorder[wm_zcount - 1 - alt_tab_selected_idx];
    const char* full_title = wm_wins[active_idx].title;
    int title_len = strlen(full_title);
    draw_string_px(hx + (hud_w - title_len*8)/2, hy + 76, full_title, 0x00E2E2E2, 0xFFFFFFFF);
}

// ---- Open / Close ----
static int wm_open_unlocked(int x, int y, int w, int h, const char* title,
            WinDrawFn draw_fn, WinKeyFn key_fn, WinTickFn tick_fn, WinMouseFn mouse_fn) {
    // Reject absurd geometry: content_buffer is kmalloc(cw2*ch2*4) in draw_one,
    // so an int overflow here (e.g. w=h=65539) turned a tiny allocation into a
    // heap overflow and a multi-hundred-MB OOB read. Cap to 4x the framebuffer.
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return -1;
    if (w > (int)fb_width * 4 || h > (int)fb_height * 4) return -1;
    // Clamp absurd positions. A fully off-screen window is unreachable by
    // mouse (and an x+w overflow makes the hit test wrap negative), so a Ring
    // 3 app could plant a window that can never be closed — a zombie-window
    // DoS. x is clamped BEFORE the x+w test so that expression cannot
    // overflow: x <= fb_width (1024) and w <= 8192 keep it far below 2^31.
    // Partially off-screen windows are allowed.
    if (x > (int)fb_width || x + w < 0) x = 0;
    if (y > (int)fb_height || y + h < 0) y = 0;
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
            wm_wins[i].last_cw = w - 2;
            wm_wins[i].last_ch = h - TITLEBAR_H - 2;
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
int wm_open(int x, int y, int w, int h, const char* title,
            WinDrawFn draw_fn, WinKeyFn key_fn, WinTickFn tick_fn, WinMouseFn mouse_fn) {
    wm_lock_acquire();
    int r = wm_open_unlocked(x, y, w, h, title, draw_fn, key_fn, tick_fn, mouse_fn);
    wm_lock_release();
    return r;
}

static int wm_is_open_unlocked(int id) {
    if (id < 0) return 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm_wins[i].visible && wm_wins[i].id == id) return 1;
    }
    return 0;
}
int wm_is_open(int id) {
    wm_lock_acquire();
    int r = wm_is_open_unlocked(id);
    wm_lock_release();
    return r;
}

static void wm_invalidate_unlocked(int id) {
    if (id < 0) return;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm_wins[i].visible && wm_wins[i].id == id) {
            wm_wins[i].buffer_dirty = 1;
            return;
        }
    }
}
void wm_invalidate(int id) {
    wm_lock_acquire();
    wm_invalidate_unlocked(id);
    wm_lock_release();
}

static void wm_close_unlocked(int id) {
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
                    // Defer free — see wm_cleanup_task(); wm_close() can be called
                    // from kernel-task apps (taskmgr, power, nano) that preempt task 0.
                    wm_defer_free(wm_wins[i].content_buffer);
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
void wm_close(int id) {
    wm_lock_acquire();
    wm_close_unlocked(id);
    wm_lock_release();
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

// Titlebar button geometry, single source of truth for draw_one(),
// wm_track_mouse() and wm_handle_mouse(). Buttons are right-aligned in
// ToaruOS fashion: minimize, maximize, close (left to right).
static void wm_btn_geom(const WmWin* w, int* close_x, int* max_x, int* min_x, int* btn_y) {
    *btn_y = w->y + (TITLEBAR_H - WM_BTN_H) / 2;
    *close_x = w->x + w->w - WM_BTN_W - 3;
    *max_x   = *close_x - WM_BTN_W - 2;
    *min_x   = *max_x - WM_BTN_W - 2;
}

// Pure-mouse-move tracking for titlebar button hover states. Called from the
// main loop on every mouse move (no button). Clears hover when the pointer is
// not over any visible window's titlebar buttons.
static void wm_track_mouse_unlocked(int mx, int my) {
    // Hover states are fully recomputed on every move; anything not explicitly
    // set below is already cleared here, so hovers can never go stale.
    for (int i = 0; i < MAX_WINDOWS; i++) {
        wm_btn_hover[i][0] = wm_btn_hover[i][1] = wm_btn_hover[i][2] = 0;
    }
    if (my < 0) return;
    for (int z = wm_zcount - 1; z >= 0; z--) {
        int idx = wm_zorder[z];
        WmWin* w = &wm_wins[idx];
        if (!w->visible || w->minimized) continue;
        if (mx < w->x || mx >= w->x + w->w || my < w->y || my >= w->y + w->h) continue;
        if (my >= w->y + TITLEBAR_H) break; // pointer in content area of top window
        // Over the titlebar of the topmost window under the pointer
        int cx, mx2, nx, by;
        wm_btn_geom(w, &cx, &mx2, &nx, &by);
        if (mx >= cx && mx < cx + WM_BTN_W && my >= by && my < by + WM_BTN_H) {
            wm_btn_hover[idx][0] = 1;
        } else if (mx >= mx2 && mx < mx2 + WM_BTN_W && my >= by && my < by + WM_BTN_H) {
            wm_btn_hover[idx][1] = 1;
        } else if (mx >= nx && mx < nx + WM_BTN_W && my >= by && my < by + WM_BTN_H) {
            wm_btn_hover[idx][2] = 1;
        }
        break;
    }
}
void wm_track_mouse(int mx, int my) {
    wm_lock_acquire();
    wm_track_mouse_unlocked(mx, my);
    wm_lock_release();
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

    // ========== Window Body (ToaruOS dark, flat) ==========
    draw_rect(x, y + TITLEBAR_H, ww, wh - TITLEBAR_H, TOARU_TITLE_I);
    // Subtle inner highlight where the body meets the titlebar
    draw_rect(x, y + TITLEBAR_H, ww, 1, TOARU_BORDER);

    // ========== Titlebar (ToaruOS flat solid, no gradient) ==========
    draw_rect(x, y, ww, TITLEBAR_H, focused ? TOARU_TITLE : TOARU_TITLE_I);

    // ========== Title text (centered, clean monochrome titlebar) ==========
    // NOTE: no app icon here on purpose — colored icons made the titlebar read
    // as yellow/orange next to the dark chrome. Just the centered title.
    int close_x, max_x, min_x, btn_y;
    wm_btn_geom(w, &close_x, &max_x, &min_x, &btn_y);
    int btn_left = min_x - 2;
    int tlen = strlen(w->title);
    int title_w = tlen * 8;
    int center_l = x + 6;
    int center_r = btn_left;
    if (center_r - center_l < title_w + 8) {
        // Narrow window: center in the whole titlebar and let it clip
        center_l = x;
        center_r = x + ww;
    }
    int tx = center_l + (center_r - center_l - title_w) / 2;
    if (tx < center_l) tx = center_l;
    int tty = y + (TITLEBAR_H - 8) / 2;
    uint32_t title_col = focused ? TOARU_TEXT : TOARU_TEXT_I;
    draw_string_px(tx, tty, w->title, title_col, 0xFFFFFFFF);

    // ========== Titlebar buttons (ToaruOS glyph buttons, right side) ==========
    // Each button: 20x16 hit box, 16x16 rounded hover/pressed visual, small
    // light-gray glyph. Close gets a red hover to signal destructiveness.
    int gx = 5;          // glyph half-width for X
    int gmx = 6;         // glyph inset for square/line

    // Close button (X) — flat gray hover, same as the other buttons
    if (wm_btn_hover[idx][0]) {
        draw_rounded_rect(close_x + (WM_BTN_W - WM_BTN_VIS_W) / 2, btn_y + (WM_BTN_H - WM_BTN_VIS_W) / 2,
                          WM_BTN_VIS_W, WM_BTN_VIS_W, 3, TOARU_BTN_HOV);
    }
    draw_line(close_x + WM_BTN_W/2 - gx, btn_y + WM_BTN_H/2 - gx,
              close_x + WM_BTN_W/2 + gx, btn_y + WM_BTN_H/2 + gx, TOARU_BTN_GLYPH);
    draw_line(close_x + WM_BTN_W/2 + gx, btn_y + WM_BTN_H/2 - gx,
              close_x + WM_BTN_W/2 - gx, btn_y + WM_BTN_H/2 + gx, TOARU_BTN_GLYPH);
    w->close_cx = close_x; w->close_cy = btn_y;

    // Minimize button (bottom bar)
    if (wm_btn_hover[idx][2]) {
        draw_rounded_rect(min_x + (WM_BTN_W - WM_BTN_VIS_W) / 2, btn_y + (WM_BTN_H - WM_BTN_VIS_W) / 2,
                          WM_BTN_VIS_W, WM_BTN_VIS_W, 3, TOARU_BTN_HOV);
    }
    draw_rect(min_x + WM_BTN_W/2 - gmx, btn_y + WM_BTN_H/2, gmx * 2, 1, TOARU_BTN_GLYPH);
    w->min_cx = min_x; w->min_cy = btn_y;

    // Maximize button (square outline; restore glyph when maximized)
    if (wm_btn_hover[idx][1]) {
        draw_rounded_rect(max_x + (WM_BTN_W - WM_BTN_VIS_W) / 2, btn_y + (WM_BTN_H - WM_BTN_VIS_W) / 2,
                          WM_BTN_VIS_W, WM_BTN_VIS_W, 3, TOARU_BTN_HOV);
    }
    if (w->maximized || w->snap_state != SNAP_NONE) {
        // Restore glyph: two overlapping squares
        draw_rect_border(max_x + WM_BTN_W/2 - gmx + 1, btn_y + WM_BTN_H/2 - gmx + 1, gmx, gmx, TOARU_BTN_GLYPH);
        draw_rect_border(max_x + WM_BTN_W/2 - gmx - 2, btn_y + WM_BTN_H/2 - gmx - 2, gmx, gmx, TOARU_BTN_GLYPH);
    } else {
        draw_rect_border(max_x + WM_BTN_W/2 - gmx, btn_y + WM_BTN_H/2 - gmx, gmx * 2, gmx * 2, TOARU_BTN_GLYPH);
    }
    w->max_cx = max_x; w->max_cy = btn_y;

    // ========== 1px solid frame (ToaruOS) ==========
    // Drawn LAST so it sits on top of the titlebar gradient. Stays INSIDE the
    // window's own bounds (x,y,w,h): draw_one()'s dirty-rect cull check and
    // mark_dirty use the interior rect, so painting the frame at x-1/y-1 could
    // leave a 1px stale ring on screen when the cull skips it.
    draw_rect_border(x, y, ww, wh, focused ? TOARU_BORDER : TOARU_BORDER_I);

    // ========== Content area ==========
    int cx2 = x + 1;
    int cy2 = y + TITLEBAR_H + 1;
    int cw2 = ww - 2;
    int ch2 = wh - TITLEBAR_H - 2;

    if (cw2 > 0 && ch2 > 0) {
        // 1. Grow-only content buffer. Reusing the capacity means live resize
        //    doesn't kmalloc/kfree a fresh cw2*ch2*4 buffer on every mouse move
        //    (that churn is what made resizing feel janky/stiff).
        //    While the mouse button is held (resizing) we NEVER (re)allocate:
        //    the buffer keeps its last committed size, so the rubber-band
        //    preview below blits only the valid old-content region and clears
        //    the growth strips. Allocating mid-drag was also corrupting the
        //    preserved copy: last_cw/last_ch stay stale during a drag, so a
        //    second growth realloc copied rows with the wrong source stride
        //    and skewed the content on screen.
        if (!w->resizing && (w->content_buffer == NULL || w->content_cap < cw2 * ch2)) {
            extern void* kmalloc(uint32_t);
            extern void kfree(void*);
            uint32_t* nb = kmalloc(cw2 * ch2 * 4);
            if (nb == NULL) {
                return; // Out of memory: keep last good buffer, skip this frame
            }
            if (!w->visible) {
                // Window closed (wm_cleanup_task/wm_close ran, deferred the old
                // buffer) while we were preempted mid-draw. Don't allocate a
                // zombie buffer that nothing will ever free.
                kfree(nb);
                return;
            }
            if (w->content_buffer) kfree(w->content_buffer);
            w->content_buffer = nb;
            w->content_cap = cw2 * ch2;
            w->buffer_dirty = 1;
        }

        // 2. Size changed. While the mouse button is held (resizing) we defer:
        //    no event, no re-render — the frame only blits the old content and
        //    clears the growth strips (rubber-band). last_cw/last_ch stay stale
        //    so the first frame after mouse release detects the change and does
        //    ONE clean re-render + notifies the app (event type 5, client w/h).
        int size_changed = (w->last_cw != cw2 || w->last_ch != ch2);
        if (size_changed && w->resizing) {
            w->buffer_dirty = 0; // cancel grow-triggered render; skip event
        } else if (size_changed) {
            if (w->owner_ring == 3) {
                extern void push_event(int, int, int, int, int);
                push_event(w->id, 5, cw2, ch2, 0);
            }
            w->last_cw = cw2;
            w->last_ch = ch2;
            w->buffer_dirty = 1;
        }

        // 3. Render to off-screen buffer if dirty. Never render while the
        //    mouse button is held: the buffer is intentionally kept at its last
        //    committed size during a drag, so drawing into it with the live
        //    (larger) cw2/ch2 would overflow the allocation. Renders are
        //    deferred until release, where buffer_dirty is forced back on.
        if (w->content_buffer && w->buffer_dirty && !w->resizing) {
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

        // 3. Composite (blit) the off-screen buffer to the global back buffer.
        //    During a resize drag last_cw/last_ch are stale, so blit only the
        //    old-content region and clear the exposed growth strips cleanly.
        if (w->content_buffer) {
            int blit_w = (w->last_cw < cw2) ? w->last_cw : cw2;
            int blit_h = (w->last_ch < ch2) ? w->last_ch : ch2;
            // src_pitch = w->last_cw, NOT blit_w: the buffer keeps its last
            // committed row width during a drag (grow-only, deferred realloc),
            // so when the window is SHRUNK below the buffer width, blitting
            // with stride==blit_w would read row n from the middle of row n-1
            // and skew/garb the preserved content. This was the visible
            // "resize glitch" while dragging smaller.
            vga_blit_buffer(w->content_buffer, blit_w, blit_h, w->last_cw, cx2, cy2);
            if (blit_w < cw2) draw_rect(cx2 + blit_w, cy2, cw2 - blit_w, ch2, 0x001E1E2E);
            if (blit_h < ch2) draw_rect(cx2, cy2 + blit_h, cw2, ch2 - blit_h, 0x001E1E2E);
        }
    }
}

static void wm_draw_all_unlocked() {
    wm_flush_frees(); // release closed-window buffers (main-loop context only)
    for (int z = 0; z < wm_zcount; z++) draw_one(wm_zorder[z]);
    wm_draw_alt_tab_hud();
}
void wm_draw_all() {
    wm_lock_acquire();
    wm_draw_all_unlocked();
    wm_lock_release();
}

// ---- Mouse handling ----
static int wm_handle_mouse_unlocked(int mx, int my, int btn, int pbtn) {
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

                    // Clamp to screen: an unbounded window grew cw2*ch2*4
                    // allocations beyond any sane size on every resize frame.
                    // Left/top edges: dragging them off-screen would push the
                    // window's x/y negative; pull the opposite edge back in.
                    if (new_x < 0) { new_w += new_x; new_x = 0; }
                    if (new_y < 0) { new_h += new_y; new_y = 0; }
                    if (new_x + new_w > (int)fb_width) new_w = (int)fb_width - new_x;
                    if (new_y + new_h > (int)fb_height) new_h = (int)fb_height - new_y;
                    if (new_w < 220) new_w = 220;
                    if (new_h < 150) new_h = 150;
                    // Re-clamp the position AFTER forcing the minimum size: the
                    // screen clamp above can shrink the window below 220x150,
                    // then the minimum push re-extends it past the screen edge
                    // with the titlebar buttons off-screen (uncloseable).
                    if (new_x + new_w > (int)fb_width) new_x = (int)fb_width - new_w;
                    if (new_y + new_h > (int)fb_height) new_y = (int)fb_height - new_h;
                    if (new_x < 0) new_x = 0;
                    if (new_y < 0) new_y = 0;

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
                if (wm_wins[i].resizing) {
                    // The drag deferred every render; force one final clean
                    // render now so the rubber-band preview always resolves,
                    // even when the drag ends back at the original size.
                    wm_wins[i].buffer_dirty = 1;
                }
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
                // ---- ToaruOS glyph button hit test (shared WM_BTN_W x WM_BTN_H) ----
                int cx, mx2, nx, by;
                wm_btn_geom(w, &cx, &mx2, &nx, &by);

                // Close button
                if (mx >= cx && mx < cx + WM_BTN_W && my >= by && my < by + WM_BTN_H) {
                    write_serial_string("WM_CLOSE via MOUSE!\n");
                    wm_close(w->id);
                    return 1;
                }

                // Maximize button
                if (mx >= mx2 && mx < mx2 + WM_BTN_W && my >= by && my < by + WM_BTN_H) {
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
                if (mx >= nx && mx < nx + WM_BTN_W && my >= by && my < by + WM_BTN_H) {
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
int wm_handle_mouse(int mx, int my, int btn, int pbtn) {
    wm_lock_acquire();
    int r = wm_handle_mouse_unlocked(mx, my, btn, pbtn);
    wm_lock_release();
    return r;
}

// ---- Scroll wheel handling ----
static void wm_handle_scroll_unlocked(int mx, int my, int delta) {
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
void wm_handle_scroll(int mx, int my, int delta) {
    wm_lock_acquire();
    wm_handle_scroll_unlocked(mx, my, delta);
    wm_lock_release();
}

static void wm_handle_key_unlocked(char c, uint8_t sc) {
    if (wm_focused < 0) return;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm_wins[i].visible && wm_wins[i].id == wm_focused && wm_wins[i].key_fn)
            wm_wins[i].key_fn(wm_wins[i].id, c, sc);
    }
}
void wm_handle_key(char c, uint8_t sc) {
    wm_lock_acquire();
    wm_handle_key_unlocked(c, sc);
    wm_lock_release();
}

static void wm_tick_all_unlocked() {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wm_wins[i].visible && wm_wins[i].tick_fn)
            wm_wins[i].tick_fn(wm_wins[i].id);
}
void wm_tick_all() {
    wm_lock_acquire();
    wm_tick_all_unlocked();
    wm_lock_release();
}

// Close all windows owned by a specific task (crash recovery)
static void wm_cleanup_task_unlocked(int tid) {
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
                // Defer the free: task_kill() can run while the main-loop thread
                // (task 0) is inside draw_one() for this very buffer. Freeing it
                // here would let draw_one() blit/render a freed pointer. Null the
                // field now; wm_flush_frees() (main-loop context) releases it.
                wm_defer_free(wm_wins[i].content_buffer);
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
void wm_cleanup_task(int tid) {
    wm_lock_acquire();
    wm_cleanup_task_unlocked(tid);
    wm_lock_release();
}

static void wm_reset_session_unlocked(void) {
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
void wm_reset_session(void) {
    wm_lock_acquire();
    wm_reset_session_unlocked();
    wm_lock_release();
}
