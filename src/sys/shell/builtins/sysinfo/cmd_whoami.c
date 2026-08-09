// src/sys/shell/builtins/sysinfo/cmd_whoami.c — the `whoami` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_whoami(void) {
        // Single-user OS: always root (matches the $USER default).
        print("root\n", 0x0F);
}
