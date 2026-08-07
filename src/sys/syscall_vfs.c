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

uint32_t handle_syscall_vfs(registers_t* regs) {
    switch (regs->eax) {
        // ----- SYS_OPEN (2): Open a VFS file/device -----
        case SYS_OPEN: {
            const char* filename = (const char*)regs->ebx;
            if (safe_strlen(filename, MAX_FILENAME) < 0) {
                regs->eax = (uint32_t)-1;
                break;
            }
            regs->eax = (uint32_t)do_sys_open(filename, 0);
            break;
        }

        // ----- SYS_READ (3): Read from an open file/device -----
        case SYS_READ: {
            int fd = (int)regs->ebx;
            char* buf = (char*)regs->ecx;
            int size = (int)regs->edx;

            if (size <= 0 || !validate_user_ptr(buf, size)) {
                regs->eax = (uint32_t)-1; break;
            }

            regs->eax = (uint32_t)do_sys_read(fd, buf, size);
            break;
        }

        // ----- SYS_WRITE (4): Write to an open file/device -----
        case SYS_WRITE: {
            int fd = (int)regs->ebx;
            const char* buf = (const char*)regs->ecx;
            int size = (int)regs->edx;

            if (size <= 0 || !validate_user_ptr(buf, size)) {
                regs->eax = (uint32_t)-1; break;
            }

            int cur_tid = get_current_task();
            extern int task_get_fd(int, int);
            if (task_get_fd(cur_tid, fd) == -1 && (fd == 1 || fd == 2)) {
                // One locked serial write for the whole buffer: byte-by-byte
                // writes would let other CPUs interleave between the bytes and
                // garble every log line (Fase 3 SMP).
                write_serial_buffer(buf, size);
                regs->eax = (uint32_t)size;
            } else {
                regs->eax = (uint32_t)do_sys_write(fd, buf, size);
            }
            break;
        }

        // ----- SYS_CLOSE (5): Close an open file/device -----
        case SYS_CLOSE: {
            int fd = (int)regs->ebx;
            regs->eax = (uint32_t)do_sys_close(fd);
            break;
        }

        // ----- SYS_STAT_FILE (38) -----
        case SYS_STAT_FILE: {
            const char* path = (const char*)regs->ebx;
            if (safe_strlen(path, MAX_PATH) < 0) {
                regs->eax = (uint32_t)-1; break;
            }
            int node = vfs_get_node(path);
            regs->eax = (uint32_t)node;
            break;
        }
        // ----- SYS_LIST_DIR (37) -----
        case SYS_LIST_DIR: {
            dir_entry_t* array = (dir_entry_t*)regs->ebx;
            int max_count = (int)regs->ecx;
            int parent_node = (int)regs->edx;
            if (!validate_user_ptr(array, sizeof(dir_entry_t) * max_count)) {
                regs->eax = (uint32_t)-1; break;
            }
            int count = 0;
            for (int i = 0; i < MAX_NODES && count < max_count; i++) {
                if (fs_nodes[i].in_use && fs_nodes[i].parent == parent_node) {
                    dir_entry_t* e = &array[count];
                    for (int j = 0; j < 31 && fs_nodes[i].name[j]; j++)
                        e->name[j] = fs_nodes[i].name[j];
                    e->name[31] = '\0';
                    // Find null terminator
                    int nlen = 0;
                    while (nlen < 31 && fs_nodes[i].name[nlen]) nlen++;
                    e->name[nlen] = '\0';
                    e->type = (int)fs_nodes[i].type;
                    e->size = fs_nodes[i].size;
                    e->node_idx = i;
                    count++;
                }
            }
            regs->eax = count;
            break;
        }
        case SYS_CREATE_FILE: {
            const char* path = (const char*)regs->ebx;
            if (safe_strlen(path, MAX_FILENAME) < 0) {
                regs->eax = (uint32_t)-1; break;
            }
            write_serial_string("[CREATE_FILE] ");
            write_serial_string(path);
            write_serial('\n');
            extern int vfs_create_file(const char* name);
            int res = vfs_create_file(path);
            regs->eax = (uint32_t)res;
            break;
        }

        // ----- SYS_DELETE_FILE (58) -----
        case SYS_DELETE_FILE: {
            const char* path = (const char*)regs->ebx;
            if (!validate_user_ptr(path, 1) || safe_strlen(path, 256) < 0) {
                regs->eax = (uint32_t)-1;
                break;
            }
            extern int vfs_delete_node(const char* path);
            regs->eax = (uint32_t)vfs_delete_node(path);
            break;
        }

        // ----- SYS_MKDIR (59) -----
        case SYS_MKDIR: {
            const char* path = (const char*)regs->ebx;
            if (!validate_user_ptr(path, 1) || safe_strlen(path, 256) < 0) {
                regs->eax = (uint32_t)-1;
                break;
            }
            extern int vfs_mkdir(const char* path);
            regs->eax = (uint32_t)vfs_mkdir(path);
            break;
        }

        // ----- SYS_RENAME_FILE (60) -----
        case SYS_RENAME_FILE: {
            const char* old_path = (const char*)regs->ebx;
            const char* new_path = (const char*)regs->ecx;
            if (!validate_user_ptr(old_path, 1) || safe_strlen(old_path, 256) < 0 ||
                !validate_user_ptr(new_path, 1) || safe_strlen(new_path, 256) < 0) {
                regs->eax = (uint32_t)-1;
                break;
            }
            extern int vfs_rename(const char* old_path, const char* new_path);
            regs->eax = (uint32_t)vfs_rename(old_path, new_path);
            break;
        }

        // Empty line
        // ----- SYS_PIPE (32) -----
        case SYS_PIPE: {
            int* pipefd = (int*)regs->ebx;
            if (!validate_user_ptr(pipefd, sizeof(int)*2)) {
                regs->eax = (uint32_t)-1;
                break;
            }
            regs->eax = (uint32_t)do_sys_pipe(pipefd);
            break;
        }

    }
    return regs->eax;
}
