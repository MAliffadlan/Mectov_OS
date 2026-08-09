// src/sys/shell/builtins/file_ops/cmd_type_arg.c — the `type_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_type_arg(void) {
        char* name = cmd_b + 5;
        while (*name == ' ') name++;
        // Only the first word is meaningful (bash: `type cmd`).
        char tbuf[ALIAS_NAME_LEN];
        int ti = 0;
        for (; name[ti] && name[ti] != ' ' && ti < ALIAS_NAME_LEN - 1; ti++) {
            tbuf[ti] = name[ti];
        }
        tbuf[ti] = '\0';
        if (tbuf[0] == '\0') {
            print("type: usage: type [name]\n", 0x0E);
        } else {
            const char* kind = NULL;
            // 1) alias (expand_alias only expands the first word, so `type
            //    ll` still sees the alias name, not its expansion).
            for (int i = 0; i < alias_count && !kind; i++) {
                if (strcmp(aliases[i].name, tbuf) == 0) {
                    print(tbuf, 0x0F); print(" is aliased to `", 0x0F);
                    print(aliases[i].value, 0x0A); print("'\n", 0x0F);
                    kind = "alias";
                }
            }
            // 2) shell builtin
            for (int i = 0; cmd_list[i] != NULL && !kind; i++) {
                if (strcmp(cmd_list[i], tbuf) == 0) {
                    print(tbuf, 0x0F); print(" is a shell builtin\n", 0x0F);
                    kind = "builtin";
                }
            }
            // 3) external app on the VFS (/apps/name.mct)
            if (!kind) {
                char apath[MAX_PATH];
                int ai = 0;
                strcpy(apath, "/apps/");
                ai = 6;
                for (int k = 0; tbuf[k] && ai < MAX_PATH - 5; k++) {
                    apath[ai++] = tbuf[k];
                }
                apath[ai++] = '.'; apath[ai++] = 'm'; apath[ai++] = 'c'; apath[ai++] = 't';
                apath[ai] = '\0';
                if (vfs_get_node(apath) >= 0) {
                    print(tbuf, 0x0F); print(" is ", 0x0F);
                    print(apath, 0x0A); print("\n", 0x0F);
                    kind = "external";
                }
            }
            if (!kind) {
                print("type: ", 0x0C); print(tbuf, 0x0C);
                print(": not found\n", 0x0C);
                kind = "notfound";
            }
            // Serial mirror for automated tests.
            extern void write_serial_string(const char*);
            write_serial_string("[SH] type ");
            write_serial_string(tbuf);
            write_serial_string(" -> ");
            write_serial_string(kind);
            write_serial_string("\n");
        }
}
