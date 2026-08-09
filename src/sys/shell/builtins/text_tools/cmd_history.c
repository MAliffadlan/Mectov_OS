// src/sys/shell/builtins/text_tools/cmd_history.c — the `history` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_history(void) {
        int idx = hist_next_slot;
        int count = 0;
        if (hist_count < HIST_MAX) {
            idx = 0;
        }
        while (count < hist_count) {
            p_int(count + 1, 0x0E);
            print("  ", 0x07);
            print(history[idx], 0x0F);
            print("\n", 0x0F);
            idx = (idx + 1) % HIST_MAX;
            count++;
        }
}
