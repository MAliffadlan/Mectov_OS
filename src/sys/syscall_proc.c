#include "../include/idt.h"
#include "../include/syscall.h"
#include "../include/task.h"
#include "../include/vfs.h"
#include "../include/net.h"
#include "../include/wm.h"
#include "../include/ipc.h"
#include "../include/serial.h"
#include "../include/utils.h"
#include "../include/keyboard.h"
#include "../include/mouse.h"
#include "../include/fd.h"
#include "../include/shell.h"
#include "../include/vmm.h"   // USER_STACK_BOTTOM
#include "../include/task.h"   // SIG_MAX, SIG_IGN_SENTINEL, sigframe_t

extern int validate_user_ptr(const void* ptr, uint32_t size);
extern int safe_strlen(const char* s, int max);
extern void print(const char* s, uint8_t color);

extern int get_win_index(int wid);
extern void push_event(int wid, int type, int x, int y, int key);
extern void win_draw_cb(int id, int cx, int cy, int cw, int ch);
extern void win_key_cb(int id, char c, uint8_t sc);
extern void win_mouse_cb(int id, int cx, int cy, int btn);

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

typedef struct {
    int type; // 1 = rect, 2 = text
    int x, y, w, h;
    uint32_t color;
    char text[128];
} draw_cmd_t;

#define MAX_DRAW_CMDS 512
typedef struct {
    draw_cmd_t cmds[MAX_DRAW_CMDS];
    int count;
    int pending_count;
    draw_cmd_t pending_cmds[MAX_DRAW_CMDS];
} win_canvas_t;

extern win_event_queue_t win_queues[];
extern win_canvas_t win_canvases[];

