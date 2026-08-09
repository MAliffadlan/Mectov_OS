// src/sys/shell/builtins/sysinfo/cmd_hostname.c — the `hostname` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_hostname(void) {
        print("mectov\n", 0x0F);
}
