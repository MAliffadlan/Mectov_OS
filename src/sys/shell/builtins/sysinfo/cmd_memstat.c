// src/sys/shell/builtins/sysinfo/cmd_memstat.c — the `memstat` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_memstat(void) {
        print("==================================================\n", 0x0B);
        print("                SYSTEM MEMORY STATS               \n", 0x0F);
        print("==================================================\n", 0x0B);
        print("Physical RAM:\n", 0x0E);
        print("  Total Memory : ", 0x0F); p_int(get_total_memory()/1024, 0x0A); print(" KB\n", 0x0F);
        print("  Used Memory  : ", 0x0F); p_int(get_used_memory()/1024, 0x0C); print(" KB\n", 0x0F);
        print("  Free Memory  : ", 0x0F); p_int(get_free_memory()/1024, 0x0A); print(" KB\n", 0x0F);
        print("--------------------------------------------------\n", 0x07);
        kmalloc_stats(print);
        print("==================================================\n", 0x0B);
}
