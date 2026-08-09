// src/sys/shell/builtins/process_ops/cmd_kill.c — the `kill` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_kill(void) {
        print("Usage: kill [PID | %job]  — Terminate a process or job\n", 0x0E);
}
