// src/sys/shell/builtins/file_ops/cmd_rmdir_arg.c — the `rmdir_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_rmdir_arg(void) {
        char* dpath = cmd_b + 6;
        sanitize_path(dpath);
        int node = vfs_get_node(dpath);
        if (node < 0 || !vfs_is_dir(node)) {
            print("rmdir: not a directory: ", 0x0C);
            print(dpath, 0x0C);
            print("\n", 0x0C);
        } else {
            // Refuse non-empty directories (vfs_delete_node would recurse).
            int has_child = 0;
            for (int i = 0; i < MAX_NODES; i++) {
                if (fs_nodes[i].in_use && fs_nodes[i].parent == node) {
                    has_child = 1;
                    break;
                }
            }
            if (has_child) {
                print("rmdir: directory not empty: ", 0x0C);
                print(dpath, 0x0C);
                print("\n", 0x0C);
            } else if (vfs_delete_node(dpath) < 0) {
                print("rmdir: failed\n", 0x0C);
            }
        }
}
