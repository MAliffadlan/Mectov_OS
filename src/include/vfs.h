#ifndef VFS_H
#define VFS_H

#include "types.h"

#define MAX_NODES     256
#define MAX_PATH      256
#define MAX_FILENAME  32

// On-disk layout of the MECTOVFS disk image (1MB = VFS_DISK_SECTORS
// sectors, created by `dd if=/dev/zero of=disk.img bs=512 count=2048` in
// run.sh and CI — keep the count in sync with VFS_DISK_SECTORS):
//   Sector 0                          : magic "MECTOVFS" + metadata
//   Sector 1 .. VFS_NODE_SECTORS      : node table (256 nodes × 512 bytes)
//   VFS_DATA_START .. VFS_DISK_SECTORS-1 : file data blocks
// Bumped from 64 to 256 nodes (layout v1 → v2): vfs_load() rejects an old
// image so the node table is rebuilt from the embedded apps instead of
// reading garbage into nodes 64..255. Kept here (not vfs.c) so shell's
// `df` reports the same numbers.
#define VFS_MAGIC_SECTOR  0
#define VFS_NODE_START    1
#define VFS_NODE_SECTORS  256  // 256 nodes * 512 bytes = 128KB on disk

// v38.96: tmpfs budget. Global cap on RAM committed to FS_RAM_FILE buffers
// (kernel heap); per-file cap lives in vfs.c and matches the fd write path.
#define VFS_TMPFS_MAX_BYTES (2 * 1024 * 1024)   // 2MB
#define VFS_DATA_START    (VFS_NODE_START + VFS_NODE_SECTORS)
#define VFS_DISK_SECTORS  2048 // total sectors on the 1MB image
// v3 (v38.23): node table gained uid/gid/mode (ownership + permissions). An
// image written by an older layout is rejected so the table is rebuilt with
// the new fields instead of reading garbage ownership into every node.
#define VFS_LAYOUT_VERSION 3

typedef enum { FS_FILE, FS_DIR, FS_DEV, FS_EXT2_FILE, FS_EXT2_DIR, FS_FAT32_FILE, FS_FAT32_DIR, FS_PROC, FS_SYMLINK,
               // v38.96: tmpfs — RAM-backed nodes (buffers in the kernel heap,
               // side table in vfs.c). Appended at the END on purpose: the node
               // table persists on disk with numeric types, so inserting in the
               // middle would renumber FS_SYMLINK and misread existing disks.
               FS_RAM_DIR, FS_RAM_FILE } fs_type_t;

// v38.85: symlinks. The link target is stored inside the node's 450-byte pad
// (fs_node_t is a packed 512-byte on-disk struct; adding a field would bump
// VFS_LAYOUT_VERSION — using pad keeps old images readable untouched).
#define VFS_SYMLINK_MAX 120
#define VFS_SYMLINK_HOPS_MAX 8   // POSIX ELOOP budget

// Create linkpath -> target. Target may be absolute (preferred; /bin links
// are absolute) or relative to the directory containing linkpath.
int vfs_symlink(const char* target, const char* linkpath);
// Read the target of a symlink into buf; returns byte count or -1 (not a
// symlink / missing). Does NOT follow the link.
int vfs_readlink(const char* path, char* buf, int size);

// v38.86: hard link — new_path becomes another name for the SAME file node
// (both names share data_sector, so both see every byte written through
// either name). Only plain VFS files; symlinks/dirs/proc/dev are refused.
// The kernel's sector allocator builds its map from in-use nodes, so the
// shared sectors stay claimed until the LAST name is deleted.
int vfs_hardlink(const char* existing_path, const char* new_path);

// v38.86: kernel stat struct — mirrors stat_t in syscall.h (kept separate to
// avoid a header cycle; the syscall layer copies field by field).
typedef struct vfs_stat {
    int size;
    int type;          // fs_type_t value
    int node_idx;
    int parent;
    int data_sector;
    int nlink;         // how many directory names point at this node
    char name[32];
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
} vfs_stat_t;

// stat() follows a final symlink; lstat() does not (reports the link itself).
// Returns 0 or -1.
int vfs_stat_follow(const char* path, vfs_stat_t* st);
int vfs_stat_nofollow(const char* path, vfs_stat_t* st);
// Count names pointing at path's node (no-follow counts the link itself).
int vfs_nlink(const char* path, int follow);

// ---- Unix-style ownership & permission bits (POSIX S_I* values) ----
#define S_IRUSR 0x100  // owner read
#define S_IWUSR 0x080  // owner write
#define S_IXUSR 0x040  // owner execute
#define S_IRGRP 0x020  // group read
#define S_IWGRP 0x010  // group write
#define S_IXGRP 0x008  // group execute
#define S_IROTH 0x004  // other read
#define S_IWOTH 0x002  // other write
#define S_IXOTH 0x001  // other execute

// UID 0 is root (kernel tasks + kernel-created nodes). User apps run as
// UID 1000 (the single logged-in user). gid is 0 everywhere today but is
// carried through the node table so group bits are already meaningful.
#define ROOT_UID    0
#define USER_UID    1000

