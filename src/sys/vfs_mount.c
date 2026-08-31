// ============================================================
// Runtime mount layer (v38.42)
// ============================================================
// A mount point is an empty VFS directory node retyped to the backend's
// FS_*_DIR; the existing type-based dispatch in vfs.c (read/write/create/
// delete hooks) then routes all operations under it to the real
// filesystem. This table records which drive + root key backs each mount
// so umount can undo the mapping, and so `mount` (no args) can list them.
//
// Limitation (until the backends grow per-volume state): ext2.c and
// fat32.c keep ONE global superblock each, so a runtime mount must target
// the drive the backend is currently initialized for — the boot drives
// (ext2 = 1, fat32 = 3) or a re-mount of the same kind. Mounting a second
// ext2 volume would silently repoint the first.

#include "../include/mount.h"
#include "../include/vfs.h"
#include "../include/serial.h"
#include "../include/utils.h"
#include "../include/ahci.h"   // AHCI_DRIVE_BASE: SATA ports are drives 4+
#include "../include/xhci.h"   // USB_DRIVE_BASE: USB mass-storage is drives 8+

static mount_t mounts[MAX_MOUNTS];

void mount_init(void) {
    memset(mounts, 0, sizeof(mounts));
}

int mount_register(int node_idx, mount_kind_t kind, int drive, uint32_t root_key) {
    if (node_idx < 0 || node_idx >= MAX_NODES) return -1;
    if (mount_lookup(node_idx) >= 0) return -1;   // node already a mount point
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) {
            mounts[i].in_use = 1;
            mounts[i].node_idx = node_idx;
            mounts[i].kind = kind;
            mounts[i].drive = drive;
            mounts[i].root_key = root_key;
            return i;
        }
    }
    return -1;
}

int mount_lookup(int node_idx) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].in_use && mounts[i].node_idx == node_idx) return i;
    }
    return -1;
}

void mount_dump(void) {
    static const char* names[] = { "none", "ext2", "fat32" };
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) continue;
        char path[MAX_PATH];
        if (vfs_get_abs_path(mounts[i].node_idx, path, sizeof(path)) < 0)
            path[0] = '\0';
        write_serial_string("[MOUNT] ");
        write_serial_string(path);
        write_serial_string(" type=");
        write_serial_string(names[mounts[i].kind]);
        write_serial_string(" drive=");
        write_serial_hex((uint32_t)mounts[i].drive);
        write_serial_string("\n");
    }
}

static int str_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

// True when no live node has `node` as a direct parent.
static int dir_is_empty(int node) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (fs_nodes[i].in_use && fs_nodes[i].parent == node) return 0;
    }
    return 1;
}

