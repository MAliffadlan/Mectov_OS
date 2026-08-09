// src/sys/shell/builtins/process_ops/cmd_taskmgr.c — the `taskmgr` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_taskmgr(void) {
load_mct_app("/apps/taskmgr.mct");
}
