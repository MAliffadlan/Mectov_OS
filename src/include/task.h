#ifndef TASK_H
#define TASK_H

#include "types.h"

// Current CPU id (LAPIC id & 15); used by per-CPU code and lock ownership keys.
int task_get_cid(void);

// Priority levels
#define PRIORITY_REALTIME   2
#define PRIORITY_INTERACTIVE 1
#define PRIORITY_BACKGROUND  0

// Task states (shared with sync.c)
#define TASK_STATE_FREE    0
#define TASK_STATE_RUNNING 1
#define TASK_STATE_READY   2
#define TASK_STATE_SLEEP   3
#define TASK_STATE_BLOCKED 4
#define TASK_STATE_ZOMBIE  5   // exited, waiting for parent to waitpid()
#define TASK_STATE_STOPPED 6   // suspended by SIGSTOP/SIGTSTP, not scheduled

// Signal numbers (POSIX subset). SIGKILL and SIGSTOP cannot be caught or
// ignored; SIGCONT resumes a stopped task.
#define SIGINT   2
#define SIGFPE   8    // raised by #XM/#DE-class math faults (v38.41)
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21   // background pgrp read from controlling terminal (default: stop)
#define SIGTTOU  22   // background pgrp write to controlling terminal (default: stop)
#define SIG_MAX  32

// Resource limits (rlimit_t + RLIMIT_* are defined in types.h, the common
// base of both task.h and syscall.h). Enforced at the syscall layer:
// RLIMIT_NPROC at fork/clone/thread-create (counts live tasks with the
// caller's uid), RLIMIT_NOFILE at fd allocation, RLIMIT_AS at heap/mmap
// growth — see task_rlimit_* below.

// Get/set a resource limit of the CURRENT task (SYS_GETRLIMIT/SYS_SETRLIMIT).
// setrlimit follows POSIX: a non-root caller may lower cur and raise cur only
// up to max, but never raise max; root may set anything. Returns 0 or -1.
int task_getrlimit(int res, rlimit_t* out);
int task_setrlimit(int res, const rlimit_t* in);
// RLIMIT_NOFILE check at fd allocation: 1 = another fd is allowed, 0 = the
// task's soft NOFILE limit is reached (root bypasses). Called with fd_lock
// held by the fd layer; reads the current task's table without task_lock.
int task_rlimit_nofile_ok(void);
// RLIMIT_AS check for the current task: 1 = growing the address space by
// `additional` bytes stays under the soft AS limit (root bypasses).
int task_rlimit_as_allows(uint32_t additional);

// mmap() regions per task (demand paged): the region is reserved in VA space
// with no physical frames; a page fault inside it lazily materializes a frame
// — zero-filled for anonymous mappings, or file bytes read straight off the
// disk for file-backed mappings (see task_mmap_file). File-backed mappings
// are MAP_SHARED-style: the first write to a page marks it dirty (tracked via
// the RO→RW fault) and msync()/munmap() writes dirty pages back to the file.
// Max 8 concurrent mappings per task.
#define MMAP_MAX_REGIONS 8
#define MMAP_BASE        0x40000000u  // user VA region for mmap (above heap/shm)
#define MMAP_END         0x80000000u

// mmap flags (SYS_MMAP_FILE). Only shared file-backed mappings exist today:
// dirty pages write back to the VFS file on msync()/munmap().
#define MMAP_FILE_SHARED 1
// Device mapping (SYS_FB_MAP): pages are MMIO mapped eagerly with PAGE_DEV
// PTEs — no fault-in, no dirty tracking, never written back, skipped by the
// fork COW/refcount walker and the address-space free walkers.
#define MMAP_FLAG_DEVICE 2

typedef struct {
    uint32_t base;      // page-aligned start VA (0 = free slot)
    uint32_t size;      // page-aligned size in bytes
    uint32_t file_size; // file length at map time (bytes); 0 for anonymous
    int      vfs_node;  // VFS node backing a file mapping (-1 = anonymous)
    uint32_t map_flags; // 0=anonymous, MMAP_FILE_SHARED=file, MMAP_FLAG_DEVICE=MMIO
    uint8_t* dirty;     // per-page dirty bitmap (kmalloc'd, file mappings)
} mmap_region_t;

// Sentinel stored in signal_handlers[] for "ignore this signal". User space
// passes 1 (SIG_IGN) to sys_signal; the kernel stores this pointer instead of
// NULL (default) so a real handler at address 1 stays impossible.
#define SIG_IGN_SENTINEL ((void*)1)

// sigaction flags (Fase 1 signal hardening). SA_RESTART re-enters an
// interrupted blocking syscall (currently SYS_SLEEP) after the handler
// returns; SA_NODEFER keeps the signal itself unblocked while its handler
// runs (default: the delivered signal is auto-blocked during the handler).
#define SA_RESTART  1
#define SA_NODEFER  2

