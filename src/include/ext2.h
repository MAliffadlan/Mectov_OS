#ifndef EXT2_H
#define EXT2_H

#include "types.h"

// Ext2 Magic Number
#define EXT2_SUPER_MAGIC 0xEF53

// Superblock
typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    
    // EXT2_DYNAMIC_REV Specific
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    uint8_t  s_volume_name[16];
    uint8_t  s_last_mounted[64];
    uint32_t s_algo_bitmap;
    
    // Precomp
    uint8_t prealloc_blocks;
    uint8_t prealloc_dir_blocks;
    uint16_t align_pad;
} __attribute__((packed)) ext2_superblock_t;

// Block Group Descriptor
typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} __attribute__((packed)) ext2_bg_descriptor_t;

// Inode
typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15]; // 0-11 direct, 12 singly, 13 doubly, 14 triply
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint32_t i_osd2[3];
} __attribute__((packed)) ext2_inode_t;

// Directory Entry
typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[255];
} __attribute__((packed)) ext2_dir_entry_t;

// File types
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2

// File types
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2

// API — read
int ext2_init(int drive);
int ext2_read_inode(uint32_t inode_num, ext2_inode_t* inode);
int ext2_read_file_data(uint32_t inode_num, char* buf, int max_size);

// API — write (persistence)
// Overwrite (or truncate-and-grow) a regular file's data. Returns bytes written
// or negative on failure. Allocates blocks from the group bitmaps as needed.
int ext2_write_file_data(uint32_t inode_num, const char* buf, int size);
// Create a new file or directory under parent_inode. Returns the new inode
// number, or 0 on failure. Directories get a data block with "." and "..".
uint32_t ext2_create_entry(uint32_t parent_inode, const char* name, uint8_t file_type);
// Remove a file/dir entry (frees the inode and all its blocks). Returns 0 ok.
int ext2_remove_entry(uint32_t parent_inode, const char* name);
// Rename an entry in place (keeps the same inode, does not touch data).
int ext2_rename_entry(uint32_t parent_inode, const char* old_name,
                      const char* new_name, uint32_t inode_num);
// Query an inode's current file size (used by the VFS layer after create).
uint32_t ext2_inode_size(uint32_t inode_num);

#endif
