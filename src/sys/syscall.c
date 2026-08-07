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
#include "../include/speaker.h"
#include "../include/rtc.h"
#include "../include/net.h"
#include "../include/shell.h"
#include "../include/spinlock.h"

// SMP: Ring-3 apps run on APs while the BSP GUI loop replays their display
// lists. The struct copies in SYS_UPDATE_WINDOW and the replay in
// win_draw_cb() are NOT atomic — without this lock, a torn copy mid-replay
// drew text with mixed coordinates/content (overlapping "ghost" glyphs).
spinlock_t gui_canvas_lock = SPINLOCK_INIT;

// Interrupt-safe GUI lock helpers. This OS's scheduler can preempt a task
// between lock-take and unlock (see task.c:81 — "Disable interrupts BEFORE
// taking the lock to prevent scheduler deadlocks!"). A lock holder must be
// non-preemptible, or the main loop can spin forever on a lock whose holder
// will only run again when the main loop itself schedules it.
void gui_lock(void) {
    __asm__ volatile("cli");
    spin_lock(&gui_canvas_lock);
}
void gui_unlock(void) {
    spin_unlock(&gui_canvas_lock);
    __asm__ volatile("sti");
}

extern int validate_user_ptr(const void* ptr, uint32_t size);

// Global IPC queue ID for stdout redirection (Terminal Ring 3)
int stdout_ipc_qid = 0;

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
    char text[128]; // Expanded from 32 to 128 to support full-length terminal rows
} draw_cmd_t;

#define MAX_DRAW_CMDS 512 // Expanded from 128 to 512 to prevent drawing command exhaustion on heavy terminals
typedef struct {
    draw_cmd_t cmds[MAX_DRAW_CMDS];
    int count;
    int pending_count;
    draw_cmd_t pending_cmds[MAX_DRAW_CMDS];
} win_canvas_t;

win_event_queue_t win_queues[MAX_WINDOWS];
win_canvas_t win_canvases[MAX_WINDOWS];

int get_win_index(int wid) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm_wins[i].visible && wm_wins[i].id == wid) return i;
    }
    
    // Debug print
    write_serial_string("get_win_index failed for wid=");
    write_serial('0' + (wid / 10));
    write_serial('0' + (wid % 10));
    write_serial('\n');
    for (int i = 0; i < MAX_WINDOWS; i++) {
        write_serial_string("Slot ");
        write_serial('0' + i);
        write_serial_string(": vis=");
        write_serial('0' + wm_wins[i].visible);
        write_serial_string(" id=");
        write_serial('0' + (wm_wins[i].id / 10));
        write_serial('0' + (wm_wins[i].id % 10));
        write_serial('\n');
    }
    return -1;
}

void push_event(int wid, int type, int x, int y, int key) {
    int idx = get_win_index(wid);
    if (idx < 0) return;
    gui_lock();
    int t = win_queues[idx].tail;
    int next = (t + 1) % MAX_EVENTS;
    if (next != win_queues[idx].head) {
        win_queues[idx].events[t].type = type;
        win_queues[idx].events[t].x = x;
        win_queues[idx].events[t].y = y;
        win_queues[idx].events[t].key = key;
        win_queues[idx].tail = next;
    }
    gui_unlock();
}

