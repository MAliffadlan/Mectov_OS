// src/sys/shell/builtins/text_tools/cmd_beep.c — the `beep` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_beep(void) {
        beep(880, 200);
}
