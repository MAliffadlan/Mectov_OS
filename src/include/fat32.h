#ifndef FAT32_H
#define FAT32_H

#include "types.h"

// --- FAT32 on-disk constants ---
#define FAT32_SIGNATURE       0x55AA          // boot sector magic at offset 510
#define FAT32_FS_TYPE_OFF     82              // "FAT32   " signature offset
#define FAT32_EOC_MIN         0x0FFFFFF8      // FAT entry >= this = end of chain
#define FAT32_BAD_CLUSTER     0x0FFFFFF7
#define FAT32_FREE_CLUSTER    0x00000000
#define FAT32_ROOT_CLUSTER    2               // standard root dir cluster
#define FAT32_MAX_SPC         16              // driver supports <= 16 sectors/cluster (8 KB)

#define FAT32_ATTR_READ_ONLY  0x01
#define FAT32_ATTR_HIDDEN     0x02
#define FAT32_ATTR_SYSTEM     0x04
#define FAT32_ATTR_LABEL      0x08
#define FAT32_ATTR_DIR        0x10
#define FAT32_ATTR_ARCHIVE    0x20
#define FAT32_ATTR_LFN        0x0F
#define FAT32_ENTRY_DELETED   0xE5            // first name byte: deleted entry
#define FAT32_ENTRY_END       0x00            // first name byte: end of directory

// FAT32 BIOS Parameter Block (boot sector, offsets 11..61).
typedef struct {
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t sectors_per_fat;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
} __attribute__((packed)) fat32_bpb_t;

// FAT32 directory entry (32 bytes, SFN only — LFN entries are skipped on read).
typedef struct {
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  nt_res;
    uint8_t  ctime_tenths;
    uint16_t ctime;
    uint16_t cdate;
    uint16_t adate;
    uint16_t cluster_high;
    uint16_t mtime;
    uint16_t mdate;
    uint16_t cluster_low;
    uint32_t size;
} __attribute__((packed)) fat32_dirent_t;

// --- API ---
// Detect + parse the BPB of a FAT32 volume on `drive` (secondary channel
// drives 2-3 are supported by the ATA layer). Returns 0 on success.
int fat32_init(int drive);
// First cluster of the root directory (valid after fat32_init).
uint32_t fat32_root_cluster(void);

// Read up to max_size bytes of a file whose chain starts at first_cluster.
// Returns bytes read (0 for an empty/absent chain), or -1 on error.
int fat32_read_file(uint32_t first_cluster, char* buf, int max_size);
// Overwrite a file with buf[0..size): allocates clusters as needed, frees
// trailing clusters on shrink. Returns the NEW first cluster on success
// (may differ from the input if the file was empty), or -1 on failure.
int fat32_write_file(uint32_t first_cluster, const char* buf, int size);
// Update the size + first cluster recorded in the parent directory entry.
int fat32_update_dirent(uint32_t parent_cluster, const char* name,
                        uint32_t first_cluster, uint32_t size);

// Create a file (is_dir=0) or directory (is_dir=1) under parent_cluster.
// Returns the new object's first cluster, or 0 on failure.
uint32_t fat32_create_entry(uint32_t parent_cluster, const char* name, int is_dir);
// Remove an entry (frees its cluster chain). Returns 0 on success.
int fat32_remove_entry(uint32_t parent_cluster, const char* name);
// Rename an entry in place (same cluster, data untouched). Returns 0 on success.
int fat32_rename_entry(uint32_t parent_cluster, const char* old_name,
                       const char* new_name);

// Recursively map the volume's directories into the VFS tree.
void fat32_populate_vfs(uint32_t root_cluster, int vfs_parent_node);
// Capacity counters for `df`. Returns 0 on success.
int fat32_get_stats(uint32_t* total_clusters, uint32_t* free_clusters,
                    uint32_t* cluster_bytes);

// Offset-aware read window (v38.53): bytes [offset, offset+len) of the chain.
int fat32_read_file_range(uint32_t first_cluster, int offset, char* buf, int len);

#endif
