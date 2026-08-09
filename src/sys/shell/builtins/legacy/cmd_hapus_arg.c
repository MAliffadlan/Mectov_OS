// src/sys/shell/builtins/legacy/cmd_hapus_arg.c — the `hapus_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_hapus_arg(void) {
        char* fname = cmd_b + 6;
        sanitize_path(fname);
        int res = vfs_delete_node(fname);
        if (res < 0) print("File not found.\n", 0x0C);
        else print("File deleted.\n", 0x0A);
}
