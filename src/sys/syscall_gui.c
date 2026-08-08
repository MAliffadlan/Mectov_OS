#include "../include/idt.h"
#include "../include/syscall.h"
#include "../include/task.h"
#include "../include/vfs.h"
#include "../include/net.h"
#include "../include/wm.h"
#include "../include/ipc.h"
#include "../include/serial.h"
#include "../include/utils.h"
#include "../include/keyboard.h"
#include "../include/mouse.h"
#include "../include/fd.h"
#include "../include/spinlock.h"

extern spinlock_t gui_canvas_lock;
extern void gui_lock(void);
extern void gui_unlock(void);

extern int validate_user_ptr(const void* ptr, uint32_t size);
extern int safe_strlen(const char* s, int max);
extern void print(const char* s, uint8_t color);

extern int get_win_index(int wid);
extern void push_event(int wid, int type, int x, int y, int key);
extern void win_draw_cb(int id, int cx, int cy, int cw, int ch);
extern void win_key_cb(int id, char c, uint8_t sc);
extern void win_mouse_cb(int id, int cx, int cy, int btn);

#define MAX_EVENTS 64
typedef struct {
    int type; // 1 = paint, 2 = key, 3 = mouse
    int x, y;
    int key;
} gui_event_t;

typedef struct {
    gui_event_t events[MAX_EVENTS];
    int head;
    int tail;
} win_event_queue_t;

typedef struct {
    int type; // 1 = rect, 2 = text
    int x, y, w, h;
    uint32_t color;
    char text[128];
} draw_cmd_t;

#define MAX_DRAW_CMDS 512
typedef struct {
    draw_cmd_t cmds[MAX_DRAW_CMDS];
    int count;
    int pending_count;
    draw_cmd_t pending_cmds[MAX_DRAW_CMDS];
} win_canvas_t;

extern win_event_queue_t win_queues[];
extern win_canvas_t win_canvases[];

