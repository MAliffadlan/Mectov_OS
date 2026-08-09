// src/sys/shell/builtins/power/cmd_reboot.c — the `reboot` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_reboot(void) {
reboot();
}
