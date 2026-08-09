// src/sys/shell/builtins/file_ops/cmd_rm_arg.c — the `rm_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_rm_arg(void) {
        char* fpath = cmd_b + 3;
        sanitize_path(fpath);
        int res = vfs_delete_node(fpath);
        if (res < 0) {
            print("rm: failed\n", 0x0C);
        }
}
