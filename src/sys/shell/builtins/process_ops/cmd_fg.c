// src/sys/shell/builtins/process_ops/cmd_fg.c — the `fg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_fg(void) {
        int num = (cmd_b[2] == ' ') ? atoi(cmd_b + 3) : -1;
        if (num <= 0) {
            print("Usage: fg [job_number]  — wait for a background job\n", 0x0E);
        } else {
            int t = find_job_tid(num);
            if (t < 0) {
                print("fg: no such job\n", 0x0C);
            } else {
                extern int task_waitpid(int pid, int* status, int options);
                extern int task_signal(int tid, int sig);
                extern int task_get_state(int tid);
                int st = -1;
                // Resume a Ctrl+Z-suspended job before waiting on it.
                if (task_get_state(t) == TASK_STATE_STOPPED) {
                    task_signal(t, SIGCONT);
                    write_serial_string("[JOBS] fg SIGCONT tid=");
                    write_serial_hex(t);
                    write_serial_string("\n");
                }
                // Drop shell_lock while parked (kernel locking audit v38.4).
                shell_lock_release_for_block();
                int r = task_waitpid(t, &st, 0);
                shell_lock_reacquire();
                if (r >= 0) {
                    print("[", 0x0E); p_int(num, 0x0E); print("] Done", 0x0A);
                    print(" (exit ", 0x07); p_int(st, 0x07); print(")\n", 0x07);
                    write_serial_string("[JOBS] fg done status=");
                    write_serial_hex(st);
                    write_serial_string("\n");
                } else {
                    print("fg: job already finished\n", 0x0C);
                }
            }
        }
}
