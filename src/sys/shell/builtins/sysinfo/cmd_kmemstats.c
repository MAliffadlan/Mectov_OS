// src/sys/shell/builtins/sysinfo/cmd_kmemstats.c — the `kmemstats` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_kmemstats(void) {
        kmalloc_stats(print);
}
