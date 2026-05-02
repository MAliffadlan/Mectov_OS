#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

// ============================================================
// Syscall Numbers — Mectov OS User Mode API
// ============================================================
#define SYS_PRINT       1   // Print string. EBX=str_ptr, ECX=color
#define SYS_OPEN        2   // Open VFS file. EBX=filename_ptr → return fd
#define SYS_READ        3   // Read file. EBX=fd, ECX=buf_ptr, EDX=size → return bytes_read
#define SYS_WRITE       4   // Write file. EBX=fd, ECX=buf_ptr, EDX=size → return bytes_written
#define SYS_CLOSE       5   // Close file. EBX=fd
#define SYS_MALLOC      6   // Allocate memory. EBX=size → return pointer
#define SYS_FREE        7   // Free memory. EBX=pointer
#define SYS_GET_TICKS   8   // Get timer ticks → return ticks
#define SYS_YIELD       9   // Yield CPU to scheduler
#define SYS_EXIT        10  // Exit current task
#define SYS_DRAW_RECT   11  // Draw rect. EBX=win_id, ECX=x, EDX=y, ESI=(w<<16)|h, EDI=color
#define SYS_DRAW_TEXT   12  // Draw text. EBX=win_id, ECX=x, EDX=y, ESI=str_ptr, EDI=color
#define SYS_GET_KEY     13  // Get keyboard char (non-blocking) → return char or 0
#define SYS_GET_MOUSE   14  // Get mouse state → EAX=x, EBX=y, ECX=buttons
#define SYS_CREATE_WINDOW 15 // EBX=x, ECX=y, EDX=w, ESI=h, EDI=title_ptr -> win_id
#define SYS_GET_EVENT   16   // EBX=win_id, ECX=event_ptr
#define SYS_UPDATE_WINDOW 17 // EBX=win_id

// === NEW SYSCALLS for Modern Architecture ===
// Thread & Process Management
#define SYS_THREAD_CREATE  18  // EBX=entry, ECX=priority, EDX=page_dir → return TID
#define SYS_SLEEP          19  // EBX=ticks
#define SYS_GET_PID        20  // → return current TID
#define SYS_SET_PRIORITY   21  // EBX=tid, ECX=priority
#define SYS_GET_PRIORITY   22  // EBX=tid → return priority

// IPC
#define SYS_IPC_CREATE     23  // EBX=key → return qid
#define SYS_IPC_SEND       24  // EBX=qid, ECX=type, EDX=data_ptr, ESI=len → return 0/-1
#define SYS_IPC_RECV       25  // EBX=qid, ECX=type_out, EDX=data_out, ESI=len_out → return 0/-1
#define SYS_IPC_DESTROY    26  // EBX=qid
#define SYS_IPC_TRY_SEND   27  // EBX=qid, ECX=type, EDX=data_ptr, ESI=len → return 0/-1
#define SYS_IPC_TRY_RECV   28  // EBX=qid, ECX=type_out, EDX=data_out, ESI=len_out → return 0/-1

// Virtual Memory
#define SYS_VMM_MAP        29  // EBX=vaddr, ECX=paddr, EDX=flags → return 0/-1
#define SYS_VMM_ALLOC      30  // EBX=vaddr, ECX=flags → return vaddr or 0
#define SYS_VMM_FREE       31  // EBX=vaddr → return 0/-1

// UNIX
#define SYS_PIPE           32  // EBX=pipefd_ptr[2] → return 0/-1

// Initialize syscall handler (int 0x80)
void init_syscalls(void);

// Switch current execution to Ring 3 user mode
void switch_to_user_mode(void);

// ============================================================
// Syscall stubs for user programs (called from Ring 3)
// ============================================================

// 3-argument syscall (covers most cases)
static inline int syscall(int num, int arg1, int arg2, int arg3) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3)
    );
    return ret;
}

// 5-argument syscall (for draw_rect and draw_text that need ESI + EDI)
static inline int syscall5(int num, int arg1, int arg2, int arg3, int arg4, int arg5) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3), "S"(arg4), "D"(arg5)
    );
    return ret;
}

// ============================================================
// Convenience wrappers for user programs
// ============================================================
static inline void sys_print(const char* msg, int color) {
    syscall(SYS_PRINT, (int)msg, color, 0);
}

static inline int sys_open(const char* filename) {
    return syscall(SYS_OPEN, (int)filename, 0, 0);
}

static inline int sys_read(int fd, char* buf, int size) {
    return syscall(SYS_READ, fd, (int)buf, size);
}

static inline int sys_write(int fd, const char* buf, int size) {
    return syscall(SYS_WRITE, fd, (int)buf, size);
}

static inline void sys_close(int fd) {
    syscall(SYS_CLOSE, fd, 0, 0);
}

static inline int sys_pipe(int pipefd[2]) {
    return syscall(SYS_PIPE, (int)pipefd, 0, 0);
}

static inline void* sys_malloc(int size) {
    return (void*)syscall(SYS_MALLOC, size, 0, 0);
}

static inline void sys_free(void* ptr) {
    syscall(SYS_FREE, (int)ptr, 0, 0);
}

static inline uint32_t sys_get_ticks(void) {
    return (uint32_t)syscall(SYS_GET_TICKS, 0, 0, 0);
}

static inline void sys_yield(void) {
    syscall(SYS_YIELD, 0, 0, 0);
}

static inline void sys_exit(void) {
    syscall(SYS_EXIT, 0, 0, 0);
    for(;;); // Never returns
}

static inline void sys_draw_rect(int wid, int x, int y, int w, int h, uint32_t color) {
    int wh = ((w & 0xFFFF) << 16) | (h & 0xFFFF);
    syscall5(SYS_DRAW_RECT, wid, x, y, wh, (int)color);
}

static inline void sys_draw_text(int wid, int x, int y, const char* text, uint32_t color) {
    syscall5(SYS_DRAW_TEXT, wid, x, y, (int)text, (int)color);
}

static inline int sys_get_key(void) {
    return syscall(SYS_GET_KEY, 0, 0, 0);
}

static inline int sys_create_window(int x, int y, int w, int h, const char* title) {
    return syscall5(SYS_CREATE_WINDOW, x, y, w, h, (int)title);
}

static inline int sys_get_event(int wid, void* event_ptr) {
    return syscall(SYS_GET_EVENT, wid, (int)event_ptr, 0);
}

static inline void sys_update_window(int wid) {
    syscall(SYS_UPDATE_WINDOW, wid, 0, 0);
}

// Mouse state return struct (packed into registers)
typedef struct {
    int x, y;
    int buttons;
} mouse_state_t;

static inline mouse_state_t sys_get_mouse(void) {
    mouse_state_t m;
    int rx, ry, rb;
    __asm__ volatile(
        "int $0x80"
        : "=a"(rx), "=b"(ry), "=c"(rb)
        : "a"(SYS_GET_MOUSE)
    );
    m.x = rx;
    m.y = ry;
    m.buttons = rb;
    return m;
}

#endif
