// src/sys/shell/builtins/app_launch/cmd_flappy.c — the `flappy` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_flappy(void) {
load_mct_app("/apps/flappy.mct");
}
