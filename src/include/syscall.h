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

// POSIX file positioning & metadata
#define SYS_LSEEK       95  // EBX=fd, ECX=offset, EDX=whence(SEEK_*) -> new offset or -1
#define SYS_FSTAT       96  // EBX=fd, ECX=stat_t* -> 0 or -1

// I/O multiplexing + POSIX misc
#define SYS_POLL        97  // EBX=pollfd_t*, ECX=nfds, EDX=timeout_ms -> ready count / 0 / -1
#define SYS_SELECT      98  // EBX=nfds, ECX=readfds*, EDX=writefds*, ESI=exceptfds*, EDI=timeout_ms -> ready count / 0 / -1
#define SYS_GETCWD      99  // EBX=buf, ECX=size -> 0 or -1
#define SYS_CHDIR       100 // EBX=path -> 0 or -1
#define SYS_CLOCK_GETTIME 101 // EBX=clock_id, ECX=timespec_t* -> 0 or -1
#define SYS_CHMOD        102 // EBX=path_ptr, ECX=mode (9 bits) -> 0 or -1
#define SYS_CHOWN        103 // EBX=path_ptr, ECX=uid, EDX=gid -> 0 or -1 (root only)
#define SYS_CLONE        104 // EBX=entry, ECX=child_stack (0=default), EDX=tls_base (0=none) -> child: 0, parent: TID
#define SYS_TLS_SET      105 // EBX=tls_base (TCB VA, 0 = remove) -> 0 or -1. Sets CURRENT task's %gs
#define SYS_GETRLIMIT    106 // EBX=resource (RLIMIT_*), ECX=rlimit_t* {cur,max} -> 0 or -1
#define SYS_SETRLIMIT    107 // EBX=resource, ECX=rlimit_t* {cur,max} -> 0 or -1 (POSIX privilege rules)

// Mount table (v38.42): mount a filesystem at a directory at runtime.
#define SYS_MOUNT        115 // EBX=path (mount point), ECX="ext2"|"fat32", EDX=ATA drive -> 0 or -1 (root only)
#define SYS_UMOUNT       116 // EBX=path -> 0 or -1 (root only)

// Kernel CSPRNG (entropy.c): fill a user buffer with cryptographically random
// bytes from the ChaCha8 DRBG. Mirrors POSIX getrandom(, , 0).
#define SYS_GETRANDOM    117 // EBX=buf, ECX=buflen -> 0 or -1

// Direct framebuffer access (display-server foundation). SYS_FB_MAP maps the
// VBE linear framebuffer READ/WRITE into the CALLING task's address space —
// device memory via PAGE_DEV PTEs (never COW'd on fork, never refcounted or
// freed by the frame allocator) — and returns the geometry in *info. While
// a live task holds the scanout, the kernel desktop stops presenting frames
// and keystrokes route to the holder (SYS_FB_RELEASE / exit / exec hands
// the pixels back).
// Authorized like logind grants DRM master: uid 0, or the controlling
// terminal's FOREGROUND process group (the active console session). A
// background job is refused.
#define SYS_FB_MAP       118 // EBX=fb_info_t* -> 0 or -1
#define SYS_FB_RELEASE   119 // (no args) -> 0 or -1; only the current owner

// fb_info_t is defined in types.h (the common base of kernel task.h and this
// Ring 3 ABI header) so both sides share one definition.

// POSIX socket API (v38.43) — fd-integrated: socket()/accept() return file
// descriptors, so read/write/close/poll/select work on sockets uniformly.
#define SYS_SOCKET       108 // EBX=domain (AF_INET), ECX=type (SOCK_*) -> fd or -1
#define SYS_BIND         109 // EBX=fd, ECX=sockaddr_t* -> 0 or -1
#define SYS_LISTEN       110 // EBX=fd, ECX=backlog (ignored; slots are the backlog) -> 0 or -1
#define SYS_ACCEPT       111 // EBX=fd -> fd of one completed inbound connection or -1
#define SYS_CONNECT      112 // EBX=fd, ECX=sockaddr_t* -> 0 (handshake started) or -1
#define SYS_SENDTO       113 // EBX=fd, ECX=buf, EDX=len, ESI=sockaddr_t* (NULL for stream) -> sent or -1
#define SYS_RECVFROM     114 // EBX=fd, ECX=buf, EDX=max, ESI=src ip[4] (optional), EDI=src port* (optional) -> n/0/-1

#define AF_INET          2
#define SOCK_STREAM      1
#define SOCK_DGRAM       2

