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

// ============================================================
// File Descriptor Table (per-system, simple implementation)
// ============================================================
#define MAX_FDS 16
#define FD_FREE   0
#define FD_OPEN   1

typedef struct {
    int state;       // FD_FREE or FD_OPEN
    int vfs_index;   // Index into fs[] array
    int offset;      // Current read/write position
} fd_entry_t;

static fd_entry_t fd_table[MAX_FDS];

static void fd_init(void) {
    for (int i = 0; i < MAX_FDS; i++) {
        fd_table[i].state = FD_FREE;
        fd_table[i].vfs_index = -1;
        fd_table[i].offset = 0;
    }
}

static int fd_alloc(int vfs_idx) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (fd_table[i].state == FD_FREE) {
            fd_table[i].state = FD_OPEN;
            fd_table[i].vfs_index = vfs_idx;
            fd_table[i].offset = 0;
            return i;
        }
    }
    return -1; // No free FDs
}

static void fd_free(int fd) {
    if (fd >= 0 && fd < MAX_FDS) {
        fd_table[fd].state = FD_FREE;
        fd_table[fd].vfs_index = -1;
        fd_table[fd].offset = 0;
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
            int idx = vfs_find(filename);
            if (idx < 0) {
                // File not found — auto-create
                idx = vfs_create(filename);
                if (idx < 0) { regs->eax = (uint32_t)-1; break; }
            }
            int fd = fd_alloc(idx);
            regs->eax = (fd >= 0) ? (uint32_t)fd : (uint32_t)-1;
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

            int vi = fd_table[fd].vfs_index;
            int off = fd_table[fd].offset;
            int avail = fs[vi].size - off;
            if (avail <= 0) { regs->eax = 0; break; } // EOF
            int to_read = (size < avail) ? size : avail;
            memcpy(buf, fs[vi].data + off, to_read);
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

            int vi = fd_table[fd].vfs_index;
            int off = fd_table[fd].offset;
            int space = MAX_FILE_SIZE - off;
            if (space <= 0) { regs->eax = 0; break; } // Full
            int to_write = (size < space) ? size : space;
            memcpy(fs[vi].data + off, buf, to_write);
            fd_table[fd].offset += to_write;
            if (fd_table[fd].offset > fs[vi].size) {
                fs[vi].size = fd_table[fd].offset;
            }
            fs[vi].data[fs[vi].size] = '\0'; // null-terminate
            vfs_save(); // Persist to disk
            regs->eax = (uint32_t)to_write;
            break;
        }

        // ----- SYS_CLOSE (5): Close an open file -----
        case SYS_CLOSE: {
            int fd = (int)regs->ebx;
            if (fd < 0 || fd >= MAX_FDS || fd_table[fd].state != FD_OPEN) {
                regs->eax = (uint32_t)-1; break;
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
            print("[syscall] Task exited.\n", 0x0E);
            task_exit();
            // task_exit never returns, but just in case:
            for(;;) __asm__("hlt");
            break;
        }

        // ----- SYS_DRAW_RECT (11): Draw rectangle -----
        case SYS_DRAW_RECT: {
            int x = (int)regs->ebx;
            int y = (int)regs->ecx;
            int w = (int)regs->edx;
            int h = (int)regs->esi;
            uint32_t color = (uint32_t)regs->edi;
            draw_rect(x, y, w, h, color);
            regs->eax = 0;
            break;
        }

        // ----- SYS_DRAW_TEXT (12): Draw text string -----
        case SYS_DRAW_TEXT: {
            int x = (int)regs->ebx;
            int y = (int)regs->ecx;
            const char* text = (const char*)regs->edx;
            uint32_t fg = (uint32_t)regs->esi;
            uint32_t bg = (uint32_t)regs->edi;
            if (safe_strlen(text, 256) < 0) { regs->eax = (uint32_t)-1; break; }
            draw_string_px(x, y, text, fg, bg);
            regs->eax = 0;
            break;
        }

        // ----- SYS_GET_KEY (13): Non-blocking keyboard read -----
        case SYS_GET_KEY: {
            uint8_t sc = k_get_scancode();
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
