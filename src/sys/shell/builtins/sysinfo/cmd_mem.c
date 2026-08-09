// src/sys/shell/builtins/sysinfo/cmd_mem.c — the `mem` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_mem(void) {
        print("RAM Status:\n", 0x0B);
        print("Total: ", 0x0F); p_int(get_total_memory()/1024, 0x0A); print(" KB\n", 0x0F);
        print("Free : ", 0x0F); p_int(get_free_memory()/1024, 0x0A); print(" KB\n", 0x0F);
}
