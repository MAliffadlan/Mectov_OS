// src/sys/shell/builtins/process_ops/cmd_renice.c — the `renice` shell command.
// v38.91: adjust a task's nice value (-20 highest prio .. 19 lowest).
//   renice <nice>        — renice the shell itself
//   renice <nice> <pid>  — renice task <pid>
// Raising another task's priority (lower nice) requires root (uid 0) —
// enforced by task_set_nice().
#include "../../shell_internal.h"

void cmd_renice_arg(void) {
        char* args = cmd_b + 7;   // past "renice "
        while (*args == ' ') args++;
        if (*args == '\0') {
                print("usage: renice <nice> [pid]   (nice: -20..19, lower = higher priority)\n", 0x0C);
                return;
        }
        int nice = atoi(args);
        // Skip the nice token, find the optional pid.
        if (*args == '-') args++;
        while (*args >= '0' && *args <= '9') args++;
        while (*args == ' ') args++;

        int me = get_current_task();
        int caller_uid = task_get_uid(me);
        if (*args == '\0') {
                int rc = task_set_nice(me, nice, caller_uid);
                if (rc == 0) {
                        print("renice: self -> ", 0x0F); p_int(nice, 0x0F); print("\n", 0x0F);
                } else if (rc == -2) {
                        print("renice: raising priority requires root\n", 0x0C);
                } else {
                        print("renice: failed\n", 0x0C);
                }
                return;
        }
        int tid = atoi(args);
        int rc = task_set_nice(tid, nice, caller_uid);
        if (rc == 0) {
                print("renice: pid ", 0x0F); p_int(tid, 0x0F);
                print(" -> ", 0x0F); p_int(nice, 0x0F); print("\n", 0x0F);
        } else if (rc == -3) {
                print("renice: no such process\n", 0x0C);
        } else if (rc == -2) {
                print("renice: raising priority requires root\n", 0x0C);
        } else {
                print("renice: failed\n", 0x0C);
        }
}
