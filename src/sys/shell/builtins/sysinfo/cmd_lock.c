// src/sys/shell/builtins/sysinfo/cmd_lock.c — the `lock` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_lock(void) {
lock_screen();
}
