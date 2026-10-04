// src/sys/shell/builtins/app_launch/cmd_q3menu.c — the `q3menu` shell command.
// v38.150: this now opens id's OWN Quake III Arena main menu: the classic 1.32
// UI module (code/q3_ui) compiled to bytecode by id's own lcc + q3asm
// (scripts/build_qvm_ui.sh -> build/vm/q3ui.qvm), executed by the kernel with
// id's own interpreter, drawing its own menu/art pictures into a software 2D
// surface and asking this port to start a map when the player picks one.
// The driver is third_party/q3mectov/q3_uiwin.c; see q3_uivm.h for what the
// traps do and what is deliberately inert (sound, the 3D player-model window,
// multiplayer, cinematics).
//
// The Mectov-styled menu that used to live here is kept as `q3menunative`
// (src/apps/q3menu_app.c): a fallback for the case where q3ui.qvm has not been
// built or staged. Same fork-into-own-task pattern as `q3arena`, for the same
// reason — the shell's SYS_EXEC_CMD context runs with interrupts disabled,
// which would starve the clock the frame pacing reads.
#include "../../shell_internal.h"

#ifdef MECTOV_Q3
extern void q3ui_start(void);

static void q3menu_task_entry(void) {
        q3ui_start();        /* never returns (parks in hlt when finished) */
}

static void q3menu_launch(const char *what) {
        extern int task_fork_kernel(void (*entry)(void), const char *child_arg);
        if (task_fork_kernel(q3menu_task_entry, what) < 0) {
                print("q3menu: failed to fork the menu task\n", 0x0C);
                return;
        }
        print("Quake III Arena: ", 0x0B);
        print(what, 0x0B);
        print("\n", 0x0B);
}
#endif

void cmd_q3menu(void) {
#ifdef MECTOV_Q3
        /* A game already running owns the engine's global state (see
         * q3_uiwin.c), and the menu task says so on the serial line; report it
         * here, where the user is typing, rather than letting the window fail
         * to appear. */
        extern int q3arena_status(char *buf, int n);
        char st[96];
        if (q3arena_status(st, (int)sizeof(st)) >= 2) {
                print("q3menu: a game is already running — ESC out of it first\n", 0x0C);
                return;
        }
        q3menu_launch("q3menu");
#else
        print("q3menu: engine not compiled in (build with MECTOV_Q3=1)\n", 0x0C);
#endif
}

/* The old Mectov-styled menu, still built, still reachable, no longer the
 * default: the official interface is what `q3menu` opens now. */
void cmd_q3menunative(void) {
#ifdef MECTOV_Q3
        open_q3menu_app();
#else
        print("q3menunative: engine not compiled in (build with MECTOV_Q3=1)\n", 0x0C);
#endif
}