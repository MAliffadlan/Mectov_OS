// src/sys/shell/builtins/file_ops/cmd_pwd.c — the `pwd` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_pwd(void) {
        char cwd[MAX_PATH];
        vfs_get_abs_path(get_current_dir(), cwd, MAX_PATH);
        print(cwd, 0x0F);
        print("\n", 0x0F);
}