// Socket address (fixed 8-byte form; family is AF_INET)
typedef struct {
    uint16_t family;
    uint16_t port;     // host byte order
    uint8_t  ip[4];
} __attribute__((packed)) sockaddr_t;

// Resource limits (rlimit_t + RLIMIT_* are defined in types.h, the common
// base of syscall.h and task.h). Enforced at fork/clone/thread-create
// (NPROC), fd allocation (NOFILE) and heap/mmap growth (AS).

// Open flags (SYS_OPEN mode / third arg)
#define O_APPEND        8   // write always appends at end of file

// lseek whence values
#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

// File metadata returned by SYS_FSTAT (type mirrors fs_type_t: 0=file,
// 1=dir, 2=dev, 3=ext2 file, 4=ext2 dir, 5=proc)
typedef struct {
    int size;           // File size in bytes (files only)
    int type;           // fs_type_t value
    int node_idx;       // VFS node index
    int parent;         // Parent node index (-1 = root)
    int data_sector;    // Starting ATA sector (files only)
    char name[32];      // Node name
    uint16_t mode;      // 9 permission bits (v38.23 ownership)
    uint16_t uid;       // owning user
    uint16_t gid;       // owning group
} stat_t;

// poll() events/struct (SYS_POLL). revents is filled by the kernel.
#define POLLIN    0x001  // data available to read
#define POLLOUT   0x004  // buffer space available to write
#define POLLHUP   0x010  // peer closed (EOF on a pipe read end)
#define POLLNVAL  0x020  // fd not open
typedef struct {
    int fd;
    int events;    // requested events (POLLIN/POLLOUT)
    int revents;   // returned events (kernel fills)
} pollfd_t;

// timespec for SYS_CLOCK_GETTIME (CLOCK_MONOTONIC only)
#define CLOCK_MONOTONIC 1
typedef struct {
    uint32_t tv_sec;
    uint32_t tv_nsec;
} timespec_t;

// Signal numbers (POSIX subset; SIGKILL and SIGSTOP cannot be caught or
// ignored, SIGCONT resumes a stopped task)
#define SIGINT   2
#define SIGFPE   8    // math fault (delivered by the kernel's #XM handler)
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

// Open with flags (O_APPEND); the kernel stores them on the descriptor.
static inline int sys_open_mode(const char* filename, int mode) {
    return syscall(SYS_OPEN, (int)filename, mode, 0);
}

// POSIX lseek: whence is SEEK_SET(0)/SEEK_CUR(1)/SEEK_END(2). Returns the
// new offset, or -1 on error (bad fd, non-seekable, negative result).
static inline int sys_lseek(int fd, int offset, int whence) {
    return syscall(SYS_LSEEK, fd, offset, whence);
}

// POSIX fstat: fills stat_t {size, type, node_idx, parent, data_sector,
// name, mode, uid, gid} for an open file descriptor. Returns 0 or -1.
static inline int sys_fstat(int fd, stat_t* st) {
    return syscall(SYS_FSTAT, fd, (int)st, 0);
}

// chmod: change a file's permission bits. EBX=path, ECX=mode (9 bits).
// Owner or root only; returns 0 or -1.
static inline int sys_chmod(const char* path, int mode) {
    return syscall(SYS_CHMOD, (int)path, mode, 0);
}

// chown: transfer a file's ownership. EBX=path, ECX=uid, EDX=gid.
// Root only (POSIX); returns 0 or -1.
static inline int sys_chown(const char* path, int uid, int gid) {
    return syscall(SYS_CHOWN, (int)path, uid, gid);
}

// mount/umount (v38.42): mount filesystem `fstype` ("ext2"|"fat32") from
// ATA `drive` at directory `path`, and undo it. Root only; 0 or -1.
static inline int sys_mount(const char* path, const char* fstype, int drive) {
    return syscall(SYS_MOUNT, (int)path, (int)fstype, drive);
}
static inline int sys_umount(const char* path) {
    return syscall(SYS_UMOUNT, (int)path, 0, 0);
}

// Map the VBE linear framebuffer into THIS task (root only). On success the
// geometry is written to *info and 0 is returned; -1 on failure (not root,
// no VBE fb, out of address-space slots or RLIMIT_AS).
static inline int sys_fb_map(fb_info_t* info) {
    return syscall(SYS_FB_MAP, (int)info, 0, 0);
}