// Basename comparison for the privileged-app whitelist. The old substring check
// let ANY app whose path merely CONTAINED "terminal.mct" (e.g. faketerminal.mct)
// run SYS_KILL_TASK / SYS_EXEC_CMD. Only the exact filename component matches.
static int launch_arg_is(const char* arg, const char* name) {
    const char* base = arg;
    for (const char* p = arg; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    return strcmp(base, name) == 0;
}

uint32_t handle_syscall_proc(registers_t* regs) {
    switch (regs->eax) {
        // ----- SYS_THREAD_CREATE (18) -----
        case SYS_THREAD_CREATE: {
            void (*entry)() = (void (*)())regs->ebx;
            int priority = (int)regs->ecx;

            // EDX is deliberately ignored. It used to be the new task's page
            // directory, i.e. Ring 3 chose the value the scheduler would load
            // into CR3 — pointing it at a page directory the app authored in
            // its own memory handed it the whole machine, and pointing it at
            // junk triple-faulted the box. A thread always shares its
            // creator's address space.
            int caller = get_current_task();

            // The entry point has to be code the caller can actually execute.
            uint32_t eip = (uint32_t)regs->ebx;
            if (task_in_kernel_space(caller) || eip < 0x08000000 || eip >= USER_STACK_BOTTOM) {
                regs->eax = (uint32_t)-1;
                break;
            }

            int tid = thread_create(entry, priority, task_get_page_dir(caller));
            __asm__ volatile("sti"); // thread_create leaves interrupts disabled
            regs->eax = (uint32_t)tid;
            write_serial_string("[SYS] thread_create -> TID=");
            write_serial('0' + (tid < 0 ? 0 : tid));
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

        // ----- SYS_GET_TASKS (46) -----
        case SYS_GET_TASKS: {
            sys_task_info_t* array = (sys_task_info_t*)regs->ebx;
            int max_count = (int)regs->ecx;
            if (!validate_user_ptr(array, sizeof(sys_task_info_t) * max_count)) { regs->eax = (uint32_t)-1; break; }
            int count = 0;
            for (int i = 0; i < 64 && count < max_count; i++) { // MAX_TASKS is 64 in task.h
                task_info_t info;
                if (get_task_info(i, &info)) {
                    array[count].id = info.id;
                    array[count].ring = info.ring;
                    array[count].state = info.state;
                    array[count].priority = info.priority;
                    count++;
                }
            }
            regs->eax = count;
            break;
        }

        // ----- SYS_KILL_TASK (48) -----
        case SYS_KILL_TASK: {
            int tid = (int)regs->ebx;
            if (tid <= 0) { regs->eax = (uint32_t)-1; break; }
            
            // Privilege check
            int caller_tid = get_current_task();
            if (!task_in_kernel_space(caller_tid)) {
                const char* launch_arg = task_get_launch_arg(caller_tid);
                if (!launch_arg_is(launch_arg, "terminal.mct") && 
                    !launch_arg_is(launch_arg, "explorer.mct") && 
                    !launch_arg_is(launch_arg, "taskmgr.mct")) {
                    write_serial_string("[SYS] Access denied for SYS_KILL_TASK from TID=");
                    write_serial_hex(caller_tid);
                    write_serial_string(" (");
                    write_serial_string(launch_arg);
                    write_serial_string(")\n");
                    regs->eax = (uint32_t)-1;
                    break;
                }
            }
            regs->eax = (uint32_t)task_kill(tid);
            break;
        }
        // ----- SYS_FORK (71): clone the current task (COW address space) -----
        case SYS_FORK: {
            extern int task_fork(void);
            regs->eax = (uint32_t)task_fork();
            break;
        }

        // ----- SYS_WAITPID (72) -----
        case SYS_WAITPID: {
            int pid = (int)regs->ebx;
            int* status = (int*)regs->ecx;
            int options = (int)regs->edx;
            if (!validate_user_ptr(status, sizeof(int))) { regs->eax = (uint32_t)-1; break; }
            extern int task_waitpid(int pid, int* status, int options);
            regs->eax = (uint32_t)task_waitpid(pid, status, options);
            break;
        }

        // ----- SYS_KILL (73): send a signal to a task -----
        case SYS_KILL: {
            int pid = (int)regs->ebx;
            int sig = (int)regs->ecx;
            if (pid <= 0) { regs->eax = (uint32_t)-1; break; }
            extern int task_signal(int tid, int sig);
            regs->eax = (uint32_t)task_signal(pid, sig);
            break;
        }

        // ----- SYS_SIGNAL (74): set a signal handler -----
        case SYS_SIGNAL: {
            int sig = (int)regs->ebx;
            uint32_t handler = (uint32_t)regs->ecx;
            if (sig <= 0 || sig >= SIG_MAX || sig == SIGKILL) {
                regs->eax = (uint32_t)-1;   // SIGKILL cannot be caught or ignored
                break;
            }
            // handler must be NULL (default), SIG_IGN (1), or user-space code
            if (handler != 0 && handler != SIG_IGN &&
                !(handler >= 0x08000000 && handler < USER_STACK_BOTTOM)) {
                regs->eax = (uint32_t)-1;
                break;
            }
            int tid = get_current_task();
            uint32_t old = (uint32_t)task_get_signal_handler(tid, sig);
            task_set_signal_handler(tid, sig, (void*)handler);
            regs->eax = old;    // 0 = default, 1 = SIG_IGN, else previous handler
            break;
        }

        // ----- SYS_SIGACTION (86): set handler + mask + flags (POSIX) -----
        // EBX=sig, ECX=&user_sigaction_t (in, optional), EDX=&user_sigaction_t (old, optional)
        case SYS_SIGACTION: {
            typedef struct { uint32_t handler; uint32_t mask; uint32_t flags; } user_sigaction_t;
            int sig = (int)regs->ebx;
            user_sigaction_t* act = (user_sigaction_t*)regs->ecx;
            user_sigaction_t* old = (user_sigaction_t*)regs->edx;
            if (sig <= 0 || sig >= SIG_MAX || sig == SIGKILL) {
                regs->eax = (uint32_t)-1;   // SIGKILL cannot be caught or ignored
                break;
            }
            if (old && !validate_user_ptr(old, sizeof(user_sigaction_t))) { regs->eax = (uint32_t)-1; break; }
            if (act && !validate_user_ptr(act, sizeof(user_sigaction_t))) { regs->eax = (uint32_t)-1; break; }

            int tid = get_current_task();
            void* oh; uint32_t om, of;
            task_get_sigaction(tid, sig, &oh, &om, &of);
            if (old) {
                old->handler = (uint32_t)(uintptr_t)oh;
                old->mask = om;
                old->flags = of;
            }
            if (act) {
                uint32_t handler = act->handler;
                // handler must be 0 (default), 1 (SIG_IGN), or user-space code
                if (handler != 0 && handler != 1 &&
                    !(handler >= 0x08000000 && handler < USER_STACK_BOTTOM)) {
                    regs->eax = (uint32_t)-1;
                    break;
                }
                void* h = (handler == 1) ? SIG_IGN_SENTINEL : (void*)(uintptr_t)handler;
                task_set_sigaction(tid, sig, h, act->mask, act->flags & (SA_RESTART | SA_NODEFER));
            }
            regs->eax = 0;
            break;
        }

        // ----- SYS_SIGPROCMASK (87): block/unblock/set the signal mask -----
        // EBX=how (0=SIG_BLOCK,1=SIG_UNBLOCK,2=SIG_SETMASK), ECX=&newset (optional),
        // EDX=&oldset (optional). SIGKILL/SIGSTOP/SIGCONT bits are ignored.
        case SYS_SIGPROCMASK: {
            int how = (int)regs->ebx;
            uint32_t* newset = (uint32_t*)regs->ecx;
            uint32_t* oldset = (uint32_t*)regs->edx;
            if (newset && !validate_user_ptr(newset, 4)) { regs->eax = (uint32_t)-1; break; }
            if (oldset && !validate_user_ptr(oldset, 4)) { regs->eax = (uint32_t)-1; break; }

            int tid = get_current_task();
            uint32_t cur = task_get_blocked(tid);
            if (oldset) *oldset = cur;
            if (newset) {
                uint32_t m = *newset;
                m &= ~((1u << SIGKILL) | (1u << SIGSTOP) | (1u << SIGCONT)); // never blockable
                if (how == SIG_BLOCK)      cur |= m;
                else if (how == SIG_UNBLOCK) cur &= ~m;
                else /* SIG_SETMASK */    cur = m;
                task_set_blocked(tid, cur);
            }
            regs->eax = 0;
            break;
        }

        // ----- SYS_SETPGID (88): join or create a process group -----
        // EBX=pid (0=self), ECX=pgid (0 = pid). Self is always allowed; a
        // parent may regroup its own child (POSIX allows a shell to set the
        // child's group before it execs).
        case SYS_SETPGID: {
            int pid = (int)regs->ebx;
            int pgid = (int)regs->ecx;
            int self = get_current_task();
            extern int task_get_parent(int);
            extern int task_get_pgrp(int);
            extern int task_set_pgrp(int, int);
            int target = (pid <= 0) ? self : pid;
            if (target != self && task_get_parent(target) != self) {
                regs->eax = (uint32_t)-1;
                break;
            }
            int new_pgid = (pgid <= 0) ? target : pgid;
            if (task_set_pgrp(target, new_pgid) < 0) { regs->eax = (uint32_t)-1; break; }
            write_serial_string("[SIG] setpgid tid=");
            write_serial_hex(target);
            write_serial_string(" pgrp=");
            write_serial_hex(new_pgid);
            write_serial_string("\n");
            regs->eax = 0;
            break;
        }

        // ----- SYS_GETPGRP (89) -----
        case SYS_GETPGRP: {
            extern int task_get_pgrp(int);
            regs->eax = (uint32_t)task_get_pgrp(get_current_task());
            break;
        }

        // ----- SYS_SETSID (90): start a new session; this task becomes both
        // the session leader and the group leader of its own new group. -----
        case SYS_SETSID: {
            int self = get_current_task();
            extern int task_get_session(int);
            if (task_get_session(self) == self) {
                regs->eax = (uint32_t)-1;   // already a session leader
                break;
            }
            extern int task_set_pgrp(int, int);
            task_set_pgrp(self, self);
            extern void task_set_session(int, int);
            task_set_session(self, self);
            write_serial_string("[SIG] setsid tid=");
            write_serial_hex(self);
            write_serial_string(" session=");
            write_serial_hex(self);
            write_serial_string("\n");
            regs->eax = (uint32_t)self;
            break;
        }

        // ----- SYS_TCSETPGRP (91): set the controlling terminal's foreground
        // process group (the shell does this when it takes back the terminal).
        case SYS_TCSETPGRP: {
            int pgrp = (int)regs->ecx;
            extern void task_set_fg_pgrp(int);
            task_set_fg_pgrp(pgrp);
            write_serial_string("[SIG] tcsetpgrp fg=");
            write_serial_hex(pgrp);
            write_serial_string("\n");
            regs->eax = 0;
            break;
        }

        // ----- SYS_TCGETPGRP (92) -----
        case SYS_TCGETPGRP: {
            extern int task_get_fg_pgrp(void);
            regs->eax = (uint32_t)task_get_fg_pgrp();
            break;
        }

        // ----- SYS_SIGRETURN (75): restore context after a handler -----
        case SYS_SIGRETURN: {
            int tid = get_current_task();
            uint32_t esp = task_get_sig_frame_esp(tid);
            if (esp == 0 || !validate_user_ptr((void*)esp, sizeof(sigframe_t))) {
                task_set_sig_frame_esp(tid, 0);
                regs->eax = (uint32_t)-1;
                break;
            }
            sigframe_t* f = (sigframe_t*)esp;
            // Restore the pre-handler signal mask first: SA_RESTART re-parks
            // the task in SYS_SLEEP below, and that sleep must run under the
            // original mask (e.g. SIGKILL-able, signals delivered normally).
            extern uint32_t task_get_blocked(int);
            extern void task_set_blocked(int, uint32_t);
            task_set_blocked(tid, f->saved_blocked);

            regs->eax  = f->saved_eax;
            regs->ecx  = f->saved_ecx;
            regs->edx  = f->saved_edx;
            regs->ebx  = f->saved_ebx;
            regs->esi  = f->saved_esi;
            regs->edi  = f->saved_edi;
            regs->ebp  = f->saved_ebp;
            regs->eip  = f->saved_eip;
            regs->cs   = f->saved_cs;
            regs->eflags = f->saved_eflags;
            regs->useresp = f->saved_esp;
            regs->ss   = f->saved_ss;
            task_set_sig_frame_esp(tid, 0);

            // SA_RESTART semantics: re-enter the interrupted SYS_SLEEP with
            // the remaining ticks before returning to user mode, so the total
            // sleep duration is preserved across the handler.
            if (f->restart && f->restart_arg > 0) {
                extern void task_sleep(int);
                write_serial_string("[SIG] SA_RESTART re-entering SYS_SLEEP ticks=");
                write_serial_hex(f->restart_arg);
                write_serial_string("\n");
                task_sleep((int)f->restart_arg);
                regs->eax = 0;   // SYS_SLEEP completes normally
            }
            break;
        }

        // ----- SYS_GETPPID (76) -----
        case SYS_GETPPID: {
            extern int task_get_ppid(void);
            regs->eax = (uint32_t)task_get_ppid();
            break;
        }

        // ----- SYS_EXEC (77): replace this task's image -----
        case SYS_EXEC: {
            const char* path = (const char*)regs->ebx;
            const char* arg  = (const char*)regs->ecx;

            // Validate + copy path and arg into KERNEL memory first: task_exec()
            // frees the old address space, so any user pointer handed to it would
            // be dangling by the time the VFS read runs.
            int plen = safe_strlen(path, 128);
            if (plen < 0) { regs->eax = (uint32_t)-1; break; }
            if (!validate_user_ptr(path, (uint32_t)plen + 1)) {
                regs->eax = (uint32_t)-1; break;
            }
            char kpath[128];
            for (int i = 0; i < plen; i++) kpath[i] = path[i];
            kpath[plen] = '\0';

            char karg[128] = {0};
            if (arg) {
                int alen = safe_strlen(arg, 128);
                if (alen >= 0 && validate_user_ptr(arg, (uint32_t)alen + 1)) {
                    for (int i = 0; i < alen; i++) karg[i] = arg[i];
                    karg[alen] = '\0';
                }
            }

            extern int task_exec(const char* path, const char* arg, void* frame);
            regs->eax = (uint32_t)task_exec(kpath, karg, regs);
            break;
        }

        // ----- SYS_GET_LAUNCH_ARG (49) -----
        case SYS_GET_LAUNCH_ARG: {
            char* user_buf = (char*)regs->ebx;
            int max_len = (int)regs->ecx;
            if (max_len <= 0 || !validate_user_ptr(user_buf, max_len)) {
                write_serial_string("[LAUNCH_ARG] bad ptr\n");
                regs->eax = (uint32_t)-1; break;
            }
            extern const char* task_get_launch_arg(int tid);
            const char* arg = task_get_launch_arg(get_current_task());
            write_serial_string("[LAUNCH_ARG] tid=");
            write_serial_hex(get_current_task());
            write_serial_string(" arg='");
            write_serial_string(arg);
            write_serial_string("'\n");
            int i = 0;
            for (; i < max_len - 1 && arg[i]; i++) {
                user_buf[i] = arg[i];
            }
            user_buf[i] = '\0';
            regs->eax = (uint32_t)i;
            break;
        }

        // ----- SYS_EXEC_CMD (45) -----
        case SYS_EXEC_CMD: {
            const char* cmd_str = (const char*)regs->ebx;
            if (safe_strlen(cmd_str, CMD_BUF_SIZE) < 0) { regs->eax = (uint32_t)-1; break; }
            
            // Privilege check
            int caller_tid = get_current_task();
            if (!task_in_kernel_space(caller_tid)) {
                const char* launch_arg = task_get_launch_arg(caller_tid);
                if (!launch_arg_is(launch_arg, "terminal.mct") && 
                    !launch_arg_is(launch_arg, "explorer.mct")) {
                    write_serial_string("[SYS] Access denied for SYS_EXEC_CMD from TID=");
                    write_serial_hex(caller_tid);
                    write_serial_string(" (");
                    write_serial_string(launch_arg);
                    write_serial_string(")\n");
                    regs->eax = (uint32_t)-1;
                    break;
                }
            }
            
            extern char cmd_b[CMD_BUF_SIZE];
            extern int b_idx;
            // Serialize shell command execution across terminals: the shell's
            // global state (cmd_b, env, aliases, history) is shared, so two
            // terminals on two cores must not run ex_cmd() concurrently
            // (kernel locking audit v38.4). ex_cmd() internally drops the lock
            // across blocking ops (sleep/waitpid) so a killed shell never
            // strands it.
            extern void shell_lock_acquire(void);
            extern void shell_lock_release(void);
            shell_lock_acquire();
            int len2 = 0;
            while (cmd_str[len2] && len2 < CMD_BUF_SIZE - 1) {
                cmd_b[len2] = cmd_str[len2];
                len2++;
            }
            cmd_b[len2] = '\0';
            b_idx = len2;
            extern int stdout_ipc_qid;
            int saved_qid = stdout_ipc_qid;
            extern int use_term_buf;
            use_term_buf = 0;
            extern void ex_cmd(void);
            ex_cmd();
            extern void vga_flush_ipc(void);
            vga_flush_ipc();
            stdout_ipc_qid = saved_qid;
            shell_lock_release();
            regs->eax = 0;
            break;
        }
    }
    return regs->eax;
}
