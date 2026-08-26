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
static uint32_t ext2_max_groups = 0; // capped at MAX below; read_inode bounds-checks against it

static void ext2_read_block(uint32_t block, unsigned char* buf) {
    // A crafted image can name arbitrary block numbers. Bound the read to the
    // filesystem's own block count and the 4096-sector (2MB) drive BEFORE the
    // block * sectors_per_block multiplication can wrap 32 bits; an out-of-
    // range block reads as zeros instead of hitting the wrong disk area.
    if (block >= sb.s_blocks_count) { memset(buf, 0, block_size); return; }
    uint32_t sectors_per_block = block_size / 512;
    uint32_t start_sector = block * sectors_per_block;
    if (start_sector >= 4096) { memset(buf, 0, block_size); return; }
    if (start_sector + sectors_per_block > 4096) sectors_per_block = 4096 - start_sector;
    // Multi-sector PIO (v38.25): one command per up-to-16-sector run.
    uint32_t done = 0;
    while (done < sectors_per_block) {
        int batch = ata_batch_limit(start_sector + done, sectors_per_block - done);
        ata_read_sectors_drive(ext2_drive, start_sector + done, batch,
                               buf + done * 512);
        done += batch;
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
    
    // Superblock fields come from a disk image the user controls: sanitize them
    // before using them in arithmetic or indexes.
    // - block_size up to 4096 is what the driver's fixed 4KB buffers support;
    //   anything larger (log_block_size >= 3) overflows them via ext2_read_block.
    // - zero block/inode counts would divide by zero below.
    if (sb.s_log_block_size > 2) { write_serial_string("[EXT2] block size too large\n"); return -1; }
    if (sb.s_blocks_per_group == 0 || sb.s_inodes_per_group == 0) {
        write_serial_string("[EXT2] corrupt group counts\n");
        return -1;
    }
    if (sb.s_inode_size < sizeof(ext2_inode_t)) { write_serial_string("[EXT2] inode size too small\n"); return -1; }

    block_size = 1024 << sb.s_log_block_size;
    // The inode accessors copy a record at `(index * inode_size) % block_size`
    // inside a fixed 4KB buffer. A forged s_inode_size that does not divide
    // block_size lets a record span block boundaries, running that memcpy past
    // the buffer and smashing the kernel stack. Real ext2 images (128/256/512
    // byte inodes) always divide evenly.
    if (block_size % sb.s_inode_size != 0) { write_serial_string("[EXT2] inode size not block-aligned\n"); return -1; }
    bgd_block = (block_size == 1024) ? 2 : 1;
    
    // Read Block Group Descriptor Table (assume it fits in one block for simplicity)
    static unsigned char bgd_buf[4096]; 
    ext2_read_block(bgd_block, bgd_buf);
    
    // Allocate memory for BGD table (static array for simplicity, up to 32 groups)
    static ext2_bg_descriptor_t bgds[32];
    uint32_t num_groups = (sb.s_blocks_count + sb.s_blocks_per_group - 1) / sb.s_blocks_per_group;
    if (num_groups > 32) {
        write_serial_string("[EXT2] More than 32 block groups, truncating (files beyond group 32 unreachable)\n");
        num_groups = 32;
    }
    ext2_max_groups = num_groups;
    
    memcpy(bgds, bgd_buf, num_groups * sizeof(ext2_bg_descriptor_t));
    bgd_table = bgds;
    
    return 0;
}

int ext2_read_inode(uint32_t inode_num, ext2_inode_t* inode) {
    if (!bgd_table) return -1;
    if (inode_num < 1 || inode_num > sb.s_inodes_count) return -1;
    if (sb.s_inodes_per_group == 0) return -1;
    
    uint32_t bg = (inode_num - 1) / sb.s_inodes_per_group;
    uint32_t index = (inode_num - 1) % sb.s_inodes_per_group;
    
    // bgd_table only holds the first ext2_max_groups descriptors; a larger
    // filesystem would index out of the static array and read garbage.
    if (bg >= ext2_max_groups || bg >= 32) return -1;
    
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
static void ext2_populate_vfs_depth(uint32_t inode_num, int vfs_parent_node, int depth);

// Scan one directory data block and map its entries into the VFS tree.
static void ext2_scan_dir_block(uint32_t block, int vfs_parent_node, int depth) {
    unsigned char buf[4096];
    ext2_read_block(block, buf);
    
    uint32_t offset = 0;
    while (offset < block_size && offset + 8 <= block_size) {
        ext2_dir_entry_t* entry = (ext2_dir_entry_t*)(buf + offset);
        if (entry->inode == 0 || entry->rec_len == 0) break;
        
        // name_len is a byte on disk but can be forged up to 255; never read
        // past the end of this block's directory data.
        uint32_t name_len = entry->name_len;
        uint32_t avail = block_size - (offset + 8);
        if (name_len > avail) name_len = avail;
        if (name_len > 255) name_len = 255;
        
        char name[256];
        memcpy(name, entry->name, name_len);
        name[name_len] = '\0';
        
        if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0 && strcmp(name, "lost+found") != 0) {
            if (entry->file_type == EXT2_FT_DIR) {
                int new_dir = vfs_create_node(name, FS_EXT2_DIR, vfs_parent_node);
                if (new_dir >= 0) {
                    fs_nodes[new_dir].ext2_inode = entry->inode;
                    ext2_inode_t dinode;
                    if (ext2_read_inode(entry->inode, &dinode) == 0) {
                        fs_nodes[new_dir].size = dinode.i_size;
                        // Carry the real on-disk ownership into the VFS node
                        // so ls -l / permission checks see what the image has.
                        fs_nodes[new_dir].uid = dinode.i_uid;
                        fs_nodes[new_dir].gid = dinode.i_gid;
                        fs_nodes[new_dir].mode = dinode.i_mode & 0x1FF;
                    }
                    ext2_populate_vfs_depth(entry->inode, new_dir, depth + 1);
                }
            } else if (entry->file_type == EXT2_FT_REG_FILE) {
                int new_file = vfs_create_node(name, FS_EXT2_FILE, vfs_parent_node);
                if (new_file >= 0) {
                    fs_nodes[new_file].ext2_inode = entry->inode;
                    ext2_inode_t finode;
                    // Ignoring the return left finode uninitialized on failure,
                    // storing garbage i_size into the VFS node.
                    if (ext2_read_inode(entry->inode, &finode) == 0) {
                        fs_nodes[new_file].size = finode.i_size;
                        fs_nodes[new_file].uid = finode.i_uid;
                        fs_nodes[new_file].gid = finode.i_gid;
                        fs_nodes[new_file].mode = finode.i_mode & 0x1FF;
                    }
                }
            }
        }
        offset += entry->rec_len;
    }
}

void ext2_populate_vfs(uint32_t inode_num, int vfs_parent_node) {
    ext2_populate_vfs_depth(inode_num, vfs_parent_node, 0);
}

static void ext2_populate_vfs_depth(uint32_t inode_num, int vfs_parent_node, int depth) {
    // A crafted image can create directory cycles (".."-by-inode tricks) that
    // would recurse until the kernel stack overflows at boot.
    if (depth > 16) return;
    ext2_inode_t inode;
    if (ext2_read_inode(inode_num, &inode) != 0) return;
    
    if (!(inode.i_mode & 0x4000)) return; // Not a directory
    
    // Direct blocks
    for (int i = 0; i < 12; i++) {
        uint32_t block = inode.i_block[i];
        if (!block) continue;
        ext2_scan_dir_block(block, vfs_parent_node, depth);
    }
    
    // Singly indirect blocks (directories can grow here)
    if (inode.i_block[12]) {
        uint32_t* ind = (uint32_t*)kmalloc(block_size);
        if (ind) {
            ext2_read_block(inode.i_block[12], (unsigned char*)ind);
            for (uint32_t i = 0; i < block_size / 4; i++) {
                if (ind[i]) ext2_scan_dir_block(ind[i], vfs_parent_node, depth);
            }
            kfree(ind);
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

// ============================================================
// Write support (persistence) — create/overwrite/delete on disk
// ============================================================

extern void write_serial_string(const char*);

static void ext2_write_block(uint32_t block, unsigned char* buf) {
    // Same bounds as ext2_read_block: never write past the filesystem's block
    // count or the 4096-sector drive.
    if (block >= sb.s_blocks_count) return;
    uint32_t sectors_per_block = block_size / 512;
    uint32_t start_sector = block * sectors_per_block;
    if (start_sector >= 4096) return;
    if (start_sector + sectors_per_block > 4096) sectors_per_block = 4096 - start_sector;
    // Multi-sector PIO (v38.25): one command per up-to-16-sector run.
    uint32_t done = 0;
    while (done < sectors_per_block) {
        int batch = ata_batch_limit(start_sector + done, sectors_per_block - done);
        ata_write_sectors_drive(ext2_drive, start_sector + done, batch,
                                buf + done * 512);
        done += batch;
    }
}

// Persist the in-memory superblock + BGD table back to disk. The superblock is
// always at byte offset 1024 (sectors 2-3), independent of block size; the BGD
// table occupies one block at bgd_block (ext2_init caps us at 32 groups, which
// fits in a single 4K block).
static void ext2_sync_super(void) {
    if (!bgd_table) return;
    unsigned char buf[1024];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &sb, sizeof(ext2_superblock_t));
    ata_write_sector_drive(ext2_drive, 2, buf);
    ata_write_sector_drive(ext2_drive, 3, buf + 512);
    // Heap-allocated: keeps this function's stack footprint small — it is
    // called from the allocators which already hold bitmap buffers.
    unsigned char* bgd_buf = (unsigned char*)kmalloc(4096);
    if (!bgd_buf) return;
    memset(bgd_buf, 0, 4096);
    memcpy(bgd_buf, bgd_table, ext2_max_groups * sizeof(ext2_bg_descriptor_t));
    ext2_write_block(bgd_block, bgd_buf);
    kfree(bgd_buf);
}

int ext2_write_inode(uint32_t inode_num, ext2_inode_t* inode) {
    if (!bgd_table) return -1;
    if (inode_num < 1 || inode_num > sb.s_inodes_count) return -1;
    if (sb.s_inodes_per_group == 0) return -1;
    
    uint32_t bg = (inode_num - 1) / sb.s_inodes_per_group;
    uint32_t index = (inode_num - 1) % sb.s_inodes_per_group;
    if (bg >= ext2_max_groups || bg >= 32) return -1;
    
    uint32_t inode_table_block = bgd_table[bg].bg_inode_table;
    uint32_t inode_size = (sb.s_rev_level == 0) ? 128 : sb.s_inode_size;
    uint32_t block_index = (index * inode_size) / block_size;
    uint32_t offset = (index * inode_size) % block_size;
    
    // Read-modify-write: preserve the rest of the inode slot (inode_size may be
    // larger than ext2_inode_t, e.g. 256-byte inodes hold extended data).
    unsigned char buf[4096];
    ext2_read_block(inode_table_block + block_index, buf);
    memcpy(buf + offset, inode, sizeof(ext2_inode_t));
    ext2_write_block(inode_table_block + block_index, buf);
    return 0;
}

// Zero the full on-disk inode slot so a freshly allocated inode carries no
// stale data (mkfs stores free-inode list pointers inside unused slots).
static void ext2_zero_inode_slot(uint32_t inode_num) {
    if (inode_num < 1 || inode_num > sb.s_inodes_count || sb.s_inodes_per_group == 0) return;
    uint32_t bg = (inode_num - 1) / sb.s_inodes_per_group;
    uint32_t index = (inode_num - 1) % sb.s_inodes_per_group;
    if (bg >= ext2_max_groups || bg >= 32) return;
    uint32_t inode_table_block = bgd_table[bg].bg_inode_table;
    uint32_t inode_size = (sb.s_rev_level == 0) ? 128 : sb.s_inode_size;
    uint32_t block_index = (index * inode_size) / block_size;
    uint32_t offset = (index * inode_size) % block_size;
    unsigned char buf[4096];
    ext2_read_block(inode_table_block + block_index, buf);
    memset(buf + offset, 0, inode_size);
    ext2_write_block(inode_table_block + block_index, buf);
}

// --- Block allocator (single bitmap block per group; <= 32768 blocks/group) ---

// True if a block is filesystem metadata (boot/superblock, BGD table, group
// bitmaps or inode tables) and must never be handed out, even if a corrupt or
// crafted bitmap marks it free. Defense in depth on top of the bitmap.
static int ext2_block_is_metadata(uint32_t bg, uint32_t block) {
    if (block == 0) return 1;              // boot block always reserved
    if (block < sb.s_first_data_block) return 1;
    if (block == bgd_block) return 1;      // BGD table
    if (bg >= ext2_max_groups) return 0;
    ext2_bg_descriptor_t* d = &bgd_table[bg];
    if (block == d->bg_block_bitmap || block == d->bg_inode_bitmap) return 1;
    uint32_t inode_size = (sb.s_rev_level == 0) ? 128 : sb.s_inode_size;
    uint32_t tbl_blocks = (sb.s_inodes_per_group * inode_size + block_size - 1) / block_size;
    if (block >= d->bg_inode_table && block < d->bg_inode_table + tbl_blocks) return 1;
    return 0;
}

static uint32_t ext2_alloc_block(void) {
    if (!bgd_table || sb.s_blocks_per_group == 0) return 0;
    unsigned char* bmp = (unsigned char*)kmalloc(4096);
    if (!bmp) return 0;
    for (uint32_t bg = 0; bg < ext2_max_groups; bg++) {
        uint32_t bmp_block = bgd_table[bg].bg_block_bitmap;
        ext2_read_block(bmp_block, bmp);
        uint32_t remaining = sb.s_blocks_count - bg * sb.s_blocks_per_group;
        if (remaining > sb.s_blocks_per_group) remaining = sb.s_blocks_per_group;
        if (remaining > 32768) remaining = 32768; // bitmap buffer cap
        for (uint32_t i = 0; i < remaining; i++) {
            uint32_t candidate = bg * sb.s_blocks_per_group + i;
            if (ext2_block_is_metadata(bg, candidate)) continue;
            if (!(bmp[i >> 3] & (1 << (i & 7)))) {
                bmp[i >> 3] |= (1 << (i & 7));
                ext2_write_block(bmp_block, bmp);
                sb.s_free_blocks_count--;
                bgd_table[bg].bg_free_blocks_count--;
                ext2_sync_super();
                kfree(bmp);
                return candidate;
            }
        }
    }
    kfree(bmp);
    return 0; // full
}

static void ext2_free_block(uint32_t block) {
    if (!bgd_table || block == 0) return;
    uint32_t bg = block / sb.s_blocks_per_group;
    if (bg >= ext2_max_groups) return;
    uint32_t idx = block % sb.s_blocks_per_group;
    if (idx >= 32768) return;
    uint32_t bmp_block = bgd_table[bg].bg_block_bitmap;
    unsigned char* bmp = (unsigned char*)kmalloc(4096);
    if (!bmp) return;
    ext2_read_block(bmp_block, bmp);
    if (bmp[idx >> 3] & (1 << (idx & 7))) {
        bmp[idx >> 3] &= ~(1 << (idx & 7));
        ext2_write_block(bmp_block, bmp);
        sb.s_free_blocks_count++;
        bgd_table[bg].bg_free_blocks_count++;
        ext2_sync_super();
    }
    kfree(bmp);
}

// --- Inode allocator ---

static uint32_t ext2_alloc_inode(void) {
    if (!bgd_table || sb.s_inodes_per_group == 0) return 0;
    if (sb.s_inodes_per_group > 32768) return 0; // bitmap buffer cap
    unsigned char* bmp = (unsigned char*)kmalloc(4096);
    if (!bmp) return 0;
    for (uint32_t bg = 0; bg < ext2_max_groups; bg++) {
        uint32_t bmp_block = bgd_table[bg].bg_inode_bitmap;
        ext2_read_block(bmp_block, bmp);
        uint32_t remaining = sb.s_inodes_count - bg * sb.s_inodes_per_group;
        if (remaining > sb.s_inodes_per_group) remaining = sb.s_inodes_per_group;
        if (remaining > 32768) remaining = 32768;
        for (uint32_t i = 0; i < remaining; i++) {
            if (!(bmp[i >> 3] & (1 << (i & 7)))) {
                bmp[i >> 3] |= (1 << (i & 7));
                ext2_write_block(bmp_block, bmp);
                sb.s_free_inodes_count--;
                bgd_table[bg].bg_free_inodes_count--;
                ext2_sync_super();
                kfree(bmp);
                return bg * sb.s_inodes_per_group + i + 1;
            }
        }
    }
    kfree(bmp);
    return 0; // full
}

static void ext2_free_inode(uint32_t inode_num) {
    if (!bgd_table || inode_num < 1) return;
    uint32_t bg = (inode_num - 1) / sb.s_inodes_per_group;
    if (bg >= ext2_max_groups) return;
    uint32_t idx = (inode_num - 1) % sb.s_inodes_per_group;
    uint32_t bmp_block = bgd_table[bg].bg_inode_bitmap;
    unsigned char* bmp = (unsigned char*)kmalloc(4096);
    if (!bmp) return;
    ext2_read_block(bmp_block, bmp);
    if (bmp[idx >> 3] & (1 << (idx & 7))) {
        bmp[idx >> 3] &= ~(1 << (idx & 7));
        ext2_write_block(bmp_block, bmp);
        sb.s_free_inodes_count++;
        bgd_table[bg].bg_free_inodes_count++;
        ext2_sync_super();
    }
    kfree(bmp);
}

// --- Inode block-pointer helpers (direct + singly indirect) ---

// Return the on-disk block for logical block lb; allocate it if alloc is set.
// 0 means "no block / allocation failed".
static uint32_t ext2_get_block(ext2_inode_t* inode, uint32_t lb, int alloc) {
    if (lb < 12) {
        if (inode->i_block[lb] == 0 && alloc) inode->i_block[lb] = ext2_alloc_block();
        return inode->i_block[lb];
    }
    uint32_t per = block_size / 4;
    if (lb < 12 + per) {
        if (inode->i_block[12] == 0) {
            if (!alloc) return 0;
            inode->i_block[12] = ext2_alloc_block();
            if (inode->i_block[12] == 0) return 0;
            unsigned char zb[4096];
            memset(zb, 0, sizeof(zb));
            ext2_write_block(inode->i_block[12], zb);
        }
        uint32_t* ind = (uint32_t*)kmalloc(block_size);
        if (!ind) return 0;
        ext2_read_block(inode->i_block[12], (unsigned char*)ind);
        uint32_t idx = lb - 12;
        uint32_t blk = ind[idx];
        if (blk == 0 && alloc) {
            blk = ext2_alloc_block();
            if (blk) {
                ind[idx] = blk;
                ext2_write_block(inode->i_block[12], (unsigned char*)ind);
            }
        }
        kfree(ind);
        return blk;
    }
    return 0; // doubly indirect unsupported for writes
}

// Clear the pointer for logical block lb (used when truncating). Frees the
// indirect table block too once it becomes empty.
static void ext2_clear_block_ptr(ext2_inode_t* inode, uint32_t lb) {
    if (lb < 12) {
        inode->i_block[lb] = 0;
        return;
    }
    uint32_t per = block_size / 4;
    if (lb < 12 + per && inode->i_block[12]) {
        uint32_t* ind = (uint32_t*)kmalloc(block_size);
        if (!ind) return;
        ext2_read_block(inode->i_block[12], (unsigned char*)ind);
        ind[lb - 12] = 0;
        int empty = 1;
        for (uint32_t i = 0; i < per; i++) {
            if (ind[i]) { empty = 0; break; }
        }
        if (empty) {
            uint32_t indblk = inode->i_block[12];
            inode->i_block[12] = 0;
            ext2_free_block(indblk);
        } else {
            ext2_write_block(inode->i_block[12], (unsigned char*)ind);
        }
        kfree(ind);
    }
}

// Count allocated blocks (data + indirect tables) in 512-byte units.
static uint32_t ext2_count_blocks(ext2_inode_t* inode) {
    uint32_t n = 0;
    for (int i = 0; i < 12; i++) if (inode->i_block[i]) n++;
    if (inode->i_block[12]) {
        n++;
        uint32_t* ind = (uint32_t*)kmalloc(block_size);
        if (ind) {
            ext2_read_block(inode->i_block[12], (unsigned char*)ind);
            for (uint32_t i = 0; i < block_size / 4; i++) if (ind[i]) n++;
            kfree(ind);
        }
    }
    return n * (block_size / 512);
}

uint32_t ext2_inode_size(uint32_t inode_num) {
    ext2_inode_t inode;
    if (ext2_read_inode(inode_num, &inode) != 0) return 0;
    return inode.i_size;
}

int ext2_get_stats(uint32_t* total_blocks, uint32_t* free_blocks,
                   uint32_t* total_inodes, uint32_t* free_inodes,
                   uint32_t* block_size_out) {
    if (!bgd_table) return -1;
    if (total_blocks)  *total_blocks  = sb.s_blocks_count;
    if (free_blocks)   *free_blocks   = sb.s_free_blocks_count;
    if (total_inodes)  *total_inodes  = sb.s_inodes_count;
    if (free_inodes)   *free_inodes   = sb.s_free_inodes_count;
    if (block_size_out) *block_size_out = block_size;
    return 0;
}

// --- File data write ---

int ext2_write_file_data(uint32_t inode_num, const char* buf, int size) {
    if (size < 0) return -1;
    ext2_inode_t inode;
    if (ext2_read_inode(inode_num, &inode) != 0) return -1;
    
    uint32_t blocks_needed = ((uint32_t)size + block_size - 1) / block_size;
    uint32_t old_blocks = (inode.i_size + block_size - 1) / block_size;
    
    // Shrink: free blocks past the new end, then clear their pointers.
    for (uint32_t lb = blocks_needed; lb < old_blocks; lb++) {
        uint32_t blk = ext2_get_block(&inode, lb, 0);
        if (blk) {
            ext2_clear_block_ptr(&inode, lb);
            ext2_free_block(blk);
        }
    }
    
    // Write/grow: allocate blocks on demand and copy data.
    uint32_t written = 0;
    uint32_t lb = 0;
    while (written < (uint32_t)size) {
        uint32_t blk = ext2_get_block(&inode, lb, 1);
        if (!blk) {
            write_serial_string("[EXT2] write: no free blocks\n");
            break;
        }
        uint32_t chunk = (uint32_t)size - written;
        if (chunk > block_size) chunk = block_size;
        unsigned char bbuf[4096];
        if (chunk < block_size) {
            ext2_read_block(blk, bbuf); // preserve tail bytes of a partial block
        } else {
            memset(bbuf, 0, sizeof(bbuf));
        }
        memcpy(bbuf, buf + written, chunk);
        ext2_write_block(blk, bbuf);
        written += chunk;
        lb++;
    }
    
    // Zero the tail of the last data block past EOF (stale bytes from a
    // previous larger file must not survive a truncate).
    if (written > 0 && (written % block_size) != 0) {
        uint32_t blk = ext2_get_block(&inode, lb - 1, 0);
        if (blk) {
            unsigned char bbuf[4096];
            ext2_read_block(blk, bbuf);
            memset(bbuf + (written % block_size), 0, block_size - (written % block_size));
            ext2_write_block(blk, bbuf);
        }
    }
    
    inode.i_size = written;
    inode.i_blocks = ext2_count_blocks(&inode);
    if (inode.i_mtime == 0) inode.i_mtime = 0x5F000000;
    if (ext2_write_inode(inode_num, &inode) != 0) return -1;
    return (int)written;
}

// Offset-aware read (v38.53 fd fix): copy len bytes starting at logical byte
// `offset` of the file into buf. Walks only the blocks the window touches
// (unlike ext2_read_file_data's whole-file read), so lseek()+read() on an
// /ext2 file no longer restarts at offset 0 and a big sequential read is not
// O(n^2). Sparse holes read as zeros. Returns bytes copied (>=0) or -1.
int ext2_read_file_range(uint32_t inode_num, uint32_t offset, char* buf, int len) {
    if (len <= 0) return 0;
    ext2_inode_t inode;
    if (ext2_read_inode(inode_num, &inode) != 0) return -1;
    if (offset >= inode.i_size) return 0;
    uint32_t avail = inode.i_size - offset;
    if ((uint32_t)len > avail) len = (int)avail;

    unsigned char bbuf[4096];   // block_size <= 4096 (same bound as the writer)
    uint32_t done = 0;
    while (done < (uint32_t)len) {
        uint32_t pos = offset + done;
        uint32_t lb = pos / block_size;
        uint32_t inb = pos % block_size;
        uint32_t blk = ext2_get_block(&inode, lb, 0);
        uint32_t chunk = block_size - inb;
        if (chunk > (uint32_t)len - done) chunk = (uint32_t)len - done;
        if (blk == 0) {
            memset(buf + done, 0, chunk);           // sparse hole
        } else {
            ext2_read_block(blk, bbuf);
            memcpy(buf + done, bbuf + inb, chunk);
        }
        done += chunk;
    }
    return (int)done;
}

// --- Directory entry manipulation ---

// On-disk record length for a name (8 header + name, padded to 4 bytes).
static uint32_t ext2_entry_size(uint32_t name_len) {
    return (8 + name_len + 3) & ~3u;
}

// Try to place a new entry inside one directory block. Returns 1 when placed
// (reusing a hole or splitting a slack entry), 0 when the block has no room.
static int ext2_try_dir_block(uint32_t block, uint32_t inode_num,
                              const char* name, uint32_t name_len,
                              uint32_t need, uint8_t file_type) {
    unsigned char buf[4096];
    ext2_read_block(block, buf);
    uint32_t off = 0;
    while (off + 8 <= block_size) {
        ext2_dir_entry_t* e = (ext2_dir_entry_t*)(buf + off);
        uint32_t rec = e->rec_len;
        if (rec == 0 || rec > block_size - off) rec = block_size - off; // defensive
        if (e->inode == 0 && rec >= need) {
            // Reuse a deleted-entry hole.
            e->inode = inode_num;
            e->name_len = (uint8_t)name_len;
            e->file_type = file_type;
            memcpy(e->name, name, name_len);
            if (rec > need) {
                e->rec_len = (uint16_t)need;
                ext2_dir_entry_t* hole = (ext2_dir_entry_t*)(buf + off + need);
                hole->inode = 0;
                hole->rec_len = (uint16_t)(rec - need);
                hole->name_len = 0;
                hole->file_type = 0;
            } else {
                e->rec_len = (uint16_t)need;
            }
            ext2_write_block(block, buf);
            return 1;
        } else if (e->inode != 0 && rec >= ext2_entry_size(e->name_len) + need) {
            // Split a slack entry (e.g. the big lost+found tail entry).
            uint32_t cur = ext2_entry_size(e->name_len);
            uint32_t old_rec = e->rec_len;
            e->rec_len = (uint16_t)cur;
            ext2_dir_entry_t* ne = (ext2_dir_entry_t*)(buf + off + cur);
            ne->inode = inode_num;
            ne->rec_len = (uint16_t)(old_rec - cur);
            ne->name_len = (uint8_t)name_len;
            ne->file_type = file_type;
            memcpy(ne->name, name, name_len);
            ext2_write_block(block, buf);
            return 1;
        }
        off += rec;
    }
    return 0;
}

// Append a block to a directory inode (direct, then singly indirect).
static int ext2_dir_append_block(uint32_t dir_inode, uint32_t newblk) {
    ext2_inode_t d2;
    if (ext2_read_inode(dir_inode, &d2) != 0) return -1;
    for (int i = 0; i < 12; i++) {
        if (!d2.i_block[i]) {
            d2.i_block[i] = newblk;
            d2.i_size += block_size;
            d2.i_blocks = ext2_count_blocks(&d2);
            ext2_write_inode(dir_inode, &d2);
            return 0;
        }
    }
    if (!d2.i_block[12]) {
        uint32_t indblk = ext2_alloc_block();
        if (!indblk) return -1;
        d2.i_block[12] = indblk;
        unsigned char zb[4096];
        memset(zb, 0, sizeof(zb));
        ext2_write_block(indblk, zb);
    }
    uint32_t* ind2 = (uint32_t*)kmalloc(block_size);
    if (!ind2) return -1;
    ext2_read_block(d2.i_block[12], (unsigned char*)ind2);
    int placed = 0;
    for (uint32_t i = 0; i < block_size / 4; i++) {
        if (!ind2[i]) { ind2[i] = newblk; placed = 1; break; }
    }
    if (placed) ext2_write_block(d2.i_block[12], (unsigned char*)ind2);
    kfree(ind2);
    if (!placed) return -1;
    d2.i_size += block_size;
    d2.i_blocks = ext2_count_blocks(&d2);
    ext2_write_inode(dir_inode, &d2);
    return 0;
}

// Add (inode_num, name, file_type) to directory inode dir_inode.
static int ext2_dir_add_entry(uint32_t dir_inode, uint32_t inode_num,
                              const char* name, uint8_t file_type) {
    uint32_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return -1;
    uint32_t need = ext2_entry_size(name_len);
    
    ext2_inode_t dinode;
    if (ext2_read_inode(dir_inode, &dinode) != 0) return -1;
    
    for (int i = 0; i < 12; i++) {
        if (dinode.i_block[i] &&
            ext2_try_dir_block(dinode.i_block[i], inode_num, name, name_len, need, file_type))
            return 0;
    }
    if (dinode.i_block[12]) {
        uint32_t* ind = (uint32_t*)kmalloc(block_size);
        if (ind) {
            ext2_read_block(dinode.i_block[12], (unsigned char*)ind);
            for (uint32_t i = 0; i < block_size / 4; i++) {
                if (ind[i] &&
                    ext2_try_dir_block(ind[i], inode_num, name, name_len, need, file_type)) {
                    kfree(ind);
                    return 0;
                }
            }
            kfree(ind);
        }
    }
    
    // No room anywhere: allocate a fresh directory block.
    uint32_t newblk = ext2_alloc_block();
    if (!newblk) return -1;
    unsigned char buf[4096];
    memset(buf, 0, sizeof(buf));
    ext2_dir_entry_t* e = (ext2_dir_entry_t*)buf;
    e->inode = inode_num;
    e->rec_len = (uint16_t)block_size;
    e->name_len = (uint8_t)name_len;
    e->file_type = file_type;
    memcpy(e->name, name, name_len);
    ext2_write_block(newblk, buf);
    if (ext2_dir_append_block(dir_inode, newblk) != 0) {
        ext2_free_block(newblk);
        return -1;
    }
    return 0;
}

// Free an inode and every block it references (direct + singly + doubly
// indirect), then release the inode itself.
static void ext2_rm_inode(uint32_t inode_num) {
    ext2_inode_t inode;
    if (ext2_read_inode(inode_num, &inode) != 0) return;
    for (int i = 0; i < 12; i++) {
        if (inode.i_block[i]) {
            ext2_free_block(inode.i_block[i]);
            inode.i_block[i] = 0;
        }
    }
    if (inode.i_block[12]) {
        uint32_t* ind = (uint32_t*)kmalloc(block_size);
        if (ind) {
            ext2_read_block(inode.i_block[12], (unsigned char*)ind);
            for (uint32_t i = 0; i < block_size / 4; i++) {
                if (ind[i]) ext2_free_block(ind[i]);
            }
            kfree(ind);
        }
        ext2_free_block(inode.i_block[12]);
        inode.i_block[12] = 0;
    }
    if (inode.i_block[13]) {
        uint32_t* db = (uint32_t*)kmalloc(block_size);
        uint32_t* ind2 = (uint32_t*)kmalloc(block_size);
        if (db && ind2) {
            ext2_read_block(inode.i_block[13], (unsigned char*)db);
            for (uint32_t i = 0; i < block_size / 4; i++) {
                if (!db[i]) continue;
                ext2_read_block(db[i], (unsigned char*)ind2);
                for (uint32_t j = 0; j < block_size / 4; j++) {
                    if (ind2[j]) ext2_free_block(ind2[j]);
                }
                ext2_free_block(db[i]);
            }
        }
        if (db) kfree(db);
        if (ind2) kfree(ind2);
        ext2_free_block(inode.i_block[13]);
        inode.i_block[13] = 0;
    }
    ext2_write_inode(inode_num, &inode);
    ext2_free_inode(inode_num);
    // Zero the whole inode slot so a freed inode carries no stale mode/size/
    // blocks on disk — otherwise fsck reports "i_blocks is N, should be 0" and
    // "zero-length directory" for the orphaned slot.
    ext2_zero_inode_slot(inode_num);
}

// Remove a directory entry without freeing the inode (used by rename).
// Returns 0 and the victim inode via *victim_out, or -2 if not found.
static int ext2_unlink_entry(uint32_t parent_inode, const char* name, uint32_t* victim_out) {
    ext2_inode_t dinode;
    if (ext2_read_inode(parent_inode, &dinode) != 0) return -1;
    uint32_t name_len = strlen(name);
    
    // Collect the directory's data blocks (direct + singly indirect).
    uint32_t* blocks = (uint32_t*)kmalloc(12 * 4 + block_size);
    if (!blocks) return -1;
    int nblocks = 0;
    for (int i = 0; i < 12; i++) {
        if (dinode.i_block[i]) blocks[nblocks++] = dinode.i_block[i];
    }
    if (dinode.i_block[12]) {
        uint32_t* ind = (uint32_t*)kmalloc(block_size);
        if (ind) {
            ext2_read_block(dinode.i_block[12], (unsigned char*)ind);
            for (uint32_t i = 0; i < block_size / 4; i++) {
                if (ind[i]) blocks[nblocks++] = ind[i];
            }
            kfree(ind);
        }
    }
    
    int found_block = -1;
    uint32_t found_off = 0, prev_off = 0xFFFFFFFF;
    for (int b = 0; b < nblocks && found_block < 0; b++) {
        unsigned char buf[4096];
        ext2_read_block(blocks[b], buf);
        uint32_t off = 0;
        prev_off = 0xFFFFFFFF;
        while (off + 8 <= block_size) {
            ext2_dir_entry_t* e = (ext2_dir_entry_t*)(buf + off);
            uint32_t rec = e->rec_len;
            if (rec == 0 || rec > block_size - off) rec = block_size - off;
            // e->name is only rec - 8 bytes long on the block; a forged name_len
            // could otherwise make the memcmp read past the block buffer.
            uint32_t avail_name = rec - 8;
            if (e->inode != 0 && e->name_len == name_len && name_len <= avail_name &&
                memcmp(e->name, name, name_len) == 0) {
                found_block = b;
                found_off = off;
                *victim_out = e->inode;
                break;
            }
            if (e->inode != 0) prev_off = off;
            off += rec;
        }
    }
    if (found_block < 0) {
        kfree(blocks);
        return -2;
    }
    
    unsigned char buf[4096];
    ext2_read_block(blocks[found_block], buf);
    ext2_dir_entry_t* e = (ext2_dir_entry_t*)(buf + found_off);
    uint32_t rec = e->rec_len;
    if (found_off + rec >= block_size && prev_off != 0xFFFFFFFF) {
        // Last entry in the block: absorb it into the previous entry so the
        // directory stays compact. The previous entry must extend all the way
        // to the END of the block, not just by the removed entry's rec_len:
        // holes (deleted entries) may sit between prev and the removed entry,
        // and summing rec_lens would leave the tail unaccounted and corrupt
        // the directory (fsck: "directory corrupted").
        ext2_dir_entry_t* prev = (ext2_dir_entry_t*)(buf + prev_off);
        prev->rec_len = (uint16_t)(block_size - prev_off);
        e->inode = 0;
        e->name_len = 0;
        e->file_type = 0;
    } else {
        // Leave a hole; a later create reuses it via ext2_try_dir_block.
        e->inode = 0;
        e->name_len = 0;
        e->file_type = 0;
    }
    ext2_write_block(blocks[found_block], buf);
    kfree(blocks);
    return 0;
}

// --- Public write API ---

uint32_t ext2_create_entry(uint32_t parent_inode, const char* name, uint8_t file_type) {
    if (!bgd_table) return 0;
    uint32_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return 0;
    
    uint32_t inode_num = ext2_alloc_inode();
    if (!inode_num) return 0;
    ext2_zero_inode_slot(inode_num);
    
    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = (file_type == EXT2_FT_DIR) ? 0x41ED : 0x81A4; // dir 0755, reg 0644
    inode.i_links_count = (file_type == EXT2_FT_DIR) ? 2 : 1;
    inode.i_size = (file_type == EXT2_FT_DIR) ? block_size : 0;
    inode.i_atime = inode.i_ctime = inode.i_mtime = 0x5F000000;
    
    if (file_type == EXT2_FT_DIR) {
        uint32_t blk = ext2_alloc_block();
        if (!blk) {
            ext2_free_inode(inode_num);
            return 0;
        }
        inode.i_block[0] = blk;
        inode.i_blocks = block_size / 512;
        unsigned char dblk[4096];
        memset(dblk, 0, sizeof(dblk));
        ext2_dir_entry_t* e1 = (ext2_dir_entry_t*)dblk;
        e1->inode = inode_num;
        e1->rec_len = 12;
        e1->name_len = 1;
        e1->file_type = EXT2_FT_DIR;
        memcpy(e1->name, ".", 1);
        ext2_dir_entry_t* e2 = (ext2_dir_entry_t*)(dblk + 12);
        e2->inode = parent_inode;
        e2->rec_len = (uint16_t)(block_size - 12);
        e2->name_len = 2;
        e2->file_type = EXT2_FT_DIR;
        memcpy(e2->name, "..", 2);
        ext2_write_block(blk, dblk);
    }
    ext2_write_inode(inode_num, &inode);
    
    if (ext2_dir_add_entry(parent_inode, inode_num, name, file_type) != 0) {
        ext2_rm_inode(inode_num); // rollback
        return 0;
    }
    
    if (file_type == EXT2_FT_DIR) {
        ext2_inode_t pinode;
        if (ext2_read_inode(parent_inode, &pinode) == 0) {
            pinode.i_links_count++;
            ext2_write_inode(parent_inode, &pinode);
        }
    }
    return inode_num;
}

int ext2_remove_entry(uint32_t parent_inode, const char* name) {
    uint32_t victim = 0;
    int rc = ext2_unlink_entry(parent_inode, name, &victim);
    if (rc != 0) return rc;
    
    ext2_inode_t vinode;
    int is_dir = 0;
    if (ext2_read_inode(victim, &vinode) == 0 && (vinode.i_mode & 0x4000)) is_dir = 1;
    
    if (is_dir) {
        ext2_inode_t pinode;
        if (ext2_read_inode(parent_inode, &pinode) == 0 && pinode.i_links_count > 0) {
            pinode.i_links_count--;
            ext2_write_inode(parent_inode, &pinode);
        }
    }
    ext2_rm_inode(victim);
    return 0;
}

int ext2_rename_entry(uint32_t parent_inode, const char* old_name,
                      const char* new_name, uint32_t inode_num) {
    uint32_t victim = 0;
    if (ext2_unlink_entry(parent_inode, old_name, &victim) != 0) return -1;
    ext2_inode_t in;
    uint8_t ft = EXT2_FT_REG_FILE;
    if (ext2_read_inode(inode_num, &in) == 0 && (in.i_mode & 0x4000)) ft = EXT2_FT_DIR;
    if (ext2_dir_add_entry(parent_inode, inode_num, new_name, ft) != 0) {
        // Rollback: try to restore the old name so a failed rename does not
        // lose the file. Best effort — the old slot is now a reusable hole.
        ext2_dir_add_entry(parent_inode, inode_num, old_name, ft);
        return -1;
    }
    return 0;
}
