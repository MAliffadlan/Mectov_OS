// src/sys/shell/builtins/text_tools/cmd_yes.c — the `yes` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_yes(void) {
        // Prints STRING (default "y") forever, one per line — the classic
        // pipeline filler. Run it backgrounded (`yes hi &`) and stop it with
        // `kill <pid>` / `kill %job`; SIGKILL terminates it immediately, and
        // a 50ms yield between lines keeps it gentle on the scheduler.
        char* word = cmd_b + 3;
        while (*word == ' ') word++;
        if (word[0] == '\0') word = "y";
        extern void task_sleep(int);
        int it = 0;
        for (;;) {
            print(word, 0x0F);
            print("\n", 0x0F);
            task_sleep(50); // 50ms per line (~20 lines/sec)
            if (++it == 1000000) break; // unreachable safety valve
        }
}
