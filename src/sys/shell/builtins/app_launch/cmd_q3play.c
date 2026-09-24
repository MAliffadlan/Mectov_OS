// src/sys/shell/builtins/app_launch/cmd_q3play.c — the `q3play` shell command.
// v38.103 (Q3 phase 3): boots the ioquake3 client layer — CL_Init/CL_Frame —
// against the TinyGL renderer in a WM window, with the window manager routing
// keyboard scancodes and captured mouse motion into the engine's event queue.
//
// Same fork-into-kernel-task pattern as `q3` (v38.99) and `q3gl` (v38.102):
// the shell's SYS_EXEC_CMD context runs with interrupts disabled, while the
// client's frame pacing (the engine's com_maxfps limiter calls NET_Sleep,
// which halts) needs the timer alive. The shell forks a dedicated kernel task
// and returns immediately, so the desktop stays interactive around the
// window.
// Built only with MECTOV_Q3=1.
#include "../../shell_internal.h"

#ifdef MECTOV_Q3
static void q3play_task_entry(void) {
        extern void q3play_start(void);
        q3play_start();   /* never returns (parks in hlt after shutdown) */
}
#endif

void cmd_q3play(void) {
#ifdef MECTOV_Q3
        extern int task_fork_kernel(void (*entry)(void), const char* child_arg);
        int pid = task_fork_kernel(q3play_task_entry, "q3play");
        if (pid < 0) {
                print("q3play: failed to fork client task\n", 0x0C);
                return;
        }
        print("Starting Quake III client (TinyGL window)...\n", 0x0C);
        print("WASD move, mouse look, ESC quits.\n", 0x07);
#else
        print("q3play: client not compiled in (build with MECTOV_Q3=1)\n", 0x0C);
#endif
}
