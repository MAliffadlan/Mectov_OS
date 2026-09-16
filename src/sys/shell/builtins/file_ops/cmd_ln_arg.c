// src/sys/shell/builtins/file_ops/cmd_ln_arg.c — the `ln` shell command.
// v38.85: symlink creation, `ln -s target linkpath` (GNU style) or
// `ln target linkpath` (Mectov implements only symlinks, so the plain
// two-argument form creates a symlink as well).
#include "../../shell_internal.h"

void cmd_ln_arg(void) {
        // Parse: optional "-s", then target, then linkpath.
        char* rest = cmd_b + 3;   // past "ln "
        while (*rest == ' ') rest++;
        if (rest[0] == '-' && (rest[1] == 's' || rest[1] == 'f')) {
                rest += 2;
                while (*rest == ' ') rest++;
        }
        char target[MAX_PATH];
        char linkpath[MAX_PATH];
        int si = 0;
        while (*rest && *rest != ' ' && si < MAX_PATH - 1) target[si++] = *rest++;
        target[si] = '\0';
        while (*rest == ' ') rest++;
        int di = 0;
        while (*rest && *rest != ' ' && di < MAX_PATH - 1) linkpath[di++] = *rest++;
        linkpath[di] = '\0';

        if (target[0] == '\0' || linkpath[0] == '\0') {
                print("usage: ln [-s] target linkpath\n", 0x0C);
                return;
        }
        // Store the target verbatim; absolute targets keep working regardless
        // of the link's location, which is what /bin aliases want.
        int res = vfs_symlink(target, linkpath);
        if (res < 0) {
                if (res == -2) print("ln: file exists\n", 0x0C);
                else print("ln: failed\n", 0x0C);
        }
}
