// src/sys/shell/builtins/sysinfo/cmd_vfsinfo.c — the `vfsinfo` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_vfsinfo(void) {
        int count = 0;
        for (int i = 0; i < MAX_NODES; i++) if (fs_nodes[i].in_use) count++;
        print("VFS STATUS:\n", 0x0B);
        print("  MAX NODES : ", 0x0F); p_int(MAX_NODES, 0x0A); print("\n", 0x0F);
        print("  IN USE    : ", 0x0F); p_int(count, 0x0A); print("\n", 0x0F);
        print("  FREE      : ", 0x0F); p_int(MAX_NODES - count, 0x0A); print("\n", 0x0F);
        print("  ROOT INUSE: ", 0x0F); p_int(fs_nodes[0].in_use, 0x0A); print("\n", 0x0F);
}
