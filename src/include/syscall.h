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
    uint32_t cpu_count;    // number of online cores (1..4)
    uint32_t cpu_load[4];  // per-core load percent over the last ~50 ms
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
#define SYS_LOAD_LIBRARY   51  // EBX=lib_name_ptr -> returns base_address of export table
#define SYS_SET_VOLUME     52  // EBX=volume (0-100)
#define SYS_GET_VOLUME     53  // -> returns volume (0-100)
#define SYS_PLAY_WAV       54  // EBX=pcm_data_ptr, ECX=length, EDX=sample_rate -> returns 0
#define SYS_STOP_WAV       55  // -> returns 0
#define SYS_CLIPBOARD_COPY 56  // EBX=str_ptr, ECX=len -> returns len
#define SYS_CLIPBOARD_PASTE 57 // EBX=buf_ptr, ECX=max_len -> returns len
#define SYS_DELETE_FILE    58  // EBX=path_ptr -> returns 0/-1
#define SYS_MKDIR          59  // EBX=path_ptr -> returns 0/-1
#define SYS_RENAME_FILE    60  // EBX=old_path_ptr, ECX=new_path_ptr -> returns 0/-1

// Synchronization (semaphores & futexes)
#define SYS_SEM_CREATE     61  // EBX=initial_count -> returns sem_id or -1
#define SYS_SEM_WAIT       62  // EBX=sem_id -> returns 0/-1 (blocks if count==0)
#define SYS_SEM_POST       63  // EBX=sem_id -> returns 0/-1
#define SYS_SEM_DESTROY    64  // EBX=sem_id -> returns 0/-1
#define SYS_FUTEX_WAIT     65  // EBX=addr, ECX=expected -> 0 slept, -1 value changed
#define SYS_FUTEX_WAKE     66  // EBX=addr, ECX=max_waiters -> returns woken count

// UDP networking
#define SYS_UDP_BIND       67  // EBX=port -> returns 0/-1
#define SYS_UDP_SEND       68  // EBX=ip_ptr, ECX=dst_port, EDX=data_ptr, ESI=len -> 0/-1
#define SYS_UDP_RECV       69  // EBX=buf_ptr, ECX=max_len -> bytes copied

// TCP connection management (multi-connection)
#define SYS_TCP_CLOSE      70  // EBX=conn_id -> returns 0/-1
#define SYS_TCP_LISTEN     85  // EBX=port -> conn_id or -1
#define SYS_SIGACTION      86  // EBX=sig, ECX=&sigaction(in,opt), EDX=&sigaction(old,opt) -> 0/-1
#define SYS_SIGPROCMASK    87  // EBX=how(0/1/2), ECX=&set(in,opt), EDX=&oldset(opt) -> 0/-1
#define SYS_SETPGID        88  // EBX=pid(0=self), ECX=pgid(0=self) -> 0/-1
#define SYS_GETPGRP        89  // -> pgrp of the calling task
#define SYS_SETSID         90  // -> new session id (task becomes group+session leader)
#define SYS_TCSETPGRP      91  // EBX=fd, ECX=pgrp -> 0/-1 (set foreground pgrp)
#define SYS_TCGETPGRP      92  // EBX=fd -> foreground pgrp

// Process model & signals
#define SYS_FORK        71  // -> child tid (parent) / 0 (child) / -1
#define SYS_WAITPID     72  // EBX=pid, ECX=status_ptr, EDX=options -> child tid / 0 (WNOHANG) / -1
#define SYS_KILL        73  // EBX=pid, ECX=sig -> 0/-1
#define SYS_SIGNAL      74  // EBX=sig, ECX=handler(0=default,1=SIG_IGN,else fn) -> old handler
#define SYS_SIGRETURN   75  // restore the frame saved before a signal handler
#define SYS_GETPPID     76  // -> parent task id
#define SYS_EXEC        77  // EBX=path_ptr, ECX=arg_ptr(optional) -> never returns on success, -1 on failure

// Shared memory (System V style)
#define SYS_SHMGET      78  // EBX=key, ECX=size -> shm id (1-based) or -1
#define SYS_SHMAT       79  // EBX=shmid -> base VA or 0
#define SYS_SHMDT       80  // EBX=addr -> 0/-1
#define SYS_SHMCTL      81  // EBX=shmid, ECX=cmd(0=IPC_RMID) -> 0/-1

// mmap / munmap (demand paging)
#define SYS_MMAP        82  // EBX=size -> base VA (reserved, frames on fault) or 0
#define SYS_MUNMAP      83  // EBX=base VA (from mmap) -> 1 or 0
#define SYS_DUP2        84  // EBX=oldfd, ECX=newfd -> newfd or -1

