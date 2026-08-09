// src/sys/shell/builtins/sysinfo/cmd_ps.c — the `ps` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_ps(void) {
        print("========================================================\n", 0x0B);
        print("  PID  RING  PRIO  STATE  PROCESS NAME\n", 0x0E);
        print("========================================================\n", 0x0B);
        for (int i = 0; i < 64; i++) {
            task_info_t info;
            if (get_task_info(i, &info)) {
                // Print PID
                print("  ", 0x0F);
                p_int(info.id, 0x0F);
                if (info.id < 10) print("    ", 0x0F);
                else print("   ", 0x0F);

                // Print Ring
                p_int(info.ring, 0x0F);
                print("     ", 0x0F);

                // Print Priority
                p_int(info.priority, 0x0F);
                print("    ", 0x0F);

                // Print State (1=RUNNING, 2=READY, 3=SLEEP)
                if (info.state == 1)      print("RUN     ", 0x0A);
                else if (info.state == 2) print("RDY     ", 0x0B);
                else if (info.state == 3) print("SLP     ", 0x07);
                else                      print("UNK     ", 0x0C);

                // Print Process Name (launch arg)
                const char* name = task_get_launch_arg(info.id);
                if (name && name[0] != '\0') {
                    print(name, 0x0F);
                } else {
                    print("unknown", 0x07);
                }
                print("\n", 0x0F);
            }
        }
        print("========================================================\n", 0x0B);
}
