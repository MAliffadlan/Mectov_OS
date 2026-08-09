// src/sys/shell/builtins/legacy/cmd_edit.c — the `edit` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_edit(void) {
        char* fname = NULL;
        if (strncmp(cmd_b, "edit ", 5) == 0) fname = cmd_b + 5;
        else if (strncmp(cmd_b, "tulis ", 6) == 0) fname = cmd_b + 6;
        else fname = cmd_b + 5;
        sanitize_path(fname);
        print("Launching Editor...\n", 0x0E);
        st_ed(fname);
}
