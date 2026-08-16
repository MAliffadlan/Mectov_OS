#ifndef MOUNT_H
#define MOUNT_H

#include "types.h"

// Runtime mount table (v38.42). Boot still mounts the built-in volumes
// (/ext2 on drive 1, /fat32 on drive 3), but the SYS_MOUNT/SYS_UMOUNT
// syscalls and the `mount`/`umount` shell builtins go through this table:
// a mount point is a VFS directory node whose fs_type is retyped to the
// backend's FS_*_DIR (the existing type-based dispatch in vfs.c then
// routes read/write/create/delete to the right filesystem), and the table
// records which drive + root key backs it so umount can undo the mapping.

#define MAX_MOUNTS 8

typedef enum {
    MOUNT_NONE = 0,
    MOUNT_EXT2,     // backend ext2.c, root key = root inode number (2)
    MOUNT_FAT32,    // backend fat32.c, root key = root cluster
} mount_kind_t;

typedef struct {
    int          in_use;
    int          node_idx;   // VFS node of the mount point
    mount_kind_t kind;
    int          drive;      // ATA drive the filesystem lives on
    uint32_t     root_key;   // ext2 root inode / FAT32 root cluster
} mount_t;

void mount_init(void);

// Record/lookup a mount on an existing directory node (the node's type is
// managed by the caller). Returns the mount slot index or -1.
int  mount_register(int node_idx, mount_kind_t kind, int drive, uint32_t root_key);
int  mount_lookup(int node_idx);
void mount_dump(void);   // serial-log every active mount (for `mount` with no args)

// Mount `fstype` ("ext2"|"fat32") from ATA `drive` at directory `path`
// (the mount point is created as a plain directory when missing, and must
// be an empty directory when present). umount drops the subtree and
// restores a plain directory node. Both take the VFS lock internally and
// are root-only at the syscall layer. Returns 0 or -1.
int  vfs_mount_path(const char* path, const char* fstype, int drive);
int  vfs_umount_path(const char* path);

#endif
