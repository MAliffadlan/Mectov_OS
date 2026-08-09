// src/sys/shell/builtins/file_ops/cmd_touch_arg.c — the `touch_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_touch_arg(void) {
        char* fpath = cmd_b + 6;
        sanitize_path(fpath);
        int res = vfs_create_file(fpath);
        if (res < 0) {
            print("touch: failed\n", 0x0C);
        }
}