// File-backed mmap: SYS_MMAP_FILE maps an open VFS FILE fd into the address
// space (pages fault in lazily FROM THE DISK on first access); dirty pages
// write back to the file on SYS_MSYNC and SYS_MUNMAP. The mapping keeps its
// own node reference, so the fd may be closed right after mmap.
#define SYS_MMAP_FILE   93  // EBX=fd, ECX=flags(MMAP_FILE_SHARED) -> base VA or 0
#define SYS_MSYNC       94  // EBX=base VA (from mmap_file) -> 1 written/0 failed

// Signal numbers (POSIX subset; SIGKILL and SIGSTOP cannot be caught or
// ignored, SIGCONT resumes a stopped task)
#define SIGINT   2
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
// Pass 1 as the handler to sys_signal to ignore a signal
#define SIG_IGN  1
// waitpid options
#define WNOHANG  1

// Virtual Memory
#define SYS_VMM_MAP        29  // EBX=vaddr, ECX=paddr, EDX=flags → return 0/-1
static inline void* sys_mmap(uint32_t size) {
    void* ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_MMAP), "b"(size));
    return ret;
}
static inline int sys_munmap(void* addr) {
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_MUNMAP), "b"(addr));
    return ret;
}
// File-backed mmap of an open file fd (flags: MMAP_FILE_SHARED). The mapping
// survives closing the fd; dirty pages write back on msync/munmap. Raw asm
// (the generic syscall() helper is declared below this point).
static inline void* sys_mmap_file(int fd, int flags) {
    void* ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_MMAP_FILE), "b"(fd), "c"(flags));
    return ret;
}
// Flush dirty pages of a file-backed mapping back to its file. Returns 1 if
// everything was written (or there was nothing to write), 0 on failure.
static inline int sys_msync(void* addr) {
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_MSYNC), "b"(addr));
    return ret;
}
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

static inline int sys_dup2(int oldfd, int newfd) {
    return syscall(SYS_DUP2, oldfd, newfd, 0);
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

static inline int sys_sleep(int ticks) {
    return syscall(SYS_SLEEP, ticks, 0, 0);
}

static inline int sys_getpid(void) {
    return syscall(SYS_GET_PID, 0, 0, 0);
}

static inline void sys_yield(void) {
    syscall(SYS_YIELD, 0, 0, 0);
}

static inline int sys_thread_create(void (*entry)(), int priority) {
    // The kernel ignores EDX (page_dir) for security — a thread always shares
    // its creator's address space.
    return syscall(SYS_THREAD_CREATE, (int)entry, priority, 0);
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
static inline void sys_set_volume(int vol) {
    __asm__ __volatile__ ("int $0x80" : : "a"(SYS_SET_VOLUME), "b"(vol));
}
static inline int sys_get_volume(void) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_GET_VOLUME));
    return ret;
}
static inline void sys_play_wav(void* pcm_data, uint32_t length, uint16_t sample_rate) {
    __asm__ __volatile__ ("int $0x80" : : "a"(SYS_PLAY_WAV), "b"(pcm_data), "c"(length), "d"(sample_rate));
}
static inline void sys_stop_wav(void) {
    __asm__ __volatile__ ("int $0x80" : : "a"(SYS_STOP_WAV));
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
// Returns the connection id (0..7) or -1 on failure.
static inline int sys_tcp_connect(uint8_t* ip, int port) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_TCP_CONNECT), "b"(ip), "c"(port));
    return ret;
}
static inline int sys_tcp_send(int conn_id, const void* data, int len) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_TCP_SEND), "b"(data), "c"(len), "d"(conn_id));
    return ret;
}
static inline int sys_tcp_recv(int conn_id, void* buf, int max_len) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_TCP_RECV), "b"(buf), "c"(max_len), "d"(conn_id));
    return ret;
}
static inline int sys_tcp_close(int conn_id) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_TCP_CLOSE), "b"(conn_id));
    return ret;
}
static inline int sys_tcp_listen(int port) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(SYS_TCP_LISTEN), "b"(port));
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

static inline int sys_clipboard_copy(const char* data, int len) {
    return syscall(SYS_CLIPBOARD_COPY, (int)data, len, 0);
}

static inline int sys_clipboard_paste(char* buf, int max_len) {
    return syscall(SYS_CLIPBOARD_PASTE, (int)buf, max_len, 0);
}

static inline int sys_delete_file(const char* path) {
    return syscall(SYS_DELETE_FILE, (int)path, 0, 0);
}

static inline int sys_mkdir(const char* path) {
    return syscall(SYS_MKDIR, (int)path, 0, 0);
}

static inline int sys_rename_file(const char* old_path, const char* new_path) {
    return syscall(SYS_RENAME_FILE, (int)old_path, (int)new_path, 0);
}

