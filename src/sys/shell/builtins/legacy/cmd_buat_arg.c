// src/sys/shell/builtins/legacy/cmd_buat_arg.c — the `buat_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_buat_arg(void) {
        char* fname = cmd_b + 5;
        sanitize_path(fname);
        // Use new VFS
        int res = vfs_create_file(fname);
        if (res >= 0) print("File created successfully.\n", 0x0A);
        else if (res == -2) print("File already exists.\n", 0x0C);
        else print("Disk is full!\n", 0x0C);
}
