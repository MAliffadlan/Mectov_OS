// src/sys/shell/builtins/file_ops/cmd_umount.c — the `umount` shell command.
// v38.42: `umount <path>` drops the subtree under a mount point and turns
// it back into a plain directory. The filesystem's data on disk is left
// untouched. Runs in the shell's kernel task (uid 0).
#include "../../shell_internal.h"

void cmd_umount(void) {
        char* arg = cmd_b + 6;
        while (*arg == ' ') arg++;
        // also accept "unmount <path>"
        if (cmd_b[0] == 'u' && cmd_b[1] == 'n') { arg = cmd_b + 8; while (*arg == ' ') arg++; }

        if (*arg == '\0') {
                print("umount: usage: umount <path>\n", 0x0C);
                return;
        }

        if (vfs_umount_path(arg) == 0) {
                print("unmounted ", 0x0A);
                print(arg, 0x0A);
                print("\n", 0x0A);
        } else {
                print("umount: not a mount point\n", 0x0C);
        }
}
