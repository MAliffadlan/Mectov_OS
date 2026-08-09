// src/sys/shell/builtins/env_cmds/cmd_alias.c — the `alias` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_alias(void) {
        if (strcmp(cmd_b, "alias") == 0) {
            for (int i = 0; i < alias_count; i++) {
                print("alias ", 0x0E);
                print(aliases[i].name, 0x0F);
                print("='", 0x07);
                print(aliases[i].value, 0x0F);
                print("'\n", 0x07);
            }
        } else {
            char* arg = cmd_b + 6;
            while (*arg == ' ') arg++;
            
            char name[ALIAS_NAME_LEN];
            char val[ALIAS_VAL_LEN];
            int n = 0, v = 0;
            
            while (*arg && *arg != '=' && *arg != ' ' && n < ALIAS_NAME_LEN - 1) {
                name[n++] = *arg++;
            }
            name[n] = '\0';
            
            if (*arg == '=') {
                arg++;
                char quote = '\0';
                if (*arg == '"' || *arg == '\'') {
                    quote = *arg;
                    arg++;
                }
                while (*arg && v < ALIAS_VAL_LEN - 1) {
                    if (quote && *arg == quote) {
                        arg++;
                        break;
                    }
                    val[v++] = *arg++;
                }
                val[v] = '\0';
                
                int found = -1;
                for (int i = 0; i < alias_count; i++) {
                    if (strcmp(aliases[i].name, name) == 0) {
                        found = i;
                        break;
                    }
                }
                if (found >= 0) {
                    strcpy(aliases[found].value, val);
                } else if (alias_count < MAX_ALIASES) {
                    strcpy(aliases[alias_count].name, name);
                    strcpy(aliases[alias_count].value, val);
                    alias_count++;
                } else {
                    print("alias: too many aliases\n", 0x0C);
                }
            } else {
                print("usage: alias NAME=\"VALUE\"\n", 0x0C);
            }
        }
}
