// src/sys/shell/builtins/file_ops/cmd_readlink_arg.c — the `readlink` shell
// command. v38.85: print the target of a symlink (one path argument).
#include "../../shell_internal.h"

void cmd_readlink_arg(void) {
        char* fpath = cmd_b + 9;   // past "readlink "
        while (*fpath == ' ') fpath++;
        if (*fpath == '\0') {
                print("usage: readlink path\n", 0x0C);
                return;
        }
        char buf[MAX_PATH];
        int n = vfs_readlink(fpath, buf, MAX_PATH);
        if (n < 0) {
                print("readlink: not a symlink\n", 0x0C);
                return;
        }
        buf[MAX_PATH - 1] = '\0';
        print(buf, 0x0F);
        print("\n", 0x0F);
}
