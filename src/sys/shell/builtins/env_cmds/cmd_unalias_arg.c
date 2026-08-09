// src/sys/shell/builtins/env_cmds/cmd_unalias_arg.c — the `unalias_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_unalias_arg(void) {
        char* name = cmd_b + 8;
        while (*name == ' ') name++;
        
        int found = -1;
        for (int i = 0; i < alias_count; i++) {
            if (strcmp(aliases[i].name, name) == 0) {
                found = i;
                break;
            }
        }
        
        if (found >= 0) {
            for (int i = found; i < alias_count - 1; i++) {
                aliases[i] = aliases[i + 1];
            }
            alias_count--;
        } else {
            print("unalias: alias not found: ", 0x0C);
            print(name, 0x0C);
            print("\n", 0x0C);
        }
}
