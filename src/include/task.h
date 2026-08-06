#ifndef TASK_H
#define TASK_H

#include "types.h"

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

// Signal numbers (POSIX subset). SIGKILL cannot be caught or ignored.
#define SIGINT   2
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGTERM  15
#define SIGCHLD  17
#define SIG_MAX  32

// Sentinel stored in signal_handlers[] for "ignore this signal". User space
// passes 1 (SIG_IGN) to sys_signal; the kernel stores this pointer instead of
// NULL (default) so a real handler at address 1 stays impossible.
#define SIG_IGN_SENTINEL ((void*)1)

// Saved-context frame the kernel pushes onto the user stack before calling a
// signal handler. SYS_SIGRETURN restores execution from it.
typedef struct {
    uint32_t sig;
    uint32_t saved_eax, saved_ecx, saved_edx, saved_ebx;
    uint32_t saved_esi, saved_edi, saved_ebp;
    uint32_t saved_eip, saved_cs, saved_eflags, saved_esp, saved_ss;
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
// Fork and redirect the child's return frame straight into a kernel function
// (never returns to user mode). Used by the shell for background commands.
// child_arg (optional) is written into the child's launch_arg BEFORE it can
// run, so the child can read its command from there.
int task_fork_kernel(void (*entry)(void), const char* child_arg);
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
void task_set_signal_handler(int tid, int sig, void* h);
void* task_get_signal_handler(int tid, int sig);
uint32_t task_get_sig_frame_esp(int tid);
void task_set_sig_frame_esp(int tid, uint32_t esp);

// === NEW: Thread & Priority API ===
// Create a task with explicit priority and optional page directory
// page_dir = 0 means use global identity map
int thread_create(void (*entry)(), int priority, uint32_t page_dir);

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

// === NEW: Task Info for GUI ===
typedef struct {
    int id;
    int state;
    int ring;
    int priority;
    int sleep_ticks;
} task_info_t;

int get_task_info(int tid, task_info_t* info);
void task_set_launch_arg(int tid, const char* arg);
const char* task_get_launch_arg(int tid);

#endif
