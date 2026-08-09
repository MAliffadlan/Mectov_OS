// src/sys/shell/builtins/sysinfo/cmd_uname.c — the `uname` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_uname(void) {
        // `uname` prints the OS name; `uname -a` prints the full kernel banner
        // (name hostname release version machine).
        if (strncmp(cmd_b, "uname -a", 8) == 0) {
            print("MectovOS mectov ", 0x0F); print(OS_VERSION, 0x0F);
            print(" mectov i686\n", 0x0F);
        } else {
            print("MectovOS\n", 0x0F);
        }
}
