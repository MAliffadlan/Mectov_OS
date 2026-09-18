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
        
        // ext2 (boot drive 1): pin the backend first so the row always
        // describes the boot volume even when another ext2 volume was
        // touched most recently (per-volume auto-select, v38.78).
        mount_select_drive(MOUNT_EXT2, 1);
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
        
        // fat32 (boot drive 3): same pinning as the ext2 row above.
        extern int fat32_get_stats(uint32_t*, uint32_t*, uint32_t*);
        mount_select_drive(MOUNT_FAT32, 3);
        uint32_t tcl = 0, fcl = 0, cbytes = 512;
        if (fat32_get_stats(&tcl, &fcl, &cbytes) == 0 && tcl > 0) {
            uint32_t total_kb = tcl * cbytes / 1024;
            uint32_t free_kb = fcl * cbytes / 1024;
            uint32_t used_kb = total_kb - free_kb;
            uint32_t pct = used_kb * 100 / total_kb;
            print("fat32          ", 0x0B);
            p_int(total_kb, 0x0F); print("      ", 0x07);
            p_int(used_kb, 0x0F); print("    ", 0x07);
            p_int(free_kb, 0x0F); print("    ", 0x07);
            p_int(pct, 0x0F); print("%  /fat32", 0x0F);
            print("\n", 0x0F);
        } else {
            print("fat32          not mounted\n", 0x07);
        }
        
        // v38.96: tmpfs row — RAM-backed, capped by VFS_TMPFS_MAX_BYTES.
        {
            extern uint32_t tmpfs_used_get(void);
            uint32_t used_b = tmpfs_used_get();
            uint32_t total_kb = VFS_TMPFS_MAX_BYTES / 1024;
            uint32_t used_kb = used_b / 1024;
            uint32_t free_kb = total_kb - used_kb;
            uint32_t pct = used_b * 100 / VFS_TMPFS_MAX_BYTES;
            print("tmpfs          ", 0x0B);
            p_int((int)total_kb, 0x0F); print("      ", 0x07);
            p_int((int)used_kb, 0x0F); print("    ", 0x07);
            p_int((int)free_kb, 0x0F); print("    ", 0x07);
            p_int((int)pct, 0x0F); print("%  /tmp", 0x0F);
            print("\n", 0x0F);
        }
}
