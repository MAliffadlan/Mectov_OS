// src/sys/shell/builtins/app_launch/cmd_q3vm.c — the `q3vm` shell command.
// v38.105 (Q3 phase 5): runs the OFFICIAL Quake III Arena game module as
// Quake VM bytecode inside the kernel — qagame.qvm, built from id Software's
// own GPL source release by id's own lcc + q3asm, executed by id's own QVM
// interpreter (third_party/q3a/code/qcommon/vm.c, vm_interpreted.c).
//
// Headless by design: this is the engine+VM milestone, so the evidence is the
// serial log (the game module's own trap_Printf output, the loader's segment
// accounting, the trap counts it produced) rather than a window.
//
// Same fork-into-kernel-task pattern as `q3`, `q3gl` and `q3play`: the shell's
// SYS_EXEC_CMD context runs with interrupts disabled, which would starve the
// timer the engine's own clock needs. The dedicated task boots with IF=1.
// Built only with MECTOV_Q3=1.
#include "../../shell_internal.h"

#ifdef MECTOV_Q3
static void q3vm_task_entry(void) {
        extern void q3vm_start(void);
        q3vm_start();   /* never returns (parks in hlt when finished) */
}
#endif

void cmd_q3vm(void) {
#ifdef MECTOV_Q3
        extern int task_fork_kernel(void (*entry)(void), const char* child_arg);
        int pid = task_fork_kernel(q3vm_task_entry, "q3vm");
        if (pid < 0) {
                print("q3vm: failed to fork VM task\n", 0x0C);
                return;
        }
        print("Starting the official Quake III Arena game VM (id source)...\n", 0x0C);
        print("Bytecode built by id's lcc + q3asm; output goes to the serial log.\n", 0x07);
#else
        print("q3vm: engine not compiled in (build with MECTOV_Q3=1)\n", 0x0C);
#endif
}
