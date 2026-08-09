// src/sys/shell/builtins/text_tools/cmd_sleep.c — the `sleep` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_sleep(void) {
        int seconds = atoi((strncmp(cmd_b, "sleep ", 6) == 0) ? cmd_b + 6 : cmd_b + 7);
        if (seconds > 0 && seconds < 60000) {
            // task_sleep() consumes PIT ticks, while the command contract says
            // "sleep [sec]". Convert here so user-facing behavior matches
            // the help text and stays scheduler-driven.
            uint64_t ticks64 = (uint64_t)seconds * 1000u;
            if (ticks64 > 0x7FFFFFFF) ticks64 = 0x7FFFFFFF;
            task_sleep((int)ticks64);
        }
}
