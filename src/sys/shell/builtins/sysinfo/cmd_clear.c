// src/sys/shell/builtins/sysinfo/cmd_clear.c — the `clear` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_clear(void) {
        if (get_use_term_buf()) term_clear();
        else c_work(); 
}
