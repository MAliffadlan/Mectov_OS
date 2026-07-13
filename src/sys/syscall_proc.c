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
#include "../include/shell.h"

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

uint32_t handle_syscall_proc(registers_t* regs) {
    switch (regs->eax) {
        // ----- SYS_THREAD_CREATE (18) -----
        case SYS_THREAD_CREATE: {
            void (*entry)() = (void (*)())regs->ebx;
            int priority = (int)regs->ecx;
            uint32_t page_dir = (uint32_t)regs->edx;
            int tid = thread_create(entry, priority, page_dir);
            __asm__ volatile("sti"); // thread_create leaves interrupts disabled
            regs->eax = (uint32_t)tid;
            write_serial_string("[SYS] thread_create -> TID=");
            write_serial('0' + tid);
            write_serial('\n');
            break;
        }

        // ----- SYS_SLEEP (19) -----
        case SYS_SLEEP: {
            int ticks = (int)regs->ebx;
            task_sleep(ticks);
            regs->eax = 0;
            break;
        }

        // ----- SYS_GET_PID (20) -----
        case SYS_GET_PID: {
            regs->eax = (uint32_t)get_current_task();
            break;
        }

        // ----- SYS_SET_PRIORITY (21) -----
        case SYS_SET_PRIORITY: {
            int tid = (int)regs->ebx;
            int priority = (int)regs->ecx;
            regs->eax = (uint32_t)task_set_priority(tid, priority);
            break;
        }

        // ----- SYS_GET_PRIORITY (22) -----
        case SYS_GET_PRIORITY: {
            int tid = (int)regs->ebx;
            regs->eax = (uint32_t)task_get_priority(tid);
            break;
        }

        // ----- SYS_GET_TASKS (46) -----
        case SYS_GET_TASKS: {
            sys_task_info_t* array = (sys_task_info_t*)regs->ebx;
            int max_count = (int)regs->ecx;
            if (!validate_user_ptr(array, sizeof(sys_task_info_t) * max_count)) { regs->eax = (uint32_t)-1; break; }
            int count = 0;
            for (int i = 0; i < 64 && count < max_count; i++) { // MAX_TASKS is 64 in task.h
                task_info_t info;
                if (get_task_info(i, &info)) {
                    array[count].id = info.id;
                    array[count].ring = info.ring;
                    array[count].state = info.state;
                    array[count].priority = info.priority;
                    count++;
                }
            }
            regs->eax = count;
            break;
        }

        // ----- SYS_KILL_TASK (48) -----
        case SYS_KILL_TASK: {
            int tid = (int)regs->ebx;
            if (tid <= 0) { regs->eax = (uint32_t)-1; break; }
            regs->eax = (uint32_t)task_kill(tid);
            break;
        }
        // ----- SYS_GET_LAUNCH_ARG (49) -----
        case SYS_GET_LAUNCH_ARG: {
            char* user_buf = (char*)regs->ebx;
            int max_len = (int)regs->ecx;
            if (max_len <= 0 || !validate_user_ptr(user_buf, max_len)) {
                write_serial_string("[LAUNCH_ARG] bad ptr\n");
                regs->eax = (uint32_t)-1; break;
            }
            extern const char* task_get_launch_arg(int tid);
            const char* arg = task_get_launch_arg(get_current_task());
            write_serial_string("[LAUNCH_ARG] tid=");
            write_serial_hex(get_current_task());
            write_serial_string(" arg='");
            write_serial_string(arg);
            write_serial_string("'\n");
            int i = 0;
            for (; i < max_len - 1 && arg[i]; i++) {
                user_buf[i] = arg[i];
            }
            user_buf[i] = '\0';
            regs->eax = (uint32_t)i;
            break;
        }

        // ----- SYS_EXEC_CMD (45) -----
        case SYS_EXEC_CMD: {
            const char* cmd_str = (const char*)regs->ebx;
            if (safe_strlen(cmd_str, CMD_BUF_SIZE) < 0) { regs->eax = (uint32_t)-1; break; }
            extern char cmd_b[CMD_BUF_SIZE];
            extern int b_idx;
            int len2 = 0;
            while (cmd_str[len2] && len2 < CMD_BUF_SIZE - 1) {
                cmd_b[len2] = cmd_str[len2];
                len2++;
            }
            cmd_b[len2] = '\0';
            b_idx = len2;
            extern int stdout_ipc_qid;
            int saved_qid = stdout_ipc_qid;
            extern int use_term_buf;
            extern int term_app_running;
            use_term_buf = 0;
            extern void ex_cmd(void);
            ex_cmd();
            extern void vga_flush_ipc(void);
            vga_flush_ipc();
            stdout_ipc_qid = saved_qid;
            regs->eax = 0;
            break;
        }
    }
    return regs->eax;
}
