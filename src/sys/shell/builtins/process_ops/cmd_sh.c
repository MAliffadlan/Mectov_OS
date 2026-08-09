// src/sys/shell/builtins/process_ops/cmd_sh.c — the `sh` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_sh(void) {
        char* fname = (strncmp(cmd_b, "sh ", 3) == 0) ? cmd_b + 3 : cmd_b + 7;
        sanitize_path(fname);
        run_script(fname);
}
