// src/sys/shell/builtins/file_ops/cmd_find.c — the `find` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_find(void) {
        // find [DIR] [-name GLOB]  — default DIR = current dir.
        char* p = cmd_b + 4;
        char dirpath[MAX_PATH] = "";
        char pattern[64] = "";
        int have_pattern = 0;
        char* tok;
        while ((tok = next_token(&p)) != NULL) {
            if (strcmp(tok, "-name") == 0) {
                char* pat = next_token(&p);
                if (pat) { strncpy(pattern, pat, 63); pattern[63] = '\0'; have_pattern = 1; }
            } else if (!dirpath[0]) {
                strncpy(dirpath, tok, MAX_PATH - 1);
                dirpath[MAX_PATH - 1] = '\0';
            }
        }
        int start = (dirpath[0]) ? vfs_get_node(dirpath) : get_current_dir();
        if (start < 0 || !vfs_is_dir(start)) {
            print("find: directory not found: ", 0x0C);
            print((dirpath[0]) ? dirpath : ".", 0x0C);
            print("\n", 0x0C);
        } else {
            // Walk the flat node table: a node belongs to the tree rooted at
            // `start` iff following its parent chain reaches `start`.
            extern void write_serial_string(const char*);
            for (int i = 0; i < MAX_NODES; i++) {
                if (!fs_nodes[i].in_use) continue;
                int cur = i;
                int under = 0;
                for (int hop = 0; cur >= 0 && cur < MAX_NODES && hop < 64; hop++) {
                    if (cur == start) { under = 1; break; }
                    cur = fs_nodes[cur].parent;
                }
                if (!under) continue;
                if (have_pattern && !wild_match(pattern, fs_nodes[i].name)) continue;
                char path[MAX_PATH];
                if (vfs_get_abs_path(i, path, MAX_PATH) < 0) continue;
                print(path, 0x0F);
                print("\n", 0x0F);
                write_serial_string("[SH] find ");
                write_serial_string(path);
                write_serial_string("\n");
            }
        }
}
