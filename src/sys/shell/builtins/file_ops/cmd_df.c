// src/sys/shell/builtins/file_ops/cmd_df.c — the `df` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_df(void) {
        print("Filesystem   1K-blocks   Used   Free   Use%  Mounted on\n", 0x0E);
        // MECTOVFS (drive 0): 1MB disk = VFS_DISK_SECTORS sectors, of which
        // VFS_DATA_START are magic + node table (see vfs.h).
        int used_sectors = VFS_DATA_START;
        for (int i = 0; i < MAX_NODES; i++) {
            if (fs_nodes[i].in_use && fs_nodes[i].type == FS_FILE) {
                int secs = (fs_nodes[i].size + 511) / 512;
                if (secs < 1) secs = 1;
                used_sectors += secs;
            }
        }
        if (used_sectors > VFS_DISK_SECTORS) used_sectors = VFS_DISK_SECTORS;
        int free_sectors = VFS_DISK_SECTORS - used_sectors;
        print("mectovfs       ", 0x0B);
        p_int(1024, 0x0F); print("      ", 0x07);
        p_int(used_sectors / 2, 0x0F); print("    ", 0x07);
        p_int(free_sectors / 2, 0x0F); print("    ", 0x07);
        p_int(used_sectors * 100 / VFS_DISK_SECTORS, 0x0F); print("%  /", 0x0F);
        print("\n", 0x0F);
        
        // ext2 (drive 1)
        uint32_t tblocks = 0, fblocks = 0, tinodes = 0, finodes = 0, bsize = 1024;
        if (ext2_get_stats(&tblocks, &fblocks, &tinodes, &finodes, &bsize) == 0 && tblocks > 0) {
            uint32_t total_kb = tblocks * bsize / 1024;
            uint32_t free_kb = fblocks * bsize / 1024;
            uint32_t used_kb = total_kb - free_kb;
            uint32_t pct = used_kb * 100 / total_kb;
            print("ext2           ", 0x0B);
            p_int(total_kb, 0x0F); print("      ", 0x07);
            p_int(used_kb, 0x0F); print("    ", 0x07);
            p_int(free_kb, 0x0F); print("    ", 0x07);
            p_int(pct, 0x0F); print("%  /ext2", 0x0F);
            print("\n", 0x0F);
            print("  Inodes: ", 0x07);
            p_int(tinodes - finodes, 0x0F); print(" used / ", 0x07);
            p_int(tinodes, 0x0F); print(" total\n", 0x07);
        } else {
            print("ext2           not mounted\n", 0x07);
        }
}
