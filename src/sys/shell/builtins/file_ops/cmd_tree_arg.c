// src/sys/shell/builtins/file_ops/cmd_tree_arg.c — the `tree_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_tree_arg(void) {
        char* dirpath = cmd_b + 5;
        sanitize_path(dirpath);
        int node = vfs_get_node(dirpath);
        if (node < 0 || !vfs_is_dir(node)) {
            print("tree: directory not found\n", 0x0C);
        } else {
            vfs_tree(node, 0, print);
        }
}
