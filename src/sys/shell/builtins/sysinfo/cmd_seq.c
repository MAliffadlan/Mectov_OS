// src/sys/shell/builtins/sysinfo/cmd_seq.c — the `seq` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_seq(void) {
        // seq LAST       → 1 2 ... LAST
        // seq FIRST LAST → FIRST ... LAST (descending works too)
        char* arg = cmd_b + 4;
        while (*arg == ' ') arg++;
        int first = 1, last = -1;
        // Accept numbers only (atoi returns 0 for garbage, which would
        // otherwise print a bogus "1 0" sequence).
        if (*arg >= '0' && *arg <= '9') {
            int a = atoi(arg);
            while (*arg >= '0' && *arg <= '9') arg++;
            if (*arg == ' ') {
                while (*arg == ' ') arg++;
                if (*arg >= '0' && *arg <= '9') {
                    first = a;
                    last = atoi(arg);
                }
            } else {
                last = a;
            }
        }
        if (last < 0) {
            print("seq: usage: seq [FIRST] LAST\n", 0x0C);
        } else if (first <= last) {
            for (int i = first; i <= last; i++) {
                p_int(i, 0x0F);
                if (i < last) print(" ", 0x0F);
            }
            print("\n", 0x0F);
        } else {
            for (int i = first; i >= last; i--) {
                p_int(i, 0x0F);
                if (i > last) print(" ", 0x0F);
            }
            print("\n", 0x0F);
        }
}
