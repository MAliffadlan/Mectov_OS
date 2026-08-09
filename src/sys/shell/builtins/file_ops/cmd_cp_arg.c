// src/sys/shell/builtins/file_ops/cmd_cp_arg.c — the `cp_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_cp_arg(void) {
        // Parse "cp src dst"
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
            print("cp: usage: cp [source] [destination]\n", 0x0C);
        } else {
            int snode = vfs_get_node(src);
            if (snode < 0 || !vfs_is_file(snode)) {
                print("cp: source not found or not a file: ", 0x0C);
                print(src, 0x0C);
                print("\n", 0x0C);
            } else if (vfs_get_node(dst) >= 0) {
                print("cp: destination already exists: ", 0x0C);
                print(dst, 0x0C);
                print("\n", 0x0C);
            } else {
                int size = fs_nodes[snode].size;
                if (size < 0 || size > 4 * 1024 * 1024) {
                    print("cp: file too large\n", 0x0C);
                } else {
                    char* buf = (char*)kmalloc(size + 1);
                    if (!buf) {
                        print("cp: out of memory\n", 0x0C);
                    } else {
                        int rd = vfs_read_file(src, buf, size);
                        if (rd < 0) {
                            print("cp: read failed\n", 0x0C);
                        } else if (vfs_create_file(dst) < 0) {
                            print("cp: cannot create destination\n", 0x0C);
                        } else {
                            // Check the written count, not just the sign: on a
                            // full disk ext2_write_file_data returns the bytes
                            // written so far (positive but < rd), which would
                            // otherwise be reported as a successful copy.
                            int wr = vfs_write_file(dst, buf, rd);
                            if (wr != rd) {
                                print("cp: write failed (disk full? wrote ", 0x0C);
                                p_int(wr, 0x0C);
                                print(" of ", 0x0C);
                                p_int(rd, 0x0C);
                                print(" bytes)\n", 0x0C);
                            } else {
                                print("cp: copied ", 0x0A);
                                print(src, 0x0F);
                                print(" -> ", 0x07);
                                print(dst, 0x0F);
                                print(" (", 0x07);
                                p_int(rd, 0x0A);
                                print(" bytes)\n", 0x07);
                            }
                        }
                        kfree(buf);
                    }
                }
            }
        }
}
