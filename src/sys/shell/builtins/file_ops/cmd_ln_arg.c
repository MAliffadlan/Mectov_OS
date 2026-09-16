// src/sys/shell/builtins/file_ops/cmd_ln_arg.c — the `ln` shell command.
// v38.86: `ln target linkpath` creates a HARD link (POSIX semantics — both
// names share the file's content), `ln -s target linkpath` creates a
// symlink. The old two-arg-creates-symlink behavior is replaced: the plain
// form is now the real thing.
#include "../../shell_internal.h"

void cmd_ln_arg(void) {
        // Parse: optional "-s" (symlink), then target, then linkpath.
        char* rest = cmd_b + 3;   // past "ln "
        while (*rest == ' ') rest++;
        int symbolic = 0;
        if (rest[0] == '-' && (rest[1] == 's' || rest[1] == 'f')) {
                if (rest[1] == 's') symbolic = 1;
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
                print("usage: ln target linkpath | ln -s target linkpath\n", 0x0C);
                return;
        }
        if (symbolic) {
                // Store the target verbatim; absolute targets keep working
                // regardless of the link's location (what /bin aliases want).
                int res = vfs_symlink(target, linkpath);
                if (res < 0) {
                        if (res == -2) print("ln: file exists\n", 0x0C);
                        else print("ln: failed\n", 0x0C);
                }
                return;
        }
        int res = vfs_hardlink(target, linkpath);
        if (res < 0) {
                if (res == -2) print("ln: file exists\n", 0x0C);
                else if (res == -3) print("ln: hard links only work on plain files (use -s)\n", 0x0C);
                else if (res == -4) print("ln: target file is empty (nothing to link yet)\n", 0x0C);
                else print("ln: failed\n", 0x0C);
        }
}
