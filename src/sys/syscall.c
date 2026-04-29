// ============================================================
// syscall.c — Mectov OS System Call Dispatcher
// ============================================================
// Handles int 0x80 from Ring 3 user programs.
// All kernel services are accessed through this interface.
// ============================================================

#include "../include/syscall.h"
#include "../include/idt.h"
#include "../include/gdt.h"
#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/timer.h"
#include "../include/mem.h"
#include "../include/vfs.h"
#include "../include/keyboard.h"
#include "../include/mouse.h"
#include "../include/serial.h"
#include "../include/wm.h"
#include "../include/apps.h"

// GUI Event structures for Ring 3
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

// Display List for Window Drawing
typedef struct {
    int type; // 1 = rect, 2 = text
    int x, y, w, h;
    uint32_t color;
    char text[32];
} draw_cmd_t;

#define MAX_DRAW_CMDS 128
typedef struct {
    draw_cmd_t cmds[MAX_DRAW_CMDS];
    int count;
    int pending_count;
    draw_cmd_t pending_cmds[MAX_DRAW_CMDS];
} win_canvas_t;

static win_event_queue_t win_queues[MAX_WINDOWS];
static win_canvas_t win_canvases[MAX_WINDOWS];

static int get_win_index(int wid) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm_wins[i].visible && wm_wins[i].id == wid) return i;
    }
    
    // Debug print
    write_serial_string("get_win_index failed. &visible=");
    write_serial_hex((uint32_t)&wm_wins[0].visible);
    write_serial_string(" &id=");
    write_serial_hex((uint32_t)&wm_wins[0].id);
    write_serial('\n');
    return -1;
}

static void push_event(int wid, int type, int x, int y, int key) {
    int idx = get_win_index(wid);
    if (idx < 0) return;
    int t = win_queues[idx].tail;
    int next = (t + 1) % MAX_EVENTS;
    if (next != win_queues[idx].head) {
        win_queues[idx].events[t].type = type;
        win_queues[idx].events[t].x = x;
        win_queues[idx].events[t].y = y;
        win_queues[idx].events[t].key = key;
        win_queues[idx].tail = next;
    }
}

static void win_draw_cb(int id, int cx, int cy, int cw, int ch) {
    (void)cw; (void)ch;
    // Replay Display List
    int idx = get_win_index(id);
    if (idx < 0) return;
    for (int i = 0; i < win_canvases[idx].count; i++) {
        draw_cmd_t* cmd = &win_canvases[idx].cmds[i];
        if (cmd->type == 1) { // rect
            draw_rect(cx + cmd->x, cy + cmd->y, cmd->w, cmd->h, cmd->color);
        } else if (cmd->type == 2) { // text
            draw_string_px(cx + cmd->x, cy + cmd->y, cmd->text, cmd->color, 0);
        }
    }
    // We don't push a Paint event every frame, we let the app decide when to update.
}
static void win_key_cb(int id, char c, uint8_t sc) {
    (void)sc;
    push_event(id, 2, 0, 0, c);
}
static void win_mouse_cb(int id, int cx, int cy, int btn) {
    push_event(id, 3, cx, cy, btn);
}
// ============================================================
// File Descriptor Table (per-system, simple implementation)
// ============================================================
#define MAX_FDS 16
#define FD_FREE   0
#define FD_OPEN   1

typedef struct {
    int state;       // FD_FREE or FD_OPEN
    char path[MAX_PATH]; // File path
    int offset;      // Current read/write position
    char* data;      // In-memory buffer (kmalloc'd)
    int size;        // Current data size
    int capacity;    // Allocated capacity
} fd_entry_t;

static fd_entry_t fd_table[MAX_FDS];

static void fd_init(void) {
    for (int i = 0; i < MAX_FDS; i++) {
        fd_table[i].state = FD_FREE;
        fd_table[i].path[0] = '\0';
        fd_table[i].offset = 0;
        fd_table[i].data = 0;
        fd_table[i].size = 0;
        fd_table[i].capacity = 0;
    }
}

