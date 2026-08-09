// src/sys/shell/builtins/file_ops/cmd_mv_arg.c — the `mv_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_mv_arg(void) {
        // Parse "mv src dst"
        char src[MAX_PATH], dst[MAX_PATH];
        char* rest = cmd_b + 3;
        while (*rest == ' ') rest++;
        int si = 0;
        while (*rest && *rest != ' ' && si < MAX_PATH - 1) src[si++] = *rest++;
        src[si] = '\0';
        while (*rest == ' ') rest++;
        int di = 0;
        while (*rest && *rest != ' ' && di < MAX_PATH - 1) dst[di++] = *rest++;
        dst[di] = '\0';
        sanitize_path(src);
        sanitize_path(dst);
        
        if (src[0] == '\0' || dst[0] == '\0') {
            print("mv: usage: mv [source] [destination]\n", 0x0C);
        } else {
            // vfs_rename silently deletes an existing destination before
            // moving, so refuse when the destination is a directory — moving a
            // file onto one would erase it (and the VFS has no "move into
            // dir" semantics).
            int dst_node = vfs_get_node(dst);
            if (dst_node >= 0 && vfs_is_dir(dst_node)) {
                print("mv: cannot overwrite a directory: ", 0x0C);
                print(dst, 0x0C);
                print("\n", 0x0C);
            } else {
                int res = vfs_rename(src, dst);
                if (res < 0) {
                    if (res == -4) print("mv: cannot move a directory into itself\n", 0x0C);
                    else if (res == -5) print("mv: cross-directory move not supported on ext2\n", 0x0C);
                    else if (res == -3) print("mv: cannot rename root\n", 0x0C);
                    else print("mv: failed\n", 0x0C);
                } else {
                    print("mv: ", 0x0A);
                    print(src, 0x0F);
                    print(" -> ", 0x07);
                    print(dst, 0x0F);
                    print("\n", 0x0A);
                }
            }
        }
}
