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

uint32_t handle_syscall_ipc(registers_t* regs) {
    switch (regs->eax) {
        // ----- SYS_IPC_CREATE (23) -----
        case SYS_IPC_CREATE: {
            uint32_t key = (uint32_t)regs->ebx;
            int qid = ipc_queue_create(key);
            regs->eax = (uint32_t)qid;
            write_serial_string("[SYS] ipc_create key=");
            write_serial_hex(key);
            write_serial_string(" qid=");
            write_serial('0' + qid);
            write_serial('\n');
            break;
        }

        // ----- SYS_IPC_SEND (24) -----
        case SYS_IPC_SEND: {
            int qid = (int)regs->ebx;
            uint32_t type = (uint32_t)regs->ecx;
            const void* data = (const void*)regs->edx;
            uint32_t len = (uint32_t)regs->esi;
            if (data && !validate_user_ptr(data, len)) {
                regs->eax = (uint32_t)-1;
                break;
            }
            regs->eax = (uint32_t)ipc_send(qid, type, data, len);
            break;
        }

        // ----- SYS_IPC_RECV (25) -----
        case SYS_IPC_RECV: {
            int qid = (int)regs->ebx;
            uint32_t* type_out = (uint32_t*)regs->ecx;
            void* data_out = (void*)regs->edx;
            uint32_t* len_out = (uint32_t*)regs->esi;
            if ((type_out && !validate_user_ptr(type_out, 4)) ||
                (data_out && !validate_user_ptr(data_out, IPC_MSG_SIZE)) ||
                (len_out && !validate_user_ptr(len_out, 4))) {
                regs->eax = (uint32_t)-1;
                break;
            }
            regs->eax = (uint32_t)ipc_receive(qid, NULL, type_out, data_out, len_out);
            break;
        }

        // ----- SYS_IPC_DESTROY (26) -----
        case SYS_IPC_DESTROY: {
            int qid = (int)regs->ebx;
            ipc_queue_destroy(qid);
            regs->eax = 0;
            break;
        }

        // ----- SYS_IPC_TRY_SEND (27) -----
        case SYS_IPC_TRY_SEND: {
            int qid = (int)regs->ebx;
            uint32_t type = (uint32_t)regs->ecx;
            const void* data = (const void*)regs->edx;
            uint32_t len = (uint32_t)regs->esi;
            if (data && !validate_user_ptr(data, len)) {
                regs->eax = (uint32_t)-1;
                break;
            }
            regs->eax = (uint32_t)ipc_try_send(qid, type, data, len);
            break;
        }

        // ----- SYS_IPC_TRY_RECV (28) -----
        case SYS_IPC_TRY_RECV: {
            int qid = (int)regs->ebx;
            uint32_t* type_out = (uint32_t*)regs->ecx;
            void* data_out = (void*)regs->edx;
            uint32_t* len_out = (uint32_t*)regs->esi;
            if ((type_out && !validate_user_ptr(type_out, 4)) ||
                (data_out && !validate_user_ptr(data_out, IPC_MSG_SIZE)) ||
                (len_out && !validate_user_ptr(len_out, 4))) {
                regs->eax = (uint32_t)-1;
                break;
            }
            regs->eax = (uint32_t)ipc_try_receive(qid, NULL, type_out, data_out, len_out);
            break;
        }
        // ----- SYS_SET_STDOUT_IPC (44) -----
        case SYS_SET_STDOUT_IPC: {
            extern int stdout_ipc_qid;
            stdout_ipc_qid = (int)regs->ebx;
            regs->eax = 0;
            break;
        }

    }
    return regs->eax;
}
