// src/sys/shell/builtins/app_launch/cmd_q3gl.c — the `q3gl` shell command.
// v38.102: TinyGL software-GL demo (Q3 phase 2). Same fork-into-kernel-task
// pattern as `q3` (v38.99): the shell's SYS_EXEC_CMD syscall context runs
// with interrupts disabled, and the renderer's frame-rate limiter needs the
// timer — so the shell forks a dedicated kernel task and returns at once,
// leaving the desktop fully interactive while the gears spin in a window.
// Built only with MECTOV_Q3=1.
#include "../../shell_internal.h"

#ifdef MECTOV_Q3
static void q3gl_task_entry(void) {
        extern void q3gl_start(void);
        q3gl_start();   /* never returns (parks in hlt after cleanup) */
}
#endif

void cmd_q3gl(void) {
#ifdef MECTOV_Q3
        extern int task_fork_kernel(void (*entry)(void), const char* child_arg);
        int pid = task_fork_kernel(q3gl_task_entry, "q3gl");
        if (pid < 0) {
                print("q3gl: failed to fork renderer task\n", 0x0C);
                return;
        }
        print("Starting TinyGL gears (dedicated kernel task)...\n", 0x0C);
#else
        print("q3gl: renderer not compiled in (build with MECTOV_Q3=1)\n", 0x0C);
#endif
}
