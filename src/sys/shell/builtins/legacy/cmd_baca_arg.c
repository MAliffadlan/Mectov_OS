// src/sys/shell/builtins/legacy/cmd_baca_arg.c — the `baca_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_baca_arg(void) {
        char* fname = cmd_b + 5;
        sanitize_path(fname);
        char buf[512];
        int sz = vfs_read_file(fname, buf, 511);
        if (sz < 0) print("File not found.\n", 0x0C);
        else { buf[sz] = '\0'; print(buf, 0x0F); print("\n", 0x0F); }
}
