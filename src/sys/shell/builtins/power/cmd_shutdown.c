// src/sys/shell/builtins/power/cmd_shutdown.c — the `shutdown` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_shutdown(void) {
shutdown();
}