static int fd_alloc(const char* path) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (fd_table[i].state == FD_FREE) {
            fd_table[i].state = FD_OPEN;
            strcpy(fd_table[i].path, path);
            fd_table[i].offset = 0;
            fd_table[i].data = 0;
            fd_table[i].size = 0;
            fd_table[i].capacity = 0;
            return i;
        }
    }
    return -1;
}

static void fd_free(int fd) {
    if (fd >= 0 && fd < MAX_FDS) {
        fd_table[fd].state = FD_FREE;
        fd_table[fd].path[0] = '\0';
        fd_table[fd].offset = 0;
        if (fd_table[fd].data) {
            kfree(fd_table[fd].data);
            fd_table[fd].data = 0;
        }
        fd_table[fd].size = 0;
        fd_table[fd].capacity = 0;
    }
}

// ============================================================
// Pointer Validation
// ============================================================
// For now with identity mapping: validate that the pointer is
// within the mapped RAM range (0 - 128MB). This prevents the
// kernel from following wild pointers into unmapped memory.
// In the future, per-task page tables would allow finer checks.
// ============================================================
#define USER_MEM_LIMIT 0x08000000  // 128MB

static int validate_user_ptr(const void* ptr, uint32_t size) {
    uint32_t addr = (uint32_t)ptr;
    // Check: not NULL, not wrapping, and within mapped memory
    if (addr == 0) return 0;
    if (addr + size < addr) return 0; // overflow check
    if (addr + size > USER_MEM_LIMIT) return 0;
    return 1;
}

// Safe strlen for user strings (max 256 chars to prevent infinite scan)
static int safe_strlen(const char* s, int max) {
    if (!validate_user_ptr(s, 1)) return -1;
    for (int i = 0; i < max; i++) {
        if (s[i] == '\0') return i;
    }
    return max;
}

// ============================================================
// Task exit (declared in task.c)
// ============================================================
extern void task_exit(void);

// ============================================================
// Syscall Handler — dispatches based on EAX register
// ============================================================
static void syscall_handler(registers_t* regs) {
    switch (regs->eax) {

        // ----- SYS_PRINT (1): Print string to terminal -----
        case SYS_PRINT: {
            const char* msg = (const char*)regs->ebx;
            uint8_t color = (uint8_t)regs->ecx;
            if (safe_strlen(msg, 256) < 0) { regs->eax = (uint32_t)-1; break; }
            // Serial marker: user app called SYS_PRINT
            write_serial('P');
            print(msg, color);
            regs->eax = 0;
            break;
        }

        // ----- SYS_OPEN (2): Open a VFS file -----
        case SYS_OPEN: {
            const char* filename = (const char*)regs->ebx;
            if (safe_strlen(filename, MAX_FILENAME) < 0) {
                regs->eax = (uint32_t)-1;
                break;
            }
            if (safe_strlen(filename, MAX_PATH) < 0) { regs->eax = (uint32_t)-1; break; }
            int fd = fd_alloc(filename);
            if (fd < 0) { regs->eax = (uint32_t)-1; break; }
            
            // Pre-load file contents into FD buffer if file exists
            int node = vfs_get_node(filename);
            if (node >= 0 && vfs_is_file(node)) {
                // Allocate a reasonable buffer
                fd_table[fd].data = (char*)kmalloc(MAX_FILE_SIZE);
                if (fd_table[fd].data) {
                    fd_table[fd].capacity = MAX_FILE_SIZE;
                    int sz = vfs_read_file(filename, fd_table[fd].data, MAX_FILE_SIZE - 1);
                    if (sz > 0) {
                        fd_table[fd].data[sz] = '\0';
                        fd_table[fd].size = sz;
                    } else {
                        fd_table[fd].data[0] = '\0';
                        fd_table[fd].size = 0;
                    }
                }
            } else {
                // Create file first
                int res = vfs_create_file(filename);
                (void)res;
                // Also allocate buffer
                fd_table[fd].data = (char*)kmalloc(MAX_FILE_SIZE);
                if (fd_table[fd].data) {
                    fd_table[fd].capacity = MAX_FILE_SIZE;
                    fd_table[fd].data[0] = '\0';
                    fd_table[fd].size = 0;
                }
            }
            regs->eax = (uint32_t)fd;
            break;
        }

        // ----- SYS_READ (3): Read from an open file -----
        case SYS_READ: {
            int fd = (int)regs->ebx;
            char* buf = (char*)regs->ecx;
            int size = (int)regs->edx;

            if (fd < 0 || fd >= MAX_FDS || fd_table[fd].state != FD_OPEN) {
                regs->eax = (uint32_t)-1; break;
            }
            if (size <= 0 || !validate_user_ptr(buf, size)) {
                regs->eax = (uint32_t)-1; break;
            }

            if (!fd_table[fd].data) {
                regs->eax = 0; break;
            }
            int off = fd_table[fd].offset;
            int avail = fd_table[fd].size - off;
            if (avail <= 0) { regs->eax = 0; break; }
            int to_read = (size < avail) ? size : avail;
            memcpy(buf, fd_table[fd].data + off, to_read);
            fd_table[fd].offset += to_read;
            regs->eax = (uint32_t)to_read;
            break;
        }

        // ----- SYS_WRITE (4): Write to an open file -----
        case SYS_WRITE: {
            int fd = (int)regs->ebx;
            const char* buf = (const char*)regs->ecx;
            int size = (int)regs->edx;

            if (fd < 0 || fd >= MAX_FDS || fd_table[fd].state != FD_OPEN) {
                regs->eax = (uint32_t)-1; break;
            }
            if (size <= 0 || !validate_user_ptr(buf, size)) {
                regs->eax = (uint32_t)-1; break;
            }

            if (!fd_table[fd].data) {
                fd_table[fd].data = (char*)kmalloc(MAX_FILE_SIZE);
                if (!fd_table[fd].data) { regs->eax = 0; break; }
                fd_table[fd].capacity = MAX_FILE_SIZE;
                fd_table[fd].data[0] = '\0';
                fd_table[fd].size = 0;
            }
            int off = fd_table[fd].offset;
            int space = fd_table[fd].capacity - off;
            if (space <= 0) { regs->eax = 0; break; }
            int to_write = (size < space) ? size : space;
            memcpy(fd_table[fd].data + off, buf, to_write);
            fd_table[fd].offset += to_write;
            if (fd_table[fd].offset > fd_table[fd].size) {
                fd_table[fd].size = fd_table[fd].offset;
            }
            fd_table[fd].data[fd_table[fd].size] = '\0';
            
            // Persist to VFS
            vfs_write_file(fd_table[fd].path, fd_table[fd].data, fd_table[fd].size);
            regs->eax = (uint32_t)to_write;
            break;
        }

        // ----- SYS_CLOSE (5): Close an open file -----
        case SYS_CLOSE: {
            int fd = (int)regs->ebx;
            if (fd < 0 || fd >= MAX_FDS || fd_table[fd].state != FD_OPEN) {
                regs->eax = (uint32_t)-1; break;
            }
            // If there's data, flush to VFS on close
            if (fd_table[fd].data && fd_table[fd].size > 0) {
                vfs_write_file(fd_table[fd].path, fd_table[fd].data, fd_table[fd].size);
            }
            fd_free(fd);
            regs->eax = 0;
            break;
        }

        // ----- SYS_MALLOC (6): Allocate memory -----
        case SYS_MALLOC: {
            uint32_t size = (uint32_t)regs->ebx;
            if (size == 0 || size > 0x100000) { // Max 1MB per alloc
                regs->eax = 0; break;
            }
            void* ptr = kmalloc(size);
            regs->eax = (uint32_t)ptr;
            break;
        }

        // ----- SYS_FREE (7): Free memory -----
        case SYS_FREE: {
            void* ptr = (void*)regs->ebx;
            if (validate_user_ptr(ptr, 1)) {
                kfree(ptr);
            }
            regs->eax = 0;
            break;
        }

        // ----- SYS_GET_TICKS (8): Get timer ticks -----
        case SYS_GET_TICKS: {
            regs->eax = get_ticks();
            break;
        }

        // ----- SYS_YIELD (9): Yield CPU -----
        case SYS_YIELD: {
            // Timer IRQ handles scheduling; just return
            regs->eax = 0;
            break;
        }

        // ----- SYS_EXIT (10): Terminate current task -----
        case SYS_EXIT: {
            write_serial('X'); // Serial marker: task exit
            write_serial_string("SYS_EXIT\n");
            
            extern int term_app_running;
            extern int term_app_task_id;
            extern int get_current_task(void);
            
            if (term_app_running && get_current_task() == term_app_task_id) {
                term_app_running = 0;
                term_app_task_id = -1;
                print("root@mectov:~# ", 0x0A);
            } else {
                print("[syscall] Task exited.\n", 0x0E);
            }
            task_exit();
            // task_exit never returns, but just in case:
            for(;;) __asm__("hlt");
            break;
        }

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
            if (safe_strlen(text, 31) < 0) { regs->eax = (uint32_t)-1; break; }
            
            int idx = get_win_index(wid);
            if (idx >= 0) {
                if (win_canvases[idx].pending_count < MAX_DRAW_CMDS) {
                    draw_cmd_t* cmd = &win_canvases[idx].pending_cmds[win_canvases[idx].pending_count++];
                    cmd->type = 2; cmd->x = x; cmd->y = y; cmd->color = color;
                    int i = 0;
                    while (text[i] && i < 31) { cmd->text[i] = text[i]; i++; }
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
            
            uint8_t sc = 0;
            if (term_app_running && get_current_task() == term_app_task_id) {
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
            
            write_serial_string("SYS_CREATE_WINDOW returned ");
            write_serial('0' + (wid / 10));
            write_serial('0' + (wid % 10));
            write_serial('\n');

            int idx = get_win_index(wid);
            if (idx >= 0) {
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

        // ----- SYS_GET_EVENT (16) -----
        case SYS_GET_EVENT: {
            int wid = (int)regs->ebx;
            gui_event_t* ev_ptr = (gui_event_t*)regs->ecx;
            if (!validate_user_ptr(ev_ptr, sizeof(gui_event_t))) { regs->eax = (uint32_t)-1; break; }
            
            int idx = get_win_index(wid);
            if (idx >= 0) {
                int h = win_queues[idx].head;
                if (h != win_queues[idx].tail) {
                    *ev_ptr = win_queues[idx].events[h];
                    win_queues[idx].head = (h + 1) % MAX_EVENTS;
                    regs->eax = 1; // Success
                } else {
                    regs->eax = 0; // No events
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
                for (int i = 0; i < win_canvases[idx].pending_count; i++) {
                    win_canvases[idx].cmds[i] = win_canvases[idx].pending_cmds[i];
                }
                win_canvases[idx].count = win_canvases[idx].pending_count;
                write_serial_string("UPD_WIN ");
                write_serial('0' + win_canvases[idx].count);
                write_serial('\n');
                win_canvases[idx].pending_count = 0; // Reset for next frame
            } else {
                write_serial_string("UPD_WIN: Invalid WID\n");
            }
            regs->eax = 0;
            break;
        }

        default:
            regs->eax = (uint32_t)-1; // Unknown syscall
            break;
    }
}

// ============================================================
// Init
// ============================================================
void init_syscalls(void) {
    fd_init();
    register_interrupt_handler(0x80, syscall_handler);
}

// ============================================================
// Switch from Ring 0 to Ring 3
// ============================================================
void switch_to_user_mode(void) {
    extern uint32_t stack_top;
    tss_set_kernel_stack((uint32_t)&stack_top);

    __asm__ volatile(
        "cli\n"
        "mov $0x23, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%esp, %%eax\n"
        "pushl $0x23\n"
        "pushl %%eax\n"
        "pushf\n"
        "pop %%eax\n"
        "or $0x200, %%eax\n"
        "push %%eax\n"
        "pushl $0x1B\n"
        "push $1f\n"
        "iret\n"
        "1:\n"
        ::: "eax", "memory"
    );
}
