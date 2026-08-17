// src/sys/shell/builtins/env_cmds/cmd_passwd.c — the `passwd` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_passwd(void) {
        char* p = cmd_b + 6;
        char* old = next_token(&p);
        char* new = next_token(&p);
        if (!old || !new) {
            print("passwd: usage: passwd <current> <new>\n", 0x0C);
        } else {
            if (!sys_verify_password(old)) {
                print("passwd: current password incorrect\n", 0x0C);
            } else if (new[0] == '\0') {
                print("passwd: new password cannot be empty\n", 0x0C);
            } else if (sys_set_password(new) == 0) {
                print("Password updated.\n", 0x0A);
            } else {
                print("passwd: failed to write ", 0x0C);
                print(PASSWD_PATH, 0x0C);
                print("\n", 0x0C);
            }
        }
}
