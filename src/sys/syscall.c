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
#include "../include/ipc.h"
#include "../include/vmm.h"
#include "../include/task.h"
#include "../include/fd.h"

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
// (Legacy simple FD table removed, moved to fd.c)

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

            regs->eax = (uint32_t)do_sys_write(fd, buf, size);
            break;
        }

        // ----- SYS_CLOSE (5): Close an open file/device -----
        case SYS_CLOSE: {
            int fd = (int)regs->ebx;
            regs->eax = (uint32_t)do_sys_close(fd);
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
            // Halt the CPU with interrupts enabled so the timer IRQ 
            // will immediately fire and switch tasks.
            __asm__ __volatile__("sti\n\thlt\n\tcli");
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
                print("root@mectov", 0x0A);
                print(" ~$ ", 0x0F);
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
                // Window is closed/invalid. Kill the task so it doesn't become a zombie
                // spinning in a while(sys_get_event) loop.
                write_serial_string("SYS_GET_EVENT: Invalid WID. Killing task.\n");
                
                // Clean up terminal state if this was a terminal-launched app
                extern int term_app_running;
                extern int term_app_task_id;
                extern int get_current_task(void);
                if (term_app_running && get_current_task() == term_app_task_id) {
                    term_app_running = 0;
                    term_app_task_id = -1;
                    print("\n", 0x0F);
                    print("root@mectov", 0x0A);
                    print(" ~$ ", 0x0F);
                }
                
                task_exit();
                for(;;) __asm__("hlt");
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

        // ===== NEW: Thread & Process Management =====
        
        // ----- SYS_THREAD_CREATE (18) -----
        case SYS_THREAD_CREATE: {
            void (*entry)() = (void (*)())regs->ebx;
            int priority = (int)regs->ecx;
            uint32_t page_dir = (uint32_t)regs->edx;
            int tid = thread_create(entry, priority, page_dir);
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

        // ===== NEW: IPC =====

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

        // ===== NEW: Virtual Memory =====

        // ----- SYS_VMM_MAP (29) -----
        case SYS_VMM_MAP: {
            uint32_t vaddr = (uint32_t)regs->ebx;
            uint32_t paddr = (uint32_t)regs->ecx;
            uint32_t flags = (uint32_t)regs->edx;
            // Get current task's page dir
            uint32_t pd = task_get_page_dir(get_current_task());
            regs->eax = (uint32_t)vmm_map_page(pd, vaddr, paddr, flags);
            break;
        }

        // ----- SYS_VMM_ALLOC (30) -----
        case SYS_VMM_ALLOC: {
            uint32_t vaddr = (uint32_t)regs->ebx;
            uint32_t flags = (uint32_t)regs->ecx;
            uint32_t pd = task_get_page_dir(get_current_task());
            regs->eax = vmm_alloc_page_at(pd, vaddr, flags);
            break;
        }

        // ----- SYS_VMM_FREE (31) -----
        case SYS_VMM_FREE: {
            uint32_t vaddr = (uint32_t)regs->ebx;
            uint32_t pd = task_get_page_dir(get_current_task());
            regs->eax = (uint32_t)vmm_unmap_page(pd, vaddr);
            break;
        }

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
