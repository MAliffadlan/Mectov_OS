#include "../include/ext2.h"
#include "../include/ata.h"
#include "../include/utils.h"
#include "../include/vfs.h"
#include "../include/mem.h"

static int ext2_drive = -1;
static ext2_superblock_t sb;
static uint32_t block_size = 1024;
static uint32_t bgd_block = 2; // Usually block 2 for 1024-byte blocks
static ext2_bg_descriptor_t* bgd_table = NULL;

static void ext2_read_block(uint32_t block, unsigned char* buf) {
    uint32_t sectors_per_block = block_size / 512;
    uint32_t start_sector = block * sectors_per_block;
    for (uint32_t i = 0; i < sectors_per_block; i++) {
        ata_read_sector_drive(ext2_drive, start_sector + i, buf + (i * 512));
    }
}

int ext2_init(int drive) {
    extern void write_serial_string(const char*);
    write_serial_string("[EXT2] ext2_init start\n");
    ext2_drive = drive;
    static unsigned char buf[1024];
    
    // Superblock is at offset 1024 (block 1 if block_size is 1024)
    // Read 2 sectors starting from sector 2
    write_serial_string("[EXT2] reading sector 2...\n");
    ata_read_sector_drive(ext2_drive, 2, buf);
    write_serial_string("[EXT2] reading sector 3...\n");
    ata_read_sector_drive(ext2_drive, 3, buf + 512);
    write_serial_string("[EXT2] sectors read ok\n");
    
    memcpy(&sb, buf, sizeof(ext2_superblock_t));
    
    extern void write_serial_string(const char*);
    
    if (sb.s_magic != EXT2_SUPER_MAGIC) {
        write_serial_string("[EXT2] Magic mismatch\n");
        return -1; // Not an Ext2 filesystem
    }
    
    write_serial_string("[EXT2] Superblock found!\n");
    
    block_size = 1024 << sb.s_log_block_size;
    bgd_block = (block_size == 1024) ? 2 : 1;
    
    // Read Block Group Descriptor Table (assume it fits in one block for simplicity)
    static unsigned char bgd_buf[4096]; 
    ext2_read_block(bgd_block, bgd_buf);
    
    // Allocate memory for BGD table (static array for simplicity, up to 32 groups)
    static ext2_bg_descriptor_t bgds[32];
    uint32_t num_groups = (sb.s_blocks_count + sb.s_blocks_per_group - 1) / sb.s_blocks_per_group;
    if (num_groups > 32) num_groups = 32;
    
    memcpy(bgds, bgd_buf, num_groups * sizeof(ext2_bg_descriptor_t));
    bgd_table = bgds;
    
    return 0;
}

int ext2_read_inode(uint32_t inode_num, ext2_inode_t* inode) {
    if (inode_num < 1 || inode_num > sb.s_inodes_count) return -1;
    
    uint32_t bg = (inode_num - 1) / sb.s_inodes_per_group;
    uint32_t index = (inode_num - 1) % sb.s_inodes_per_group;
    
    uint32_t inode_table_block = bgd_table[bg].bg_inode_table;
    uint32_t inode_size = (sb.s_rev_level == 0) ? 128 : sb.s_inode_size;
    
    uint32_t block_index = (index * inode_size) / block_size;
    uint32_t offset = (index * inode_size) % block_size;
    
    unsigned char buf[4096];
    ext2_read_block(inode_table_block + block_index, buf);
    
    memcpy(inode, buf + offset, sizeof(ext2_inode_t));
    return 0;
}

