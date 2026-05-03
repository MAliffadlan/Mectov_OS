#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"
#include "rtc.h"
#include "pci.h"

typedef struct {
    uint32_t uptime_ms;
    uint32_t total_ram_kb;
    uint32_t used_ram_kb;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_bpp;
    char cpu_brand[48];
    uint8_t mac_addr[6];
} sysinfo_t;

typedef struct {
    char name[32];
    int type;     // 0=file, 1=dir, 2=dev
    int size;
    int node_idx;
} dir_entry_t;

typedef struct {
    int id;
    int ring;
    int state;
    int priority;
} sys_task_info_t;

typedef struct {
    int id;
    char title[32];
    int owner_ring;
    int visible;
    int minimized;
} sys_win_info_t;

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

#define SYS_GET_TIME       33  // EBX=rtc_time_t* ptr
#define SYS_PLAY_SOUND     34  // EBX=freq, ECX=duration_ms
#define SYS_GET_SYSINFO    35  // EBX=sysinfo_t* ptr
#define SYS_GET_PCI_INFO   36  // EBX=pci_device_t* ptr, ECX=max_count -> returns count
#define SYS_LIST_DIR       37  // EBX=dir_entry_t* array, ECX=max_count, EDX=parent_node -> returns count
#define SYS_STAT_FILE      38  // EBX=path_ptr -> returns node_idx or -1

// Network (Browser Ring 3)
#define SYS_DNS_RESOLVE    39  // EBX=domain_ptr -> returns 0 (async)
#define SYS_TCP_CONNECT    40  // EBX=ip_ptr(4 bytes), ECX=port -> returns 0
#define SYS_TCP_SEND       41  // EBX=data_ptr, ECX=len -> returns 0/-1
#define SYS_TCP_RECV       42  // EBX=buf_ptr, ECX=max_len -> returns bytes_read or state (<0)
#define SYS_NET_STATUS     43  // -> returns packed status: dns_resolved|tcp_state|dns_ip

// Terminal (Ring 3 stdout IPC)
#define SYS_SET_STDOUT_IPC 44  // EBX=ipc_qid (0 to disable)
#define SYS_EXEC_CMD       45  // EBX=cmd_string_ptr -> returns 0
#define SYS_GET_TASKS      46  // EBX=sys_task_info_t* array, ECX=max_count -> returns count
#define SYS_GET_WINDOWS    47  // EBX=sys_win_info_t* array, ECX=max_count -> returns count
#define SYS_KILL_TASK      48  // EBX=tid -> returns 0/-1
#define SYS_GET_LAUNCH_ARG 49  // EBX=buf_ptr, ECX=max_len -> returns length
#define SYS_CREATE_FILE    50  // EBX=path_ptr -> returns 0/-1

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

// New Phase 1 syscalls
static inline void sys_get_time(rtc_time_t* out_time) {
    __asm__ __volatile__ ("int $0x80" : : "a"(SYS_GET_TIME), "b"(out_time));
}
static inline void sys_play_sound(int freq, int duration_ms) {
    __asm__ __volatile__ ("int $0x80" : : "a"(SYS_PLAY_SOUND), "b"(freq), "c"(duration_ms));
}

static inline void sys_get_sysinfo(sysinfo_t* out_info) {
    __asm__ __volatile__ ("int $0x80" : : "a"(SYS_GET_SYSINFO), "b"(out_info));
}
static inline int sys_get_pci_info(pci_device_t* out_array, int max_count) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_GET_PCI_INFO), "b"(out_array), "c"(max_count));
    return ret;
}
static inline int sys_list_dir(dir_entry_t* out_array, int max_count, int parent_node) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_LIST_DIR), "b"(out_array), "c"(max_count), "d"(parent_node));
    return ret;
}
static inline int sys_stat_file(const char* path) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_STAT_FILE), "b"(path));
    return ret;
}

// Network syscalls
static inline void sys_dns_resolve(const char* domain) {
    __asm__ __volatile__ ("int $0x80" : : "a"(SYS_DNS_RESOLVE), "b"(domain));
}
static inline void sys_tcp_connect(uint8_t* ip, int port) {
    __asm__ __volatile__ ("int $0x80" : : "a"(SYS_TCP_CONNECT), "b"(ip), "c"(port));
}
static inline int sys_tcp_send(const void* data, int len) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_TCP_SEND), "b"(data), "c"(len));
    return ret;
}
static inline int sys_tcp_recv(void* buf, int max_len) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_TCP_RECV), "b"(buf), "c"(max_len));
    return ret;
}

typedef struct {
    int dns_resolved;
    uint8_t dns_ip[4];
    int tcp_state;
} net_status_t;

static inline void sys_net_status(net_status_t* out) {
    __asm__ __volatile__ ("int $0x80" : : "a"(SYS_NET_STATUS), "b"(out));
}

// Terminal IPC syscalls
static inline void sys_set_stdout_ipc(int qid) {
    __asm__ __volatile__ ("int $0x80" : : "a"(SYS_SET_STDOUT_IPC), "b"(qid));
}
static inline int sys_exec_cmd(const char* cmd) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_EXEC_CMD), "b"(cmd));
    return ret;
}

static inline int sys_get_tasks(sys_task_info_t* array, int max_count) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_GET_TASKS), "b"(array), "c"(max_count));
    return ret;
}

static inline int sys_get_windows(sys_win_info_t* array, int max_count) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_GET_WINDOWS), "b"(array), "c"(max_count));
    return ret;
}

static inline int sys_kill_task(int tid) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_KILL_TASK), "b"(tid));
    return ret;
}

static inline int sys_get_launch_arg(char* buf, int max_len) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_GET_LAUNCH_ARG), "b"(buf), "c"(max_len));
    return ret;
}

static inline int sys_create_file(const char* path) {
    return syscall(SYS_CREATE_FILE, (int)path, 0, 0);
}

// IPC syscalls (stubs for Ring 3)
static inline int sys_ipc_create(int key) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_IPC_CREATE), "b"(key));
    return ret;
}
static inline int sys_ipc_try_recv(int qid, void* data, int max_len) {
    int ret;
    uint32_t len_out = (uint32_t)max_len;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_IPC_TRY_RECV), "b"(qid), "c"(0), "d"(data), "S"(&len_out) : "memory");
    if (ret == 0) return (int)len_out;
    return ret;
}

#endif
