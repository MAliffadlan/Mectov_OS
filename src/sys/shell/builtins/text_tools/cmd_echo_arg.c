// src/sys/shell/builtins/text_tools/cmd_echo_arg.c — the `echo_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_echo_arg(void) {
        print(cmd_b + 5, 0x0F);
        print("\n", 0x0F);
}