// Give the scanout back (only the current owner may). The kernel desktop
// repaints on the next tick. 0 or -1.
static inline int sys_fb_release(void) {
    return syscall(SYS_FB_RELEASE, 0, 0, 0);
}


// POSIX sockets (v38.43) — fd-integrated: read()/write()/close()/poll()
// work on the returned descriptors. accept() is non-blocking (poll first);
// connect() returns once the SYN is out (poll for POLLOUT = established).
static inline int sys_socket(int domain, int type) {
    return syscall(SYS_SOCKET, domain, type, 0);
}
static inline int sys_bind(int fd, sockaddr_t* sa) {
    return syscall(SYS_BIND, fd, (int)sa, 0);
}
static inline int sys_listen(int fd, int backlog) {
    return syscall(SYS_LISTEN, fd, backlog, 0);
}
static inline int sys_accept(int fd) {
    return syscall(SYS_ACCEPT, fd, 0, 0);
}
static inline int sys_connect(int fd, sockaddr_t* sa) {
    return syscall(SYS_CONNECT, fd, (int)sa, 0);
}
static inline int sys_sendto(int fd, const void* buf, int len, sockaddr_t* sa) {
    int ret;
    __asm__ __volatile__("int $0x80"
        : "=a"(ret)
        : "a"(SYS_SENDTO), "b"(fd), "c"(buf), "d"(len), "S"(sa));
    return ret;
}
static inline int sys_recvfrom(int fd, void* buf, int max, uint8_t* src_ip, uint16_t* src_port) {
    int ret;
    __asm__ __volatile__("int $0x80"
        : "=a"(ret)
        : "a"(SYS_RECVFROM), "b"(fd), "c"(buf), "d"(max), "S"(src_ip), "D"(src_port));
    return ret;
}

// clone: create a thread sharing this task's address space. entry runs in
// the child; child_stack is the child's initial user ESP (0 = the default
// per-slot stack); tls_base is the child's TCB (its %gs points there).
// Returns the child's TID to the parent and 0 to the child.
static inline int sys_clone(int entry, int child_stack, int tls_base) {
    return syscall(SYS_CLONE, entry, child_stack, tls_base);
}

// tls_set: install (or remove, base=0) the current thread's TLS. After this
// returns, %gs:0 reads the TCB at `base` (write self/fields before calling).
static inline int sys_tls_set(int tls_base) {
    return syscall(SYS_TLS_SET, tls_base, 0, 0);
}

// POSIX poll(): fill fds[i].revents and return the number of ready fds
// (0 on timeout, -1 on error). timeout_ms < 0 blocks forever.
static inline int sys_poll(pollfd_t* fds, int nfds, int timeout_ms) {
    return syscall(SYS_POLL, (int)fds, nfds, timeout_ms);
}

// POSIX select(): bitmaps are uint32_t (fd < 32). Returns ready count,
// 0 on timeout, -1 on error.
static inline int sys_select(int nfds, uint32_t* readfds, uint32_t* writefds,
                             uint32_t* exceptfds, int timeout_ms) {
    return syscall5(SYS_SELECT, nfds, (int)readfds, (int)writefds, (int)exceptfds, timeout_ms);
}

// POSIX getcwd()/chdir(): absolute path of the calling task's working
// directory, or change it (path may be relative or absolute).
static inline int sys_getcwd(char* buf, int size) {
    return syscall(SYS_GETCWD, (int)buf, size, 0);
}
static inline int sys_chdir(const char* path) {
    return syscall(SYS_CHDIR, (int)path, 0, 0);
}

// POSIX clock_gettime() (CLOCK_MONOTONIC only): uptime since boot.
static inline int sys_clock_gettime(int clock_id, timespec_t* ts) {
    return syscall(SYS_CLOCK_GETTIME, clock_id, (int)ts, 0);
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

// Resource limits (POSIX getrlimit/setrlimit subset, v38.28). resource is
// RLIMIT_NPROC/RLIMIT_AS/RLIMIT_NOFILE; rl is {cur, max}. setrlimit follows
// POSIX privilege rules: a non-root caller may lower cur and raise cur only
// up to max, but may never raise max; root may set anything. Both return
// 0 on success, -1 on error (bad resource / EPERM / EINVAL).
static inline int sys_getrlimit(int resource, rlimit_t* rl) {
    return syscall(SYS_GETRLIMIT, resource, (int)rl, 0);
}
static inline int sys_setrlimit(int resource, rlimit_t* rl) {
    return syscall(SYS_SETRLIMIT, resource, (int)rl, 0);
}

#endif