// Traverse Ext2 directory and map to VFS recursively
void ext2_populate_vfs(uint32_t inode_num, int vfs_parent_node) {
    ext2_inode_t inode;
    if (ext2_read_inode(inode_num, &inode) != 0) return;
    
    if (!(inode.i_mode & 0x4000)) return; // Not a directory
    
    unsigned char buf[4096];
    
    // Iterate over direct blocks
    for (int i = 0; i < 12; i++) {
        uint32_t block = inode.i_block[i];
        if (!block) continue;
        
        ext2_read_block(block, buf);
        
        uint32_t offset = 0;
        while (offset < block_size) {
            ext2_dir_entry_t* entry = (ext2_dir_entry_t*)(buf + offset);
            if (entry->inode == 0 || entry->rec_len == 0) break;
            
            char name[256];
            memcpy(name, entry->name, entry->name_len);
            name[entry->name_len] = '\0';
            
            if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0 && strcmp(name, "lost+found") != 0) {
                if (entry->file_type == EXT2_FT_DIR) {
                    int new_dir = vfs_create_node(name, FS_EXT2_DIR, vfs_parent_node);
                    if (new_dir >= 0) {
                        fs_nodes[new_dir].ext2_inode = entry->inode;
                        ext2_populate_vfs(entry->inode, new_dir);
                    }
                } else if (entry->file_type == EXT2_FT_REG_FILE) {
                    int new_file = vfs_create_node(name, FS_EXT2_FILE, vfs_parent_node);
                    if (new_file >= 0) {
                        fs_nodes[new_file].ext2_inode = entry->inode;
                        ext2_inode_t finode;
                        ext2_read_inode(entry->inode, &finode);
                        fs_nodes[new_file].size = finode.i_size;
                    }
                }
            }
            offset += entry->rec_len;
        }
    }
}

int ext2_read_file_data(uint32_t inode_num, char* buf, int max_size) {
    ext2_inode_t inode;
    if (ext2_read_inode(inode_num, &inode) != 0) return -1;
    
    uint32_t size = inode.i_size;
    if (size > (uint32_t)max_size) size = max_size;
    
    uint32_t bytes_read = 0;
    unsigned char block_buf[4096];
    
    // Read direct blocks
    for (int i = 0; i < 12 && bytes_read < size; i++) {
        uint32_t block = inode.i_block[i];
        if (!block) break;
        
        ext2_read_block(block, block_buf);
        uint32_t chunk = (size - bytes_read > block_size) ? block_size : (size - bytes_read);
        memcpy(buf + bytes_read, block_buf, chunk);
        bytes_read += chunk;
    }
    
    // 2. Read singly indirect block (12)
    if (bytes_read < size && inode.i_block[12]) {
        uint32_t* indirect_buf = (uint32_t*)kmalloc(block_size);
        if (indirect_buf) {
            ext2_read_block(inode.i_block[12], (unsigned char*)indirect_buf);
            uint32_t num_ptrs = block_size / 4;
            
            for (uint32_t i = 0; i < num_ptrs && bytes_read < size; i++) {
                uint32_t block = indirect_buf[i];
                if (!block) break;
                
                ext2_read_block(block, block_buf);
                uint32_t chunk = (size - bytes_read > block_size) ? block_size : (size - bytes_read);
                memcpy(buf + bytes_read, block_buf, chunk);
                bytes_read += chunk;
            }
            kfree(indirect_buf);
        }
    }
    
    // 3. Read doubly indirect block (13)
    if (bytes_read < size && inode.i_block[13]) {
        uint32_t* doubly_buf = (uint32_t*)kmalloc(block_size);
        uint32_t* indirect_buf = (uint32_t*)kmalloc(block_size);
        
        if (doubly_buf && indirect_buf) {
            ext2_read_block(inode.i_block[13], (unsigned char*)doubly_buf);
            uint32_t num_ptrs = block_size / 4;
            
            for (uint32_t i = 0; i < num_ptrs && bytes_read < size; i++) {
                uint32_t indirect_block = doubly_buf[i];
                if (!indirect_block) break;
                
                ext2_read_block(indirect_block, (unsigned char*)indirect_buf);
                for (uint32_t j = 0; j < num_ptrs && bytes_read < size; j++) {
                    uint32_t block = indirect_buf[j];
                    if (!block) break;
                    
                    ext2_read_block(block, block_buf);
                    uint32_t chunk = (size - bytes_read > block_size) ? block_size : (size - bytes_read);
                    memcpy(buf + bytes_read, block_buf, chunk);
                    bytes_read += chunk;
                }
            }
        }
        if (doubly_buf) kfree(doubly_buf);
        if (indirect_buf) kfree(indirect_buf);
    }
    
    return bytes_read;
}
