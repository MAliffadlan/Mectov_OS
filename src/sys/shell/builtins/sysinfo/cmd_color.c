// src/sys/shell/builtins/sysinfo/cmd_color.c — the `color` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_color(void) {
        print("Color output is only available on TTY (Text Mode).\n", 0x0E);
}
