// src/sys/shell/builtins/app_launch/cmd_q3.c — the `q3` shell command.
// v38.99: the engine does NOT run inline inside the shell's SYS_EXEC_CMD
// syscall — that context runs with interrupts disabled (IF=0) for the whole
// command lifetime, which starves the PIT timer and freezes Com_Frame's
// frame-wait loop (Com_TimeVal never advances). Instead the shell forks a
// dedicated kernel task (task_fork_kernel entries boot with EFLAGS 0x202,
// IF=1, fully preemptible) and returns immediately, so the terminal prompt
// stays responsive and the engine owns its own interrupt-enabled context.
// Built only with MECTOV_Q3=1.
#include "../../shell_internal.h"

#ifdef MECTOV_Q3
static void q3_task_entry(void) {
        extern void q3_start(void);
        q3_start();   /* never returns (Sys_Quit / Sys_Error hlt on exit) */
}
#endif

void cmd_q3(void) {
#ifdef MECTOV_Q3
        extern int task_fork_kernel(void (*entry)(void), const char* child_arg);
        int pid = task_fork_kernel(q3_task_entry, "q3");
        if (pid < 0) {
                print("q3: failed to fork engine task\n", 0x0C);
                return;
        }
        print("Starting Quake III engine core (dedicated kernel task)...\n", 0x0C);
#else
        print("q3: engine not compiled in (build with MECTOV_Q3=1)\n", 0x0C);
#endif
}
