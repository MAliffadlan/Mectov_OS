// src/sys/shell/builtins/file_ops/cmd_tree.c — the `tree` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_tree(void) {
        vfs_tree(get_current_dir(), 0, print);
}
