// src/sys/shell/builtins/file_ops/cmd_ls.c — the `ls` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_ls(void) {
        vfs_list_dir(get_current_dir(), print);
}
