// src/sys/shell/builtins/env_cmds/cmd_export.c — the `export` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_export(void) {
        if (strcmp(cmd_b, "export") == 0) {
            for (int i = 0; i < env_var_count; i++) {
                print("declare -x ", 0x0E);
                print(env_vars[i].name, 0x0F);
                print("=\"", 0x07);
                print(env_vars[i].value, 0x0F);
                print("\"\n", 0x07);
            }
        } else {
            char* arg = cmd_b + 7;
            while (*arg == ' ') arg++;
            
            char name[ENV_NAME_LEN];
            char val[ENV_VAL_LEN];
            int n = 0, v = 0;
            
            while (*arg && *arg != '=' && *arg != ' ' && n < ENV_NAME_LEN - 1) {
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
                while (*arg && v < ENV_VAL_LEN - 1) {
                    if (quote && *arg == quote) {
                        arg++;
                        break;
                    }
                    val[v++] = *arg++;
                }
                val[v] = '\0';
                
                int found = -1;
                for (int i = 0; i < env_var_count; i++) {
                    if (strcmp(env_vars[i].name, name) == 0) {
                        found = i;
                        break;
                    }
                }
                if (found >= 0) {
                    strcpy(env_vars[found].value, val);
                } else if (env_var_count < MAX_ENV_VARS) {
                    strcpy(env_vars[env_var_count].name, name);
                    strcpy(env_vars[env_var_count].value, val);
                    env_var_count++;
                } else {
                    print("export: too many variables\n", 0x0C);
                }
            } else {
                print("usage: export NAME=VALUE\n", 0x0C);
            }
        }
}
