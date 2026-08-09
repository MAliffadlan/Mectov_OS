// src/sys/shell/builtins/process_ops/cmd_bg.c — the `bg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_bg(void) {
        int num = (cmd_b[2] == ' ') ? atoi(cmd_b + 3) : -1;
        if (num <= 0) {
            print("Usage: bg [job_number]\n", 0x0E);
        } else {
            int t = find_job_tid(num);
            if (t < 0) {
                print("bg: no such job\n", 0x0C);
            } else {
                extern int task_signal(int tid, int sig);
                extern int task_get_state(int tid);
                if (task_get_state(t) == TASK_STATE_STOPPED) {
                    task_signal(t, SIGCONT);
                    write_serial_string("[JOBS] bg SIGCONT tid=");
                    write_serial_hex(t);
                    write_serial_string("\n");
                    print("bg: job ", 0x0A); p_int(num, 0x0A);
                    print(" resumed in background\n", 0x0A);
                } else {
                    print("bg: job ", 0x0A); p_int(num, 0x0A);
                    print(" is not stopped\n", 0x0C);
                }
            }
        }
}