// Synchronization syscalls (semaphores & futexes)
static inline int sys_sem_create(int initial) {
    return syscall(SYS_SEM_CREATE, initial, 0, 0);
}
static inline int sys_sem_wait(int sem_id) {
    return syscall(SYS_SEM_WAIT, sem_id, 0, 0);
}
static inline int sys_sem_post(int sem_id) {
    return syscall(SYS_SEM_POST, sem_id, 0, 0);
}
static inline int sys_sem_destroy(int sem_id) {
    return syscall(SYS_SEM_DESTROY, sem_id, 0, 0);
}
static inline int sys_futex_wait(void* addr, uint32_t expected) {
    return syscall(SYS_FUTEX_WAIT, (int)addr, (int)expected, 0);
}
static inline int sys_futex_wake(void* addr, int max_waiters) {
    return syscall(SYS_FUTEX_WAKE, (int)addr, max_waiters, 0);
}

// UDP syscalls
static inline int sys_udp_bind(uint16_t port) {
    return syscall(SYS_UDP_BIND, port, 0, 0);
}
static inline int sys_udp_send(uint8_t* ip, uint16_t dst_port, const void* data, int len) {
    return syscall5(SYS_UDP_SEND, (int)ip, dst_port, (int)data, len, 0);
}
static inline int sys_udp_recv(void* buf, int max_len) {
    return syscall(SYS_UDP_RECV, (int)buf, max_len, 0);
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

// ============================================================
// Process model & signal syscalls (Ring 3 stubs)
// ============================================================
static inline int sys_fork(void) {
    return syscall(SYS_FORK, 0, 0, 0);
}

static inline int sys_waitpid(int pid, int* status, int options) {
    return syscall(SYS_WAITPID, pid, (int)status, options);
}

static inline int sys_kill(int pid, int sig) {
    return syscall(SYS_KILL, pid, sig, 0);
}

static inline void* sys_signal(int sig, void* handler) {
    return (void*)syscall(SYS_SIGNAL, sig, (int)handler, 0);
}

// sigaction (POSIX subset): handler (0=default,1=SIG_IGN,else fn), sa_mask
// (signals auto-blocked while the handler runs), sa_flags (SA_RESTART=1,
// SA_NODEFER=2).
typedef struct {
    uint32_t handler;
    uint32_t mask;
    uint32_t flags;
} sigaction_t;

static inline int sys_sigaction(int sig, sigaction_t* act, sigaction_t* old) {
    return syscall(SYS_SIGACTION, sig, (int)act, (int)old);
}

// sigprocmask: how is SIG_BLOCK(0)/SIG_UNBLOCK(1)/SIG_SETMASK(2); newset and
// oldset are optional (NULL to skip). Returns 0/-1.
static inline int sys_sigprocmask(int how, uint32_t* newset, uint32_t* oldset) {
    return syscall(SYS_SIGPROCMASK, how, (int)newset, (int)oldset);
}

static inline int sys_sigreturn(void) {
    return syscall(SYS_SIGRETURN, 0, 0, 0);
}

// Process groups / sessions (Fase 2)
static inline int sys_setpgid(int pid, int pgid) {
    return syscall(SYS_SETPGID, pid, pgid, 0);
}
static inline int sys_getpgrp(void) {
    return syscall(SYS_GETPGRP, 0, 0, 0);
}
static inline int sys_setsid(void) {
    return syscall(SYS_SETSID, 0, 0, 0);
}
static inline int sys_tcsetpgrp(int fd, int pgrp) {
    return syscall(SYS_TCSETPGRP, fd, pgrp, 0);
}
static inline int sys_tcgetpgrp(int fd) {
    return syscall(SYS_TCGETPGRP, fd, 0, 0);
}

static inline int sys_getppid(void) {
    return syscall(SYS_GETPPID, 0, 0, 0);
}

// Replace the current task's image with the program at `path` (POSIX exec).
// `arg` (optional, may be NULL) becomes the new program's launch arg. On
// success the current program is replaced and this never returns; returns -1
// on failure (old image keeps running).
static inline int sys_exec(const char* path, const char* arg) {
    return syscall(SYS_EXEC, (int)path, (int)arg, 0);
}

// Shared memory
static inline int sys_shmget(uint32_t key, uint32_t size) {
    return syscall(SYS_SHMGET, (int)key, (int)size, 0);
}
static inline void* sys_shmat(int shmid) {
    return (void*)syscall(SYS_SHMAT, shmid, 0, 0);
}
static inline int sys_shmdt(void* addr) {
    return syscall(SYS_SHMDT, (int)addr, 0, 0);
}
static inline int sys_shmctl(int shmid, int cmd) {
    return syscall(SYS_SHMCTL, shmid, cmd, 0);
}

// Exit the current task with a status (available to child processes).
// Legacy callers use sys_exit() which passes status 0.
static inline void sys_exit_with_code(int code) {
    syscall(SYS_EXIT, code, 0, 0);
    for(;;); // Never returns
}

#endif