uint32_t handle_syscall_gui(registers_t* regs) {
    switch (regs->eax) {
        // ----- SYS_DRAW_RECT (11): Draw rectangle in window -----
        case SYS_DRAW_RECT: {
            int wid = (int)regs->ebx;
            int x = (int)regs->ecx;
            int y = (int)regs->edx;
            int w = (regs->esi >> 16) & 0xFFFF;
            int h = regs->esi & 0xFFFF;
            uint32_t color = (uint32_t)regs->edi;
            
            int idx = get_win_index(wid);
            if (idx >= 0) {
                if (win_canvases[idx].pending_count < MAX_DRAW_CMDS) {
                    draw_cmd_t* cmd = &win_canvases[idx].pending_cmds[win_canvases[idx].pending_count++];
                    cmd->type = 1; cmd->x = x; cmd->y = y; cmd->w = w; cmd->h = h; cmd->color = color;
                } else {
                    write_serial_string("SYS_DRAW_RECT: Full\n");
                }
            } else {
                extern int get_current_task(void);
                int tid = get_current_task();
                write_serial_string("SYS_DRAW_RECT: Task ");
                write_serial_hex(tid);
                write_serial_string(" Invalid WID ");
                write_serial_hex(wid);
                write_serial('\n');
            }
            regs->eax = 0;
            break;
        }

        // ----- SYS_DRAW_TEXT (12): Draw text in window -----
        case SYS_DRAW_TEXT: {
            int wid = (int)regs->ebx;
            int x = (int)regs->ecx;
            int y = (int)regs->edx;
            const char* text = (const char*)regs->esi;
            uint32_t color = (uint32_t)regs->edi;
            if (safe_strlen(text, 127) < 0) { regs->eax = (uint32_t)-1; break; }
            
            int idx = get_win_index(wid);
            if (idx >= 0) {
                if (win_canvases[idx].pending_count < MAX_DRAW_CMDS) {
                    draw_cmd_t* cmd = &win_canvases[idx].pending_cmds[win_canvases[idx].pending_count++];
                    cmd->type = 2; cmd->x = x; cmd->y = y; cmd->color = color;
                    int i = 0;
                    while (text[i] && i < 127) { cmd->text[i] = text[i]; i++; }
                    cmd->text[i] = '\0';
                }
            }
            regs->eax = 0;
            break;
        }

        // ----- SYS_GET_KEY (13): Non-blocking keyboard read -----
        case SYS_GET_KEY: {
            extern int term_app_running;
            extern int term_app_task_id;
            extern int get_current_task(void);
            extern uint8_t term_app_pop_key(void);
            extern int task_is_background(int);
            extern int task_signal(int, int);

            // SIGTTIN: a background process-group member reading from the
            // controlling terminal is stopped (default action). The terminal's
            // own shell is exempt; apps launched from the desktop have no
            // group and are unaffected. We still deliver a key to apps that
            // caught/ignored SIGTTIN, so the read never wedges.
            int me = get_current_task();
            if (me != term_app_task_id && task_is_background(me)) {
                task_signal(me, SIGTTIN);
                write_serial_string("[SIG] SIGTTIN to background tid=");
                write_serial_hex(me);
                write_serial_string("\n");
                regs->eax = 0;
                break;
            }

            uint8_t sc = 0;
            if (term_app_running && me == term_app_task_id) {
                sc = term_app_pop_key();
            } else {
                sc = k_get_scancode();
            }
            
            if (sc == 0 || sc >= 0x80) {
                regs->eax = 0; // No key or key release
            } else {
                regs->eax = (uint32_t)scancode_to_char(sc);
            }
            break;
        }

        // ----- SYS_GET_MOUSE (14): Get mouse state -----
        case SYS_GET_MOUSE: {
            regs->eax = (uint32_t)mouse_x;
            regs->ebx = (uint32_t)mouse_y;
            regs->ecx = (uint32_t)mouse_btn;
            break;
        }

        // ----- SYS_CREATE_WINDOW (15) -----
        case SYS_CREATE_WINDOW: {
            int x = (int)regs->ebx;
            int y = (int)regs->ecx;
            int w = (int)regs->edx;
            int h = (int)regs->esi;
            const char* title = (const char*)regs->edi;
            if (safe_strlen(title, 48) < 0) { regs->eax = (uint32_t)-1; break; }
            int wid = wm_open(x, y, w, h, title, win_draw_cb, win_key_cb, NULL, win_mouse_cb);
            
            // Hex mirror so automated tests can count how many windows
            // actually opened (wid is a positive id, or -1 when the WM's
            // window table is full).
            write_serial_string("[WM] create wid=");
            write_serial_hex((uint32_t)wid);
            write_serial_string(" title=");
            write_serial_string(title);
            write_serial('\n');

            int idx = get_win_index(wid);
            if (idx >= 0) {
                wm_wins[idx].owner_ring = 3; // From syscall -> Ring 3
                wm_wins[idx].owner_task = get_current_task(); // Track owner for crash recovery
                win_queues[idx].head = 0;
                win_queues[idx].tail = 0;
                win_canvases[idx].count = 0;
                win_canvases[idx].pending_count = 0;
                // Force an initial paint event so the app knows it can draw
                push_event(wid, 1, 0, 0, 0);
            }
            regs->eax = (uint32_t)wid;
            break;
        }
        case SYS_GET_EVENT: {
            int wid = (int)regs->ebx;
            int idx = get_win_index(wid);
            if (idx >= 0) {
                if (!validate_user_ptr((void*)regs->ecx, sizeof(gui_event_t))) {
                    regs->eax = (uint32_t)-2;
                    break;
                }
                int h = win_queues[idx].head;
                if (h != win_queues[idx].tail) {
                    gui_lock();
                    gui_event_t* ev_ptr = (gui_event_t*)regs->ecx;
                    *ev_ptr = win_queues[idx].events[h];
                    win_queues[idx].head = (h + 1) % MAX_EVENTS;
                    gui_unlock();
                    regs->eax = 1;
                } else {
                    regs->eax = 0;
                }
            } else {
                regs->eax = (uint32_t)-1;
            }
            break;
        }

        // ----- SYS_UPDATE_WINDOW (17) -----
        case SYS_UPDATE_WINDOW: {
            int wid = (int)regs->ebx;
            int idx = get_win_index(wid);
            if (idx >= 0) {
                // Swap pending display list to active display list!
                // Locked against win_draw_cb() replay on the BSP (SMP).
                gui_lock();
                for (int i = 0; i < win_canvases[idx].pending_count; i++) {
                    win_canvases[idx].cmds[i] = win_canvases[idx].pending_cmds[i];
                }
                win_canvases[idx].count = win_canvases[idx].pending_count;
                win_canvases[idx].pending_count = 0; // Reset for next frame
                gui_unlock();
                
                // Composite WM: trigger a redraw for this window's buffer
                wm_wins[idx].buffer_dirty = 1;
                extern volatile int needs_redraw;
                needs_redraw = 1;
            } else {
                write_serial_string("UPD_WIN: Invalid WID\n");
            }
            regs->eax = 0;
            break;
        }
    }
    return regs->eax;
}
