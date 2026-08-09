// src/sys/shell/builtins/file_ops/cmd_mkdir_arg.c — the `mkdir_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_mkdir_arg(void) {
        char* dirpath = cmd_b + 6;
        sanitize_path(dirpath);
        int res = vfs_mkdir(dirpath);
        if (res < 0) {
            print("mkdir: failed (", 0x0C);
            p_int(res, 0x0C);
            print(")\n", 0x0C);
        }
}