// Kernel stack guard pages: every task's 16KB kernel stack sits above a 4KB
// unmapped guard page; a stack overflow faults on it and the #PF handler
// panics with a clear message (see task.c). TASK_KSTACK_SIZE is the usable
// stack size (shared with /proc/tasks, which shows peak usage as a % of it).
#define TASK_KSTACK_SIZE 16384
int task_is_stack_guard(uint32_t addr);       // 1 if addr is in any guard page
uint32_t task_stack_top(int tid);             // top of task tid's kernel stack
void task_install_stack_guards(uint32_t page_dir); // unmap guards in a page dir

// sigprocmask operations (POSIX how values)
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

// Saved-context frame the kernel pushes onto the user stack before calling a
// signal handler. SYS_SIGRETURN restores execution from it.
typedef struct {
    uint32_t sig;
    uint32_t saved_eax, saved_ecx, saved_edx, saved_ebx;
    uint32_t saved_esi, saved_edi, saved_ebp;
    uint32_t saved_eip, saved_cs, saved_eflags, saved_esp, saved_ss;
    uint32_t saved_blocked;  // signal mask to restore when the handler returns
    uint32_t restart;        // 1 = SA_RESTART: re-enter the interrupted syscall
    uint32_t restart_arg;    // argument for the restarted syscall (sleep ticks)
} sigframe_t;

void init_tasking();
int create_task(void (*entry)());
int create_user_task(void (*entry)());
uint32_t schedule(uint32_t esp);
void task_exit(void);
int get_current_task(void);

// === NEW: Process model (fork / waitpid / signals) ===
// Fork the current Ring 3 task. Returns the child's tid to the parent and 0
// to the child. The child gets a COW copy of the address space, a byte copy
// of the kernel stack (so its syscall returns 0), the fd table (refcounts
// bumped) and the signal handlers.
int task_fork(void);
// Replace the current task's image with the program at `path` (POSIX exec).
// The old address space is freed, the fd table / current dir / parent survive,
// caught signal handlers reset to default (SIG_IGN stays), and the task's live
// syscall return frame is patched to enter the new entry point. `path` and
// `arg` must be kernel-side copies (the user pointers die with the old space).
// On success this never returns to the old image; on failure the task keeps
// running and returns a negative error.
int task_exec(const char* path, const char* arg, void* frame);
// Fork and redirect the child's return frame straight into a kernel function
// (never returns to user mode). Used by the shell for background commands.
// child_arg (optional) is written into the child's launch_arg BEFORE it can
// run, so the child can read its command from there.
int task_fork_kernel(void (*entry)(void), const char* child_arg);
// Fork the current task AND exec a new image in the child in one step: the
// child gets a FRESH address space built from `path` (no COW copy of the
// parent), its fd table is rewired so fd 0 reads from `in_fd` and fd 1 writes
// to `out_fd` (each only when >= 0), and its parent is set to the caller so
// the shell can waitpid() it. `path`/`arg` must be kernel-side copies.
// Returns the child tid (parent side), or a negative error.
int task_fork_exec(int in_fd, int out_fd, const char* path, const char* arg);
// Wait for a child. pid > 0 waits for that child, pid <= 0 any child.
// options bit 0 (WNOHANG) returns 0 instead of blocking. Returns the child
// tid on success, -1 if there are no children (ECHILD).
int task_waitpid(int pid, int* status, int options);
// Exit the current task with an exit status; children are reparented to the
// kernel task and the parent is woken if it is blocked in waitpid().
void task_exit_with_code(int code);
int task_get_ppid(void);
// Deliver a signal to a task. Default actions terminate (SIGCHLD is ignored
// by default); SIGKILL always terminates; a registered handler is marked
// pending and delivered at the next return-to-user.
int task_signal(int tid, int sig);
// Reap zombie tasks whose parent is dead or that have outlived the reap
// timeout (safety net for parents that never call waitpid()). Called from the
// BSP main loop once per second.
void task_reap_zombies(void);
// Deliver pending signals to the task whose return frame is `frame`
// (registers_t*). If a default-action signal terminates the task, the frame
// is patched to park in the kernel instead of iret'ing back to user code.
// Returns 1 if anything was delivered, 0 otherwise.
int task_deliver_signals(void* frame);
// Deliver a synchronous fault signal (e.g. SIGSEGV from an unresolvable user
// #PF) to the CURRENT task from exception context. Rewrites `frame` so a
// user-installed handler runs, or terminates the task (128+sig) on default
// action. Returns 1 when the frame was rewritten (handler or park).
int task_fault_signal(int sig, void* frame);
void task_set_signal_handler(int tid, int sig, void* h);
void* task_get_signal_handler(int tid, int sig);
// sigaction core: handler + sa_mask + flags. Backward-compatible with
// task_set_signal_handler (mask=0, flags=0).
void task_set_sigaction(int tid, int sig, void* h, uint32_t mask, uint32_t flags);
void task_get_sigaction(int tid, int sig, void** h, uint32_t* mask, uint32_t* flags);
// sigprocmask support: per-task blocked-signal bitmap
uint32_t task_get_blocked(int tid);
void task_set_blocked(int tid, uint32_t bits);
uint32_t task_get_sig_frame_esp(int tid);
void task_set_sig_frame_esp(int tid, uint32_t esp);
// Process groups & sessions (Fase 2): a process group is a set of tasks that
// receive terminal signals together; the foreground pgrp of the controlling
// terminal owns reads/writes. Default pgrp = shell pgrp (inherited); a task
// becomes its own group leader with setpgid(pid,0)/setsid().
int task_get_pgrp(int tid);
int task_set_pgrp(int tid, int pgrp);
int task_get_session(int tid);
void task_set_session(int tid, int session);
int task_get_parent(int tid);
// Unix user id (v38.23): 0 = root (kernel tasks), 1000 = the logged-in user.
// Inherited by fork/exec/thread_create; set to USER_UID by load_mct_app for
// Ring 3 apps so permission checks see a non-root caller.
int task_get_uid(int tid);
void task_set_uid(int tid, int uid);
int task_get_gid(int tid);
void task_set_gid(int tid, int gid);
// Foreground process group of the controlling terminal (0 = none).
int task_get_fg_pgrp(void);
void task_set_fg_pgrp(int pgrp);
// Send a signal to every task in `pgrp` (POSIX process-group semantics for
// Ctrl+C/Ctrl+Z). Returns the number signalled, -1 on a bad group.
int task_signal_pgrp(int pgrp, int sig);
// 1 if `tid` is in the background (has a pgrp that is not the controlling
// terminal's foreground pgrp). Returns 0 when there is no controlling
// terminal or the task has no group.
int task_is_background(int tid);