void win_draw_cb(int id, int cx, int cy, int cw, int ch) {
    (void)cw; (void)ch;
    // Replay Display List
    int idx = get_win_index(id);
    if (idx < 0) return;
    gui_lock();
    for (int i = 0; i < win_canvases[idx].count; i++) {
        draw_cmd_t* cmd = &win_canvases[idx].cmds[i];
        if (cmd->type == 1) { // rect
            draw_rect(cx + cmd->x, cy + cmd->y, cmd->w, cmd->h, cmd->color);
        } else if (cmd->type == 2) { // text
            draw_string_px(cx + cmd->x, cy + cmd->y, cmd->text, cmd->color, 0xFFFFFFFF);
        }
    }
    gui_unlock();
    // We don't push a Paint event every frame, we let the app decide when to update.
}
void win_key_cb(int id, char c, uint8_t sc) {
    if (keyboard_ctrl_held && c >= 'a' && c <= 'z') {
        push_event(id, 2, 0, 0, c - 'a' + 1);
    } else if (keyboard_ctrl_held && c >= 'A' && c <= 'Z') {
        push_event(id, 2, 0, 0, c - 'A' + 1);
    } else if (c == 0 && sc > 0) {
        push_event(id, 2, 0, 0, 0xE000 | sc);
    } else {
        push_event(id, 2, 0, 0, c);
    }
}
void win_mouse_cb(int id, int cx, int cy, int btn) {
    if (btn == 0x10) {
        // Scroll up
        push_event(id, 4, cx, cy, 1);
    } else if (btn == 0x20) {
        // Scroll down
        push_event(id, 4, cx, cy, -1);
    } else {
        push_event(id, 3, cx, cy, btn);
    }
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
#define USER_MEM_LIMIT 0x10000000  // only used for kernel-task callers now

// No legitimate syscall buffer is this big. Rejecting early also neutralises
// the `sizeof(T) * max_count` overflow bypasses in the array-returning
// syscalls, and keeps the page walk below bounded.
#define USER_PTR_MAX_SIZE 0x1000000  // 16MB

// Confirm one page is mapped user-accessible in this address space, faulting it
// in first if it is merely un-touched heap.
//
// The demand pager in idt.c only runs for faults taken at CPL 3. If the kernel
// is the first to touch a freshly sys_malloc'd page — the ordinary
// `read(fd, malloc(n), n)` idiom — the fault arrives with CS=0x08 and becomes
// an unrecoverable panic. So materialise the page here, at validation time,
// while we can still fail cleanly and return -1 to the caller.
static int user_page_ok(uint32_t pd_paddr, uint32_t va, uint32_t heap_top) {
    uint32_t* pd = (uint32_t*)(uintptr_t)(pd_paddr & 0xFFFFF000);
    uint32_t pde = pd[va >> 22];

    if ((pde & (PAGE_PRESENT | PAGE_USER)) == (PAGE_PRESENT | PAGE_USER)) {
        uint32_t* pt = (uint32_t*)(uintptr_t)(pde & 0xFFFFF000);
        uint32_t pte = pt[(va >> 12) & 0x3FF];
        // PAGE_USER is required at both levels; a COW page is fine here, the
        // fault handler in idt.c copies it when the write actually happens.
        if ((pte & (PAGE_PRESENT | PAGE_USER)) == (PAGE_PRESENT | PAGE_USER)) return 1;
    }

    // Not mapped. The only addresses we will fault in on the caller's behalf
    // are inside its own reserved heap window.
    if (va < 0x08000000 || va >= heap_top) return 0;

    uint32_t phys = frame_alloc();
    if (phys == 0) return 0;
    memset((void*)(uintptr_t)phys, 0, 4096);  // zero via the kernel identity map
    if (vmm_map_page(pd_paddr, va, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER) != 0) {
        frame_free(phys);
        return 0;
    }
    __asm__ __volatile__("invlpg (%0)" : : "r"(va));
    return 1;
}

static int validate_user_array_ptr(const void* ptr, uint32_t elem_size, int count) {
    if (count <= 0 || elem_size == 0) return 0;
    uint64_t total = (uint64_t)elem_size * (uint64_t)count;
    if (total == 0 || total > USER_PTR_MAX_SIZE) return 0;
    return validate_user_ptr(ptr, (uint32_t)total);
}

// Is [ptr, ptr+size) memory the CALLER is allowed to hand us?
//
// This used to compare the pointer against a 256MB constant, which accepted
// every kernel address in the identity map — so any syscall taking an output
// buffer could be aimed at kernel .text, the task table, or a page directory.
// Now it walks the caller's own page directory and demands PAGE_USER on every
// page of the range.
int validate_user_ptr(const void* ptr, uint32_t size) {
    uint32_t addr = (uint32_t)ptr;
    if (addr == 0) return 0;
    if (size == 0) size = 1;
    if (size > USER_PTR_MAX_SIZE) return 0;
    if (addr + size < addr) return 0;  // wrap

    int tid = get_current_task();

    // Kernel tasks run on the global identity map, where there is no user/kernel
    // split to enforce and every caller is already Ring 0 code. Keep the old
    // constant bound for them.
    if (task_in_kernel_space(tid)) return (addr + size) <= USER_MEM_LIMIT;

    uint32_t pd = task_get_page_dir(tid);
    uint32_t heap_top = task_get_heap_ptr(tid);
    uint32_t last = (addr + size - 1) & 0xFFFFF000;
    for (uint32_t va = addr & 0xFFFFF000; ; va += 4096) {
        if (!user_page_ok(pd, va, heap_top)) return 0;
        if (va == last) break;
    }
    return 1;
}

// Safe strlen for user strings (bounded by max to prevent an infinite scan).
//
// Validates page by page as it walks rather than only checking the first byte:
// an unterminated string can run off the end of the caller's mapped region, and
// now that the kernel identity map is no longer user-accessible there is real
// unmapped memory to run into. A fault here arrives at CPL 0 and panics.
int safe_strlen(const char* s, int max) {
    if (max <= 0) return -1;
    uint32_t addr = (uint32_t)s;
    for (int i = 0; i < max; i++) {
        if (i == 0 || (((addr + (uint32_t)i) & 0xFFF) == 0)) {
            if (!validate_user_ptr((const void*)(uintptr_t)(addr + (uint32_t)i), 1)) return -1;
        }
        if (s[i] == '\0') return i;
    }
    // No NUL byte found inside the validated range: never hand an unterminated
    // pointer to kernel string code, or the caller would walk off the user's
    // mapped pages at CPL0 and panic the kernel. Signal failure instead.
    return -1;
}

// ============================================================
// Task exit (declared in task.c)
// ============================================================
extern void task_exit(void);

// ============================================================
// Syscall Handler — dispatches based on EAX register
// ============================================================
extern uint32_t handle_syscall_vfs(registers_t* regs);
extern uint32_t handle_syscall_gui(registers_t* regs);
extern uint32_t handle_syscall_net(registers_t* regs);
extern uint32_t handle_syscall_proc(registers_t* regs);
extern uint32_t handle_syscall_ipc(registers_t* regs);

static void syscall_handler(registers_t* regs) {
    // The 0x80 gate is a TRAP gate now (isr128 sti's before calling us), so
    // handlers run with IF=1 by default. Most of them touch shared state
    // (VFS nodes, WM windows, fd table, event queues) that is not locked — a
    // timer interrupt in the middle of, say, SYS_GET_EVENT lets the keyboard
    // IRQ push into the same queue and drop keystrokes. So default to
    // non-preemptible (IF=0) exactly like the old interrupt gate; blocking
    // syscalls (SYS_SLEEP / SYS_WAITPID / SYS_YIELD) re-enable IF themselves
    // and stay genuinely preemptible while parked.
    __asm__ volatile("cli");

    uint32_t call_no = regs->eax;   // captured before handlers overwrite eax
    switch (regs->eax) {
        case SYS_OPEN:
        case SYS_READ:
        case SYS_WRITE:
        case SYS_CLOSE:
        case SYS_STAT_FILE:
        case SYS_LIST_DIR:
        case SYS_CREATE_FILE:
        case SYS_DELETE_FILE:
        case SYS_MKDIR:
        case SYS_RENAME_FILE:
        case SYS_PIPE:
            regs->eax = handle_syscall_vfs(regs);
            break;

        case SYS_DRAW_RECT:
        case SYS_DRAW_TEXT:
        case SYS_GET_KEY:
        case SYS_GET_MOUSE:
        case SYS_CREATE_WINDOW:
        case SYS_GET_EVENT:
        case SYS_UPDATE_WINDOW:
            regs->eax = handle_syscall_gui(regs);
            break;

        case SYS_DNS_RESOLVE:
        case SYS_TCP_CONNECT:
        case SYS_TCP_SEND:
        case SYS_TCP_RECV:
        case SYS_TCP_CLOSE:
        case SYS_TCP_LISTEN:
        case SYS_NET_STATUS:
        case SYS_UDP_BIND:
        case SYS_UDP_SEND:
        case SYS_UDP_RECV:
            regs->eax = handle_syscall_net(regs);
            break;

        case SYS_THREAD_CREATE:
        case SYS_SLEEP:
        case SYS_GET_PID:
        case SYS_SET_PRIORITY:
        case SYS_GET_PRIORITY:
        case SYS_GET_TASKS:
        case SYS_KILL_TASK:
        case SYS_GET_LAUNCH_ARG:
        case SYS_EXEC_CMD:
        case SYS_FORK:
        case SYS_WAITPID:
        case SYS_KILL:
        case SYS_SIGNAL:
        case SYS_SIGRETURN:
        case SYS_GETPPID:
        case SYS_EXEC:
        case SYS_SIGACTION:
        case SYS_SIGPROCMASK:
            regs->eax = handle_syscall_proc(regs);
            break;

        case SYS_IPC_CREATE:
        case SYS_IPC_SEND:
        case SYS_IPC_RECV:
        case SYS_IPC_DESTROY:
        case SYS_IPC_TRY_SEND:
        case SYS_IPC_TRY_RECV:
        case SYS_SET_STDOUT_IPC:
            regs->eax = handle_syscall_ipc(regs);
            break;

        // ----- Synchronization: semaphores & futexes (sync.c) -----
        case SYS_SEM_CREATE: {
            extern int sem_create(int);
            regs->eax = (uint32_t)sem_create((int)regs->ebx);
            break;
        }
        case SYS_SEM_WAIT: {
            extern int sem_wait(int);
            regs->eax = (uint32_t)sem_wait((int)regs->ebx);
            break;
        }
        case SYS_SEM_POST: {
            extern int sem_post(int);
            regs->eax = (uint32_t)sem_post((int)regs->ebx);
            break;
        }
        case SYS_SEM_DESTROY: {
            extern int sem_destroy(int);
            regs->eax = (uint32_t)sem_destroy((int)regs->ebx);
            break;
        }
        case SYS_FUTEX_WAIT: {
            extern int futex_wait(uint32_t, uint32_t);
            regs->eax = (uint32_t)futex_wait((uint32_t)regs->ebx, (uint32_t)regs->ecx);
            break;
        }
        case SYS_FUTEX_WAKE: {
            extern int futex_wake(uint32_t, int);
            regs->eax = (uint32_t)futex_wake((uint32_t)regs->ebx, (int)regs->ecx);
            break;
        }

        // ----- SYS_PRINT (1): Print string to terminal -----
        case SYS_PRINT: {
            const char* msg = (const char*)regs->ebx;
            uint8_t color = (uint8_t)regs->ecx;
            if (safe_strlen(msg, 256) < 0) { regs->eax = (uint32_t)-1; break; }
            write_serial_string(msg);
            print(msg, color);
            regs->eax = 0;
            break;
        }
        // ----- SYS_MALLOC (6): Allocate memory -----
        case SYS_MALLOC: {
            uint32_t size = (uint32_t)regs->ebx;
            if (size == 0 || size > 0x1000000) { // Max 16MB per alloc
                regs->eax = 0; break;
            }
            
            extern uint32_t task_get_page_dir(int tid);
            extern uint32_t task_get_heap_ptr(int tid);
            extern void task_set_heap_ptr(int tid, uint32_t ptr);
            
            int tid = get_current_task();
            uint32_t pd = task_get_page_dir(tid);
            
            if (pd == 0) {
                // Kernel task (identity map), use kmalloc
                void* ptr = kmalloc(size);
                regs->eax = (uint32_t)ptr;
            } else {
                // Ring 3 user task: Demand Paging
                uint32_t current_heap = task_get_heap_ptr(tid);
                uint32_t start_addr = current_heap;
                uint32_t end_addr = start_addr + size;
                
                // Align to 4KB for next allocation
                if (end_addr % 4096 != 0) {
                    end_addr = (end_addr + 4095) & 0xFFFFF000;
                }
                
                // Prevent overflow, wrapping, or collision with libraries at 0x09000000
                if (end_addr < start_addr || end_addr > 0x08F00000) { // Limit to ~15MB (before 0x09000000)
                    regs->eax = 0;
                } else {
                    task_set_heap_ptr(tid, end_addr);
                    regs->eax = start_addr;
                }
            }
            break;
        }

        // ----- SYS_FREE (7): Free memory -----
        case SYS_FREE: {
            void* ptr = (void*)regs->ebx;
            // A Ring 3 task's memory never came from kmalloc — SYS_MALLOC just
            // bumps its heap_ptr and the demand pager backs it, and the whole
            // address space is reclaimed by task_cleanup(). Passing a user
            // pointer to kfree() only ever corrupted the kernel free list, so
            // this is a no-op for anything with a private address space.
            if (task_in_kernel_space(get_current_task()) && validate_user_ptr(ptr, 1)) {
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
            __asm__ __volatile__("sti\n\thlt\n\tcli");
            regs->eax = 0;
            break;
        }
        // ----- SYS_EXIT (10): Terminate current task with an exit status -----
        case SYS_EXIT: {
            int tid = get_current_task();
            int code = (int)regs->ebx;
            write_serial_string("[SYSCALL] SYS_EXIT code=");
            write_serial_hex(code);
            write_serial_string(" from TID=");
            write_serial('0' + (tid / 10));
            write_serial('0' + (tid % 10));
            write_serial('\n');
            extern void task_exit_with_code(int code);
            task_exit_with_code(code);
            for(;;) __asm__("hlt");
            break;
        }

        // ----- SYS_GET_TIME (33) -----
        case SYS_GET_TIME: {
            rtc_time_t* out_time = (rtc_time_t*)regs->ebx;
            if (!validate_user_ptr(out_time, sizeof(rtc_time_t))) {
                regs->eax = (uint32_t)-1;
                break;
            }
            *out_time = rtc_read_time();
            regs->eax = 0;
            break;
        }

        // ----- SYS_PLAY_SOUND (34) -----
        case SYS_PLAY_SOUND: {
            regs->eax = 0;
            break;
        }
        // ----- SYS_GET_SYSINFO (35) -----
        case SYS_GET_SYSINFO: {
            sysinfo_t* info = (sysinfo_t*)regs->ebx;
            if (validate_user_ptr(info, sizeof(sysinfo_t))) {
                extern volatile uint32_t timer_ticks;
                info->uptime_ms = timer_ticks;
                info->total_ram_kb = get_total_memory() / 1024;
                info->used_ram_kb = get_used_memory() / 1024;
                info->fb_width = fb_width;
                info->fb_height = fb_height;
                info->fb_bpp = fb_bpp;
                extern char cpu_brand[49];
                for(int i=0; i<48; i++) info->cpu_brand[i] = cpu_brand[i];
                extern uint8_t rtl_mac[6];
                for(int i=0; i<6; i++) info->mac_addr[i] = rtl_mac[i];
                regs->eax = 0;
            } else {
                regs->eax = -1;
            }
            break;
        }

        // ----- Shared memory (78-81) -----
        case SYS_SHMGET: {
            extern int shm_get(uint32_t key, uint32_t size);
            regs->eax = (uint32_t)shm_get((uint32_t)regs->ebx, (uint32_t)regs->ecx);
            break;
        }
        case SYS_SHMAT: {
            extern uint32_t shm_at(int shmid);
            regs->eax = shm_at((int)regs->ebx);
            break;
        }
        case SYS_SHMDT: {
            extern int shm_dt(uint32_t addr);
            regs->eax = (uint32_t)shm_dt((uint32_t)regs->ebx);
            break;
        }
        case SYS_SHMCTL: {
            extern int shm_ctl(int shmid, int cmd);
            regs->eax = (uint32_t)shm_ctl((int)regs->ebx, (int)regs->ecx);
            break;
        }

        // ----- SYS_MMAP (82) / SYS_MUNMAP (83) -----
        case SYS_MMAP: {
            extern uint32_t task_mmap_reserve(uint32_t size);
            regs->eax = task_mmap_reserve((uint32_t)regs->ebx);
            break;
        }
        case SYS_MUNMAP: {
            extern uint32_t task_munmap(uint32_t addr);
            regs->eax = task_munmap((uint32_t)regs->ebx);
            break;
        }
        // ----- SYS_DUP2 (84): duplicate a file descriptor -----
        case SYS_DUP2: {
            extern int do_sys_dup2(int oldfd, int newfd);
            regs->eax = (uint32_t)do_sys_dup2((int)regs->ebx, (int)regs->ecx);
            break;
        }

        // ----- SYS_GET_PCI_INFO (36) -----
        case SYS_GET_PCI_INFO: {
            pci_device_t* array = (pci_device_t*)regs->ebx;
            int max_count = (int)regs->ecx;
            if (validate_user_array_ptr(array, sizeof(pci_device_t), max_count)) {
                int count = (pci_device_count < max_count) ? pci_device_count : max_count;
                for (int i = 0; i < count; i++) {
                    array[i] = pci_devices[i];
                }
                regs->eax = count;
            } else {
                regs->eax = -1;
            }
            break;
        }

        // ----- SYS_GET_WINDOWS (47) -----
        case SYS_GET_WINDOWS: {
            sys_win_info_t* array = (sys_win_info_t*)regs->ebx;
            int max_count = (int)regs->ecx;
            if (!validate_user_array_ptr(array, sizeof(sys_win_info_t), max_count)) { regs->eax = (uint32_t)-1; break; }
            int count = 0;
            for (int i = 0; i < MAX_WINDOWS && count < max_count; i++) {
                if (wm_wins[i].id >= 0 && wm_wins[i].visible) {
                    array[count].id = wm_wins[i].id;
                    array[count].owner_ring = wm_wins[i].owner_ring;
                    array[count].visible = wm_wins[i].visible;
                    array[count].minimized = wm_wins[i].minimized;
                    int j = 0;
                    while (wm_wins[i].title[j] && j < 31) {
                        array[count].title[j] = wm_wins[i].title[j];
                        j++;
                    }
                    array[count].title[j] = '\0';
                    count++;
                }
            }
            regs->eax = count;
            break;
        }

        // ----- SYS_LOAD_LIBRARY (51) -----
        case SYS_LOAD_LIBRARY: {
            const char* lib_name = (const char*)regs->ebx;
            if (safe_strlen(lib_name, MAX_FILENAME) < 0) {
                regs->eax = 0; break;
            }
            extern void* load_mct_library(const char* filename);
            regs->eax = (uint32_t)load_mct_library(lib_name);
            break;
        }
        // ===== Volume Control =====
        case SYS_SET_VOLUME: {
            extern void sb16_set_volume(uint8_t vol);
            sb16_set_volume((uint8_t)regs->ebx);
            regs->eax = 0;
            break;
        }
        case SYS_GET_VOLUME: {
            extern uint8_t sb16_get_volume(void);
            regs->eax = (uint32_t)sb16_get_volume();
            break;
        }
        case SYS_PLAY_WAV: {
            // Play a user-supplied PCM buffer through the SB16. The DMA IRQ
            // (fill_buffer) runs in interrupt context and reads the sample data
            // while ANY task may be active, so the app's user-space pointer is
            // copied into a kernel heap buffer first — reading it from the IRQ
            // under a different CR3 would fault or play garbage.
            const uint8_t* pcm = (const uint8_t*)regs->ebx;
            uint32_t len = (uint32_t)regs->ecx;
            uint16_t rate = (uint16_t)regs->edx;
            if (len == 0 || len > 2 * 1024 * 1024 || !validate_user_ptr(pcm, len)) {
                regs->eax = (uint32_t)-1;
                break;
            }
            extern void sb16_set_audio_buffer(uint8_t*, uint32_t);
            extern void sb16_start_playback(uint16_t);
            static uint8_t* play_buf = NULL;
            extern void* kmalloc(uint32_t);
            extern void kfree(void*);
            uint8_t* new_buf = (uint8_t*)kmalloc(len);
            if (!new_buf) { regs->eax = (uint32_t)-1; break; }
            memcpy(new_buf, pcm, len);
            // Point the IRQ at the new copy FIRST, then free the old one: an
            // SB16 IRQ can fire between these statements, and fill_buffer must
            // never see a freed buffer.
            sb16_set_audio_buffer(new_buf, len);
            kfree(play_buf);          // free previous copy (NULL-safe)
            play_buf = new_buf;
            sb16_start_playback(rate);
            regs->eax = 0;
            break;
        }
        case SYS_STOP_WAV: {
            extern void sb16_stop_playback(void);
            sb16_stop_playback();
            regs->eax = 0;
            break;
        }
        // ----- SYS_VMM_MAP (29) -----
        // REMOVED. This took a raw physical address and raw PTE flags straight
        // from Ring 3 and installed them, which is a complete bypass of every
        // other check in this file: an app could map kernel .text, another
        // task's page directory, or LAPIC MMIO into its own address space as
        // user-writable, or forge a PAGE_COW entry to drive the fault handler
        // out of bounds. Nothing in the tree uses it — SYS_MALLOC plus the
        // demand pager already covers user allocation.
        case SYS_VMM_MAP:
            regs->eax = (uint32_t)-1;
            break;

        // ----- SYS_VMM_ALLOC (30) -----
        // Kept, but the caller no longer picks the flags and cannot name an
        // address outside its own user window.
        case SYS_VMM_ALLOC: {
            uint32_t vaddr = (uint32_t)regs->ebx & 0xFFFFF000;
            int tid = get_current_task();
            if (task_in_kernel_space(tid) || vaddr < 0x08000000 || vaddr >= USER_STACK_BOTTOM) {
                regs->eax = 0;
                break;
            }
            regs->eax = vmm_alloc_page_at(task_get_page_dir(tid), vaddr,
                                          PAGE_PRESENT | PAGE_RW | PAGE_USER);
            break;
        }

        // ----- SYS_VMM_FREE (31) -----
        case SYS_VMM_FREE: {
            uint32_t vaddr = (uint32_t)regs->ebx & 0xFFFFF000;
            int tid = get_current_task();
            if (task_in_kernel_space(tid) || vaddr < 0x08000000 || vaddr >= USER_STACK_BOTTOM) {
                regs->eax = (uint32_t)-1;
                break;
            }
            uint32_t pd = task_get_page_dir(tid);
            // vmm_unmap_page() frees the PTE but NOT the frame — release the
            // frame first so unmap+free doesn't leak a physical page per call.
            uint32_t pd_idx = vaddr >> 22;
            uint32_t pt_idx = (vaddr >> 12) & 0x3FF;
            uint32_t* pd_ent = (uint32_t*)(uintptr_t)pd;
            if (pd_ent[pd_idx] & PAGE_PRESENT) {
                uint32_t* pt = (uint32_t*)(uintptr_t)(pd_ent[pd_idx] & 0xFFFFF000);
                if (pt[pt_idx] & PAGE_PRESENT) {
                    extern void frame_free(uint32_t);
                    frame_free(pt[pt_idx] & 0xFFFFF000);
                }
            }
            regs->eax = (uint32_t)vmm_unmap_page(pd, vaddr);
            break;
        }

        // ----- SYS_CLIPBOARD_COPY (56) -----
        case SYS_CLIPBOARD_COPY: {
            const char* user_data = (const char*)regs->ebx;
            int len = (int)regs->ecx;
            if (len <= 0 || len >= 4096 || !validate_user_array_ptr(user_data, 1, len)) {
                regs->eax = (uint32_t)-1;
                break;
            }
            extern int clipboard_copy(const char* data, int len);
            regs->eax = (uint32_t)clipboard_copy(user_data, len);
            break;
        }

        // ----- SYS_CLIPBOARD_PASTE (57) -----
        case SYS_CLIPBOARD_PASTE: {
            char* user_buf = (char*)regs->ebx;
            int max_len = (int)regs->ecx;
            if (max_len <= 0 || !validate_user_array_ptr(user_buf, 1, max_len)) {
                regs->eax = (uint32_t)-1;
                break;
            }
            extern int clipboard_paste(char* buf, int max_len);
            regs->eax = (uint32_t)clipboard_paste(user_buf, max_len);
            break;
        }

        default:
            regs->eax = (uint32_t)-1; // Unknown syscall
            break;
    }

    // Deliver pending signals before iret'ing back to Ring 3. The frame is
    // rewritten in place (EIP/USERESP) so the handler runs; SYS_SIGRETURN
    // restores it afterwards. SYS_SIGRETURN already did its own frame restore,
    // SYS_EXIT never returns, and SYS_EXEC replaced the frame with the new
    // program's entry (no old-image signals apply to it).
    if (call_no != SYS_SIGRETURN && call_no != SYS_EXIT && call_no != SYS_EXEC) {
        extern int task_deliver_signals(void* frame);
        task_deliver_signals(regs);
    }
}

// ============================================================
// Init
// ============================================================
void init_syscalls(void) {
    fd_init();
    extern void sync_init(void);
    sync_init();
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
