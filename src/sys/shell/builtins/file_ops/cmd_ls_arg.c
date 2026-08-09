// src/sys/shell/builtins/file_ops/cmd_ls_arg.c — the `ls_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_ls_arg(void) {
        char* dirpath = cmd_b + 3;
        sanitize_path(dirpath);
        int node = vfs_get_node(dirpath);
        if (node < 0 || !vfs_is_dir(node)) {
            print("ls: directory not found: ", 0x0C);
            print(dirpath, 0x0C);
            print("\n", 0x0C);
        } else {
            vfs_list_dir(node, print);
        }
}
