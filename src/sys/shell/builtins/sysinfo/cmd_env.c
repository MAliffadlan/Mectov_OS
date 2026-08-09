// src/sys/shell/builtins/sysinfo/cmd_env.c — the `env` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_env(void) {
        if (env_var_count == 0) {
            print("env: no environment variables\n", 0x0C);
        } else {
            for (int i = 0; i < env_var_count; i++) {
                print(env_vars[i].name, 0x0F);
                print("=", 0x07);
                print(env_vars[i].value, 0x0F);
                print("\n", 0x0F);
            }
        }
}
