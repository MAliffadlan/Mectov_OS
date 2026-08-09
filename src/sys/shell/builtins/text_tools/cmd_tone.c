// src/sys/shell/builtins/text_tools/cmd_tone.c — the `tone` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_tone(void) {
        int freq = atoi(cmd_b + 5);
        if (freq > 20 && freq < 20000) beep(freq, 300);
}