// === NEW: Thread & Priority API ===
// Create a task with explicit priority and optional page directory
// page_dir = 0 means use global identity map
int thread_create(void (*entry)(), int priority, uint32_t page_dir);
// Extended create (v38.24): shares the caller's address space like
// thread_create, plus an optional explicit child user stack (0 = the default
// per-slot stack) and an optional TLS base (0 = no TLS). With a nonzero
// tls_base the new task's FS/GS descriptors (all CPUs) point at it, so
// Ring 3 %gs accesses resolve to the thread control block. Caller must have
// interrupts disabled (same discipline as thread_create).
int thread_create_ex(void (*entry)(), int priority, uint32_t page_dir,
                     uint32_t child_stack, uint32_t tls_base);
// Set the CURRENT task's TLS base (SYS_TLS_SET): updates its GDT descriptors
// and the pending return frame so the caller immediately runs with %gs
// pointing at the TCB. base 0 removes TLS.
int task_set_tls(uint32_t base);

// Sleep the current task for N timer ticks
void task_sleep(int ticks);

// Get/set priority of a task
int task_set_priority(int tid, int priority);
int task_get_priority(int tid);

// Get/set page directory for a task
uint32_t task_get_page_dir(int tid);
// 1 if the task runs on the kernel's global address space. Task 0 holds the
// boot CR3 rather than 0, so never test page_dir == 0 directly.
int task_in_kernel_space(int tid);
void task_set_page_dir(int tid, uint32_t page_dir);

// Wake up a sleeping task (used by timer)
void task_wake(int tid);

// Blocked-state accessors for sync.c (semaphores/futexes)
int task_get_state(int tid);
void task_set_state(int tid, int state);

// Check if a task is alive (a ZOMBIE is not alive)
int task_is_alive(int tid);

// Kill a specific task by ID (from kernel, e.g. Ctrl+C)
int task_kill(int tid);

// File Descriptor accessors
int task_get_fd(int tid, int local_fd);
void task_set_fd(int tid, int local_fd, int global_fd);

// === NEW: Heap Pointer for Demand Paging ===
uint32_t task_get_heap_ptr(int tid);
void task_set_heap_ptr(int tid, uint32_t ptr);

// === NEW: Shared memory attachment bitmap ===
// Bit (shmid-1) set while the task has that segment mapped via shmat(). Kept
// in the task so exit can release segments a task died without shmdt()ing.
uint32_t task_get_shm_bits(int tid);
void task_set_shm_bits(int tid, uint32_t bits);

// === NEW: Task Info for GUI ===
typedef struct {
    int id;
    int state;
    int ring;
    int priority;
    int sleep_ticks;
    int stack_watermark;  // peak kernel-stack bytes used (see task.c scheduler)
} task_info_t;

int get_task_info(int tid, task_info_t* info);
void task_set_launch_arg(int tid, const char* arg);
const char* task_get_launch_arg(int tid);

// Trusted shell-host flag (v38.53): kernel-computed from the resolved image
// identity + parent trust; gates SYS_EXEC_CMD / SYS_KILL_TASK. Never derive
// it from the user-controlled launch_arg string.
int task_grant_trusted_shell(int parent_tid, const char* image_name);
int task_is_trusted_shell(int tid);
void task_set_trusted_shell(int tid, int trusted);
// Enumerate live tasks: returns the first live task with id > `after` and
// fills `info` (like get_task_info), or -1 when no more tasks exist. Start
// with after = -1 to include the kernel task (tid 0). Used by /proc/tasks.
int task_enum(int after, task_info_t* info);

#endif