typedef struct {
    char name[MAX_FILENAME];
    fs_type_t type;
    int parent;          // Index parent directory (-1 = root)
    int size;            // Untuk FILE: size data
    int data_sector;     // Untuk FILE: ATA sector start data
    int in_use;
    uint32_t ext2_inode; // Ext2 Inode Number
    uint16_t uid;        // owning user (POSIX UID)
    uint16_t gid;        // owning group (POSIX GID)
    uint16_t mode;       // 9 permission bits (S_IRUSR|...|S_IXOTH)
    char pad[450];       // Total 512 bytes per node
} __attribute__((packed)) fs_node_t;

extern fs_node_t fs_nodes[MAX_NODES];

// Reentrant irqsave VFS lock (owner (cpu,tid) + depth) and the subtree
// dropper — shared with the runtime mount layer (vfs_mount.c, v38.42).
// Ordering: task_lock > fd_lock > vfs_lock > ata_lock.
void vfs_lock_acquire(void);
void vfs_lock_release(void);
void vfs_clear_children(int node);

int get_current_dir(void);
void set_current_dir(int dir);

// Inisialisasi VFS
void vfs_init();

// Simpan/load dari ATA disk
void vfs_save();
int vfs_load();

// Operasi node
int vfs_find_in_dir(const char* name, int dir_node);
int vfs_find_path(const char* path, int* parent_dir);
int vfs_create_node(const char* name, fs_type_t type, int parent);
int vfs_mkdir(const char* path);
int vfs_create_file(const char* path);
int vfs_delete_node(const char* path);
int vfs_rename(const char* old_path, const char* new_path);
// uid-aware variants (v38.53): enforce protected-path rules (e.g.
// /etc/passwd is undeletable/unrenamable by non-root — deleting it would
// re-arm the default-password login fallback). The plain forms above act as
// the logged-in user (USER_UID) for shell builtins; syscall handlers pass
// the real caller uid here; kernel-internal callers pass ROOT_UID.
#define PASSWD_PROTECT_PATH "/etc/passwd"
int vfs_delete_node_as(const char* path, int acting_uid);
int vfs_rename_as(const char* old_path, const char* new_path, int acting_uid);
int vfs_write_file(const char* path, const char* data, int size);
int vfs_read_file(const char* path, char* buf, int max_size);
// Offset-aware read by node index WITHOUT taking vfs_lock (callers that
// already hold it, or that cannot block — e.g. the mmap page-fault handler —
// use this; it takes only ata_lock, the innermost lock). Plain FS_FILE nodes
// only. Returns bytes read or -1.
int vfs_read_file_offset(int node, int offset, char* buf, int len);
// v38.96: offset-aware write into a tmpfs (FS_RAM_FILE) node. append=1
// writes at EOF (O_APPEND). Grows the RAM buffer on demand, honors the
// tmpfs budget; returns bytes written or <0.
int vfs_write_file_offset(int node, int offset, const char* buf, int len, int append);

// Resolusi path
void vfs_resolve_path(const char* path, char* resolved, int buf_size);
int vfs_get_abs_path(int node_idx, char* buf, int buf_size);

// List directory
void vfs_list_dir(int dir_node, void (*print_fn)(const char*, unsigned char));
void vfs_tree(int dir_node, int depth, void (*print_fn)(const char*, unsigned char));

// Helpers
int vfs_is_dir(int node);
int vfs_is_file(int node);
int vfs_get_node(const char* path);
int vfs_get_node_count();
int vfs_get_parent(const char* path, char* parent_path, int buf_size);

// ---- Ownership & permissions (v38.23) ----
// Check whether the CURRENT task may access `node` for the given permission
// bits (S_IRUSR for read, S_IWUSR for write, S_IXUSR for execute — the owner
// set is compared against the caller's uid). Returns 1 (allow) or 0 (deny).
// Root (uid 0) bypasses every check, POSIX-style. FAT32 nodes are always
// allowed (the filesystem has no permission concept); ext2/MECTOVFS nodes
// enforce their stored mode.
int vfs_check_perm(int node, uint16_t want);
// Set the mode/owner of a node (path resolves inside; takes vfs_lock).
// chmod requires the caller to own the node (or be root); chown requires
// root. Returns 0 or -1.
int vfs_chmod(const char* path, uint16_t mode);
int vfs_chown(const char* path, uint16_t uid, uint16_t gid);
// Force the default mode for a node regardless of ownership (kernel use:
// seeding keeps /apps executable for every user). Returns 0 or -1.
int vfs_set_mode(const char* path, uint16_t mode);
// Long-format directory listing for `ls -l`: prints one line per entry with
// mode string, owner/group, size and name.
void vfs_list_dir_long(int dir_node, void (*print_fn)(const char*, unsigned char));
// Format a 9-bit mode into "rwxr-xr-x" (out must hold 10 bytes).
void vfs_format_mode(uint16_t mode, char* out);

#endif