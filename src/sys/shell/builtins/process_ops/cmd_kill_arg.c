// src/sys/shell/builtins/process_ops/cmd_kill_arg.c — the `kill_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_kill_arg(void) {
        char* arg = cmd_b + 5;
        while (*arg == ' ') arg++;
        if (arg[0] == '%') {
            // Job syntax: kill %n
            int num = atoi(arg + 1);
            write_serial_string("[JOBS] kill % arg='");
            write_serial_string(arg);
            write_serial_string("'\n");
            int t = find_job_tid(num);
            if (t < 0) {
                print("kill: no such job\n", 0x0C);
                write_serial_string("[JOBS] kill no such job\n");
            } else {
                task_kill(t);
                print("Job ", 0x0A); p_int(num, 0x0A); print(" (pid ", 0x0A);
                p_int(t, 0x0A); print(") sent SIGKILL.\n", 0x0A);
                write_serial_string("[JOBS] kill %");
                write_serial_hex(num);
                write_serial_string(" tid=");
                write_serial_hex(t);
                write_serial_string(" SIGKILL sent\n");
            }
        } else {
            int tid = atoi(arg);
            if (tid == 0) {
                print("kill: cannot terminate idle kernel process (PID 0)!\n", 0x0C);
            } else if (tid < 0 || tid >= 64) {
                print("kill: invalid PID!\n", 0x0C);
            } else if (!task_is_alive(tid)) {
                print("kill: process not found!\n", 0x0C);
            } else {
                int res = task_kill(tid);
                if (res == 0) {
                    print("Process ", 0x0A);
                    p_int(tid, 0x0A);
                    print(" terminated successfully.\n", 0x0A);
                } else {
                    print("kill: failed to terminate process!\n", 0x0C);
                }
            }
        }
}