int vfs_mount_path(const char* path, const char* fstype, int drive) {
    if (!path || !fstype) return -1;

    mount_kind_t kind;
    if (str_eq(fstype, "ext2")) {
        kind = MOUNT_EXT2;
    } else if (str_eq(fstype, "fat32")) {
        kind = MOUNT_FAT32;
    } else {
        write_serial_string("[MOUNT] unknown fstype\n");
        return -1;
    }
    // Drives 0-3 are IDE; 4..7 are AHCI ports (v38.50); 8..11 are USB
    // mass-storage units (v38.56) — all routed through the same sector
    // API, so ext2/fat32 mount on any of them unchanged.
    if (drive < 0 || drive >= USB_DRIVE_BASE + USB_MAX_DRIVES) return -1;

    vfs_lock_acquire();

    // Find or create the mount point (an empty directory on the parent
    // filesystem — for a fresh path that is the MECTOVFS root fs).
    int node = vfs_get_node(path);
    if (node < 0) {
        char parent[MAX_PATH];
        if (vfs_get_parent(path, parent, sizeof(parent)) < 0) {
            vfs_lock_release();
            return -1;
        }
        int pnode = vfs_get_node(parent);
        if (pnode < 0 || !vfs_is_dir(pnode)) {
            vfs_lock_release();
            return -1;
        }
        // Last path component = the new mount point's name.
        const char* name = path;
        for (const char* p = path; *p; p++) if (*p == '/') name = p + 1;
        if (!*name) {
            vfs_lock_release();
            return -1;
        }
        node = vfs_create_node(name, FS_DIR, pnode);   // persists via vfs_save
        if (node < 0) {
            vfs_lock_release();
            return -1;
        }
    } else {
        if (!vfs_is_dir(node) || !dir_is_empty(node)) {
            write_serial_string("[MOUNT] mount point must be an empty directory\n");
            vfs_lock_release();
            return -1;
        }
    }
    if (mount_lookup(node) >= 0) {
        write_serial_string("[MOUNT] already mounted\n");
        vfs_lock_release();
        return -1;
    }

    // Bring the backend up for this drive, then retype the node and
    // populate the subtree from the real filesystem.
    extern int ext2_init(int drive);
    extern int fat32_init(int drive);
    extern void ext2_populate_vfs(uint32_t inode_num, int vfs_parent_node);
    extern void fat32_populate_vfs(uint32_t root_cluster, int vfs_parent_node);
    extern uint32_t fat32_root_cluster(void);

    uint32_t root_key = 0;
    if (kind == MOUNT_EXT2) {
        if (ext2_init(drive) != 0) {
            write_serial_string("[MOUNT] ext2_init failed\n");
            vfs_lock_release();
            return -1;
        }
        fs_nodes[node].type = FS_EXT2_DIR;
        fs_nodes[node].ext2_inode = 2;   // ext2 root directory inode
        root_key = 2;
        ext2_populate_vfs(2, node);
    } else {
        if (fat32_init(drive) != 0) {
            write_serial_string("[MOUNT] fat32_init failed\n");
            vfs_lock_release();
            return -1;
        }
        fs_nodes[node].type = FS_FAT32_DIR;
        root_key = fat32_root_cluster();
        fs_nodes[node].data_sector = (int)root_key;
        fat32_populate_vfs(root_key, node);
    }

    if (mount_register(node, kind, drive, root_key) < 0) {
        // Table full: undo the retype + subtree so the node is consistent.
        vfs_clear_children(node);
        fs_nodes[node].type = FS_DIR;
        vfs_lock_release();
        return -1;
    }

    // Diagnostics: how many entries the populate pass actually attached.
    int children = 0;
    for (int i = 0; i < MAX_NODES; i++)
        if (fs_nodes[i].in_use && fs_nodes[i].parent == node) children++;
    write_serial_string("[MOUNT] populated ");
    write_serial_hex((uint32_t)children);
    write_serial_string(" children\n");

    write_serial_string("[MOUNT] mounted ");
    write_serial_string(path);
    write_serial_string("\n");
    vfs_lock_release();
    return 0;
}

int vfs_umount_path(const char* path) {
    if (!path) return -1;

    vfs_lock_acquire();
    int node = vfs_get_node(path);
    if (node < 0) {
        vfs_lock_release();
        return -1;
    }
    int m = mount_lookup(node);
    if (m < 0) {
        write_serial_string("[MOUNT] not a mount point\n");
        vfs_lock_release();
        return -1;
    }

    // Drop the subtree (the data lives on the real filesystem, untouched)
    // and restore a plain directory node so the type dispatch stops
    // routing into the backend. The node table is persisted so a reboot
    // after umount boots with a plain directory at this path too.
    vfs_clear_children(node);
    fs_nodes[node].type = FS_DIR;
    fs_nodes[node].ext2_inode = 0;
    fs_nodes[node].data_sector = 0;
    mounts[m].in_use = 0;
    vfs_save();

    write_serial_string("[MOUNT] unmounted ");
    write_serial_string(path);
    write_serial_string("\n");
    vfs_lock_release();
    return 0;
}
