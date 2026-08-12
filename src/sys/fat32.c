// src/sys/fat32.c — FAT32 read/write driver (512-byte sectors, <=16 spc).
//
// Mirrors the ext2 integration pattern: fat32_init() validates the BPB,
// fat32_populate_vfs() maps the volume into the VFS tree as FS_FAT32_* nodes,
// and the VFS layer dispatches reads/writes/creates/removes here. The first
// cluster of each object lives in the VFS node's data_sector field (like
// ext2's ext2_inode). All disk I/O goes through ata_*_sector_drive(), which
// takes ata_lock internally — safe under the vfs_lock held by callers.
#include "../include/fat32.h"
#include "../include/ata.h"
#include "../include/utils.h"
#include "../include/serial.h"
#include "../include/vfs.h"
#include "../include/mem.h"

static int fat32_drive = -1;
static uint16_t fat32_bps = 512;
static uint8_t  fat32_spc = 1;
static uint16_t fat32_reserved = 0;
static uint8_t  fat32_num_fats = 0;
static uint32_t fat32_sectors_per_fat = 0;
static uint32_t fat32_root = FAT32_ROOT_CLUSTER;
static uint32_t fat32_total_sectors = 0;
static uint32_t fat32_first_data = 0;   // first sector of the data region
static uint32_t fat32_max_cluster = 0;  // highest valid cluster number

// --- low-level disk / FAT access -------------------------------------------

static void fat32_read_sectors(uint32_t lba, unsigned char* buf, int count) {
    for (int i = 0; i < count; i++) {
        if (ata_read_sector_drive(fat32_drive, lba + i, buf + i * 512) != 0) {
            memset(buf + i * 512, 0, 512);
        }
    }
}
static void fat32_write_sectors(uint32_t lba, const unsigned char* buf, int count) {
    for (int i = 0; i < count; i++) {
        ata_write_sector_drive(fat32_drive, lba + i, (unsigned char*)(buf + i * 512));
    }
}

static uint32_t fat32_cluster_sector(uint32_t cluster) {
    return fat32_first_data + (cluster - FAT32_ROOT_CLUSTER) * fat32_spc;
}

// Read one cluster (fat32_spc * 512 bytes) into buf. buf must be >= 8 KB.
static void fat32_read_cluster(uint32_t cluster, unsigned char* buf) {
    if (cluster < FAT32_ROOT_CLUSTER || cluster > fat32_max_cluster) {
        memset(buf, 0, fat32_spc * 512);
        return;
    }
    fat32_read_sectors(fat32_cluster_sector(cluster), buf, fat32_spc);
}
static void fat32_write_cluster(uint32_t cluster, const unsigned char* buf) {
    if (cluster < FAT32_ROOT_CLUSTER || cluster > fat32_max_cluster) return;
    fat32_write_sectors(fat32_cluster_sector(cluster), buf, fat32_spc);
}

// FAT entry for `cluster`. Mirrors into every FAT copy on write so fsck-style
// tools (mtools etc.) see a consistent table.
static uint32_t fat32_get_next(uint32_t cluster) {
    uint32_t fat_off = cluster * 4;
    uint32_t sec = fat32_reserved + fat_off / fat32_bps;
    uint32_t off = fat_off % fat32_bps;
    unsigned char b[512];
    fat32_read_sectors(sec, b, 1);
    uint32_t v;
    memcpy(&v, b + off, 4);
    return v & 0x0FFFFFFF;
}
static void fat32_set_next(uint32_t cluster, uint32_t value) {
    uint32_t fat_off = cluster * 4;
    uint32_t sec = fat32_reserved + fat_off / fat32_bps;
    uint32_t off = fat_off % fat32_bps;
    unsigned char b[512];
    fat32_read_sectors(sec, b, 1);
    uint32_t v;
    memcpy(&v, b + off, 4);
    v = (v & 0xF0000000) | (value & 0x0FFFFFFF);
    memcpy(b + off, &v, 4);
    for (uint8_t i = 0; i < fat32_num_fats; i++) {
        fat32_write_sectors(fat32_reserved + (uint32_t)i * fat32_sectors_per_fat + sec - fat32_reserved, b, 1);
    }
}

// Allocate a free cluster (scan from 2), mark it EOC, return it (or 0).
static uint32_t fat32_alloc_cluster(void) {
    for (uint32_t c = FAT32_ROOT_CLUSTER; c <= fat32_max_cluster; c++) {
        if (fat32_get_next(c) == FAT32_FREE_CLUSTER) {
            fat32_set_next(c, FAT32_EOC_MIN);
            return c;
        }
    }
    return 0;
}

// Mark every cluster in a chain free.
static void fat32_free_chain(uint32_t cluster) {
    uint32_t c = cluster;
    int guard = 0;
    while (c >= FAT32_ROOT_CLUSTER && c <= fat32_max_cluster && guard++ < 1 << 20) {
        uint32_t nxt = fat32_get_next(c);
        fat32_set_next(c, FAT32_FREE_CLUSTER);
        if (nxt < FAT32_ROOT_CLUSTER || nxt > fat32_max_cluster || nxt >= FAT32_EOC_MIN) break;
        c = nxt;
    }
}

// --- name handling (SFN 8.3) ------------------------------------------------

static char fat32_toupper_char(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    return c;
}

// Convert a VFS name to an 8.3 short name (space-padded, uppercased) in out[11].
static void fat32_short_name(const char* name, char* out) {
    char base[8], ext[3];
    int bl = 0, el = 0;
    // Split at the last dot.
    int dot = -1;
    for (int i = 0; name[i] && i < MAX_FILENAME; i++) if (name[i] == '.') dot = i;
    int namelen = 0;
    while (name[namelen]) namelen++;
    if (dot >= 0 && dot < namelen - 1) {
        for (int i = 0; i < dot && i < 8; i++) base[bl++] = fat32_toupper_char(name[i]);
        for (int i = dot + 1; name[i] && i - dot - 1 < 3; i++) ext[el++] = fat32_toupper_char(name[i]);
    } else {
        for (int i = 0; name[i] && i < 8; i++) base[bl++] = fat32_toupper_char(name[i]);
    }
    for (int i = bl; i < 8; i++) base[i] = ' ';
    for (int i = el; i < 3; i++) ext[i] = ' ';
    memcpy(out, base, 8);
    memcpy(out + 8, ext, 3);
}

// Case-insensitive compare of a dirent's 8.3 name against a VFS name.
static int fat32_name_matches(const fat32_dirent_t* e, const char* name) {
    char short_name[11];
    fat32_short_name(name, short_name);
    for (int i = 0; i < 11; i++) {
        if (e->name[i] != short_name[i]) return 0;
    }
    return 1;
}

// Build a display name "NAME.EXT" from a dirent.
static void fat32_dirent_name(const fat32_dirent_t* e, char* out, int out_size) {
    int n = 0;
    int i;
    for (i = 0; i < 8 && e->name[i] != ' ' && n < out_size - 1; i++) out[n++] = e->name[i];
    int has_ext = 0;
    for (i = 0; i < 3; i++) if (e->ext[i] != ' ') has_ext = 1;
    if (has_ext && n < out_size - 1) {
        out[n++] = '.';
        for (i = 0; i < 3 && e->ext[i] != ' ' && n < out_size - 1; i++) out[n++] = e->ext[i];
    }
    out[n] = '\0';
}

// --- directory scanning ------------------------------------------------------

// Walk every dirent in a directory chain, calling fn for each non-LFN,
// non-dot, non-label entry. Returns 1 if the callback returned nonzero
// (match — iteration stops early), 0 if the whole chain was scanned without
// a match.
typedef int (*fat32_dir_cb)(const fat32_dirent_t* e, void* arg);
static int fat32_iter_dir(uint32_t dir_cluster, fat32_dir_cb fn, void* arg) {
    static unsigned char cbuf[FAT32_MAX_SPC * 512];
    uint32_t cluster = dir_cluster;
    int guard = 0;
    while (cluster >= FAT32_ROOT_CLUSTER && cluster <= fat32_max_cluster &&
           cluster < FAT32_EOC_MIN && guard++ < (1 << 20)) {
        fat32_read_cluster(cluster, cbuf);
        int cluster_bytes = fat32_spc * 512;
        int end = 0;
        for (int off = 0; off + 32 <= cluster_bytes; off += 32) {
            fat32_dirent_t* e = (fat32_dirent_t*)(cbuf + off);
            if ((unsigned char)e->name[0] == FAT32_ENTRY_END) { end = 1; break; }
            if ((unsigned char)e->name[0] == FAT32_ENTRY_DELETED) continue;
            if (e->attr & FAT32_ATTR_LFN) continue;
            if (e->attr & FAT32_ATTR_LABEL) continue;
            if (e->name[0] == '.' && (e->name[1] == ' ' || e->name[1] == '.')) continue;
            if (fn && fn(e, arg)) return 1;
        }
        if (end) break;
        cluster = fat32_get_next(cluster);
    }
    return 0;
}

// --- lookup ------------------------------------------------------------------

typedef struct { const char* name; fat32_dirent_t* out; } fat32_find_arg_t;
static int fat32_find_cb(const fat32_dirent_t* e, void* arg) {
    fat32_find_arg_t* a = (fat32_find_arg_t*)arg;
    if (fat32_name_matches(e, a->name)) {
        memcpy(a->out, e, sizeof(fat32_dirent_t));
        return 1;
    }
    return 0;
}
// Find a dirent by name in dir_cluster. Returns 1 found, 0 not found.
static int fat32_find_entry(uint32_t dir_cluster, const char* name, fat32_dirent_t* out) {
    fat32_find_arg_t a = { name, out };
    return fat32_iter_dir(dir_cluster, fat32_find_cb, &a);
}

// --- public: read / write ----------------------------------------------------

int fat32_read_file(uint32_t first_cluster, char* buf, int max_size) {
    if (max_size <= 0) return 0;
    uint32_t cluster = first_cluster;
    int total = 0;
    int guard = 0;
    while (cluster >= FAT32_ROOT_CLUSTER && cluster <= fat32_max_cluster &&
           cluster < FAT32_EOC_MIN && total < max_size && guard++ < (1 << 20)) {
        uint32_t sector = fat32_cluster_sector(cluster);
        for (int i = 0; i < fat32_spc && total < max_size; i++) {
            unsigned char tmp[512];
            fat32_read_sectors(sector + i, tmp, 1);
            int chunk = max_size - total;
            if (chunk > 512) chunk = 512;
            memcpy(buf + total, tmp, chunk);
            total += chunk;
        }
        cluster = fat32_get_next(cluster);
    }
    return total;
}

int fat32_write_file(uint32_t first_cluster, const char* buf, int size) {
    static unsigned char cbuf[FAT32_MAX_SPC * 512];
    if (size <= 0) {
        // Truncate to zero: keep the first cluster (if any) so the dirent
        // stays valid, free everything after it.
        if (first_cluster >= FAT32_ROOT_CLUSTER) {
            uint32_t tail = fat32_get_next(first_cluster);
            if (tail >= FAT32_ROOT_CLUSTER && tail < FAT32_EOC_MIN) {
                fat32_free_chain(tail);
                fat32_set_next(first_cluster, FAT32_EOC_MIN);
            }
            return (int)first_cluster;
        }
        return 0;
    }
    uint32_t cluster = first_cluster;
    if (cluster < FAT32_ROOT_CLUSTER) {
        cluster = fat32_alloc_cluster();
        if (cluster < FAT32_ROOT_CLUSTER) return -1;
    }
    uint32_t new_first = cluster;
    int remaining = size;
    int written = 0;
    uint32_t prev = cluster;
    while (remaining > 0) {
        memset(cbuf, 0, fat32_spc * 512);
        int chunk = remaining;
        if (chunk > fat32_spc * 512) chunk = fat32_spc * 512;
        memcpy(cbuf, buf + written, chunk);
        fat32_write_cluster(cluster, cbuf);
        written += chunk;
        remaining -= chunk;
        prev = cluster;
        cluster = fat32_get_next(cluster);
        if (remaining > 0) {
            if (cluster < FAT32_ROOT_CLUSTER || cluster > fat32_max_cluster || cluster >= FAT32_EOC_MIN) {
                uint32_t nxt = fat32_alloc_cluster();
                if (nxt < FAT32_ROOT_CLUSTER) {
                    fat32_free_chain(new_first);
                    return -1;
                }
                fat32_set_next(prev, nxt);
                cluster = nxt;
            }
        }
    }
    // Truncate: free any clusters left after the written data.
    if (cluster >= FAT32_ROOT_CLUSTER && cluster <= fat32_max_cluster && cluster < FAT32_EOC_MIN) {
        fat32_free_chain(cluster);
        fat32_set_next(prev, FAT32_EOC_MIN);
    }
    return (int)new_first;
}

int fat32_update_dirent(uint32_t parent_cluster, const char* name,
                        uint32_t first_cluster, uint32_t size) {
    static unsigned char cbuf[FAT32_MAX_SPC * 512];
    uint32_t cluster = parent_cluster;
    char short_name[11];
    fat32_short_name(name, short_name);
    int guard = 0;
    while (cluster >= FAT32_ROOT_CLUSTER && cluster <= fat32_max_cluster &&
           cluster < FAT32_EOC_MIN && guard++ < (1 << 20)) {
        fat32_read_cluster(cluster, cbuf);
        int cluster_bytes = fat32_spc * 512;
        for (int off = 0; off + 32 <= cluster_bytes; off += 32) {
            fat32_dirent_t* e = (fat32_dirent_t*)(cbuf + off);
            if ((unsigned char)e->name[0] == FAT32_ENTRY_END) return -1;
            if ((unsigned char)e->name[0] == FAT32_ENTRY_DELETED) continue;
            if (e->attr & FAT32_ATTR_LFN) continue;
            if (memcmp(e->name, short_name, 11) == 0) {
                e->cluster_high = (uint16_t)((first_cluster >> 16) & 0xFFFF);
                e->cluster_low  = (uint16_t)(first_cluster & 0xFFFF);
                e->size = size;
                fat32_write_cluster(cluster, cbuf);
                return 0;
            }
        }
        cluster = fat32_get_next(cluster);
    }
    return -1;
}

// --- public: create / remove / rename ----------------------------------------

// Find the byte offset of a free dirent slot in dir_cluster's chain.
// Returns cluster + offset via out pointers, or -1 if the directory is full.
static int fat32_find_free_slot(uint32_t dir_cluster, uint32_t* slot_cluster, int* slot_off) {
    static unsigned char cbuf[FAT32_MAX_SPC * 512];
    uint32_t cluster = dir_cluster;
    int guard = 0;
    while (cluster >= FAT32_ROOT_CLUSTER && cluster <= fat32_max_cluster &&
           cluster < FAT32_EOC_MIN && guard++ < (1 << 20)) {
        fat32_read_cluster(cluster, cbuf);
        int cluster_bytes = fat32_spc * 512;
        for (int off = 0; off + 32 <= cluster_bytes; off += 32) {
            fat32_dirent_t* e = (fat32_dirent_t*)(cbuf + off);
            if ((unsigned char)e->name[0] == FAT32_ENTRY_END ||
                (unsigned char)e->name[0] == FAT32_ENTRY_DELETED) {
                *slot_cluster = cluster;
                *slot_off = off;
                return 0;
            }
        }
        cluster = fat32_get_next(cluster);
    }
    return -1;
}

uint32_t fat32_create_entry(uint32_t parent_cluster, const char* name, int is_dir) {
    if (!name || !name[0]) return 0;
    fat32_dirent_t chk;
    if (fat32_find_entry(parent_cluster, name, &chk)) return 0; // already exists

    uint32_t new_cluster = 0;
    if (is_dir) {
        new_cluster = fat32_alloc_cluster();
        if (new_cluster < FAT32_ROOT_CLUSTER) return 0;
        // Initialize "." and "..".
        static unsigned char dbuf[FAT32_MAX_SPC * 512];
        memset(dbuf, 0, fat32_spc * 512);
        fat32_dirent_t* dot = (fat32_dirent_t*)dbuf;
        memset(dot, 0, sizeof(fat32_dirent_t));
        memset(dot->name, ' ', 8);
        memset(dot->ext, ' ', 3);
        dot->name[0] = '.';
        dot->attr = FAT32_ATTR_DIR;
        dot->cluster_low = (uint16_t)(new_cluster & 0xFFFF);
        dot->cluster_high = (uint16_t)((new_cluster >> 16) & 0xFFFF);
        fat32_dirent_t* dotdot = (fat32_dirent_t*)(dbuf + 32);
        memset(dotdot, 0, sizeof(fat32_dirent_t));
        memset(dotdot->name, ' ', 8);
        memset(dotdot->ext, ' ', 3);
        dotdot->name[0] = '.';
        dotdot->name[1] = '.';
        dotdot->attr = FAT32_ATTR_DIR;
        dotdot->cluster_low = (uint16_t)(parent_cluster & 0xFFFF);
        dotdot->cluster_high = (uint16_t)((parent_cluster >> 16) & 0xFFFF);
        fat32_write_cluster(new_cluster, dbuf);
    } else {
        // Allocate a cluster for new files too (mkfs.fat/mtools and Windows
        // do the same): the returned cluster is the create-success signal, and
        // an empty file stays addressable by its dirent. Data writes later
        // overwrite the whole cluster, so no stale bytes can leak.
        new_cluster = fat32_alloc_cluster();
        if (new_cluster < FAT32_ROOT_CLUSTER) return 0;
    }

    // Locate a free slot in the parent directory (extend it if full).
    uint32_t slot_cluster = 0;
    int slot_off = -1;
    if (fat32_find_free_slot(parent_cluster, &slot_cluster, &slot_off) != 0) {
        // Extend the directory chain by one cluster and write at its start.
        uint32_t tail = parent_cluster;
        int guard = 0;
        while (fat32_get_next(tail) >= FAT32_ROOT_CLUSTER && fat32_get_next(tail) < FAT32_EOC_MIN &&
               guard++ < (1 << 20)) tail = fat32_get_next(tail);
        uint32_t nxt = fat32_alloc_cluster();
        if (nxt < FAT32_ROOT_CLUSTER) { if (is_dir) fat32_free_chain(new_cluster); return 0; }
        fat32_set_next(tail, nxt);
        slot_cluster = nxt;
        slot_off = 0;
        static unsigned char zbuf[FAT32_MAX_SPC * 512];
        memset(zbuf, 0, fat32_spc * 512);
        fat32_write_cluster(nxt, zbuf);
    }

    static unsigned char cbuf[FAT32_MAX_SPC * 512];
    fat32_read_cluster(slot_cluster, cbuf);
    fat32_dirent_t* e = (fat32_dirent_t*)(cbuf + slot_off);
    memset(e, 0, sizeof(fat32_dirent_t));
    fat32_short_name(name, e->name);
    e->attr = is_dir ? FAT32_ATTR_DIR : FAT32_ATTR_ARCHIVE;
    e->cluster_low = (uint16_t)(new_cluster & 0xFFFF);
    e->cluster_high = (uint16_t)((new_cluster >> 16) & 0xFFFF);
    e->size = 0;
    fat32_write_cluster(slot_cluster, cbuf);
    return new_cluster;
}

int fat32_remove_entry(uint32_t parent_cluster, const char* name) {
    static unsigned char cbuf[FAT32_MAX_SPC * 512];
    uint32_t cluster = parent_cluster;
    char short_name[11];
    fat32_short_name(name, short_name);
    int guard = 0;
    while (cluster >= FAT32_ROOT_CLUSTER && cluster <= fat32_max_cluster &&
           cluster < FAT32_EOC_MIN && guard++ < (1 << 20)) {
        fat32_read_cluster(cluster, cbuf);
        int cluster_bytes = fat32_spc * 512;
        for (int off = 0; off + 32 <= cluster_bytes; off += 32) {
            fat32_dirent_t* e = (fat32_dirent_t*)(cbuf + off);
            if ((unsigned char)e->name[0] == FAT32_ENTRY_END) return -1;
            if ((unsigned char)e->name[0] == FAT32_ENTRY_DELETED) continue;
            if (e->attr & FAT32_ATTR_LFN) continue;
            if (memcmp(e->name, short_name, 11) == 0) {
                uint32_t fc = ((uint32_t)e->cluster_high << 16) | e->cluster_low;
                fat32_free_chain(fc);
                e->name[0] = (char)FAT32_ENTRY_DELETED;
                fat32_write_cluster(cluster, cbuf);
                return 0;
            }
        }
        cluster = fat32_get_next(cluster);
    }
    return -1;
}

int fat32_rename_entry(uint32_t parent_cluster, const char* old_name, const char* new_name) {
    static unsigned char cbuf[FAT32_MAX_SPC * 512];
    uint32_t cluster = parent_cluster;
    char old_short[11], new_short[11];
    fat32_short_name(old_name, old_short);
    fat32_short_name(new_name, new_short);
    int guard = 0;
    while (cluster >= FAT32_ROOT_CLUSTER && cluster <= fat32_max_cluster &&
           cluster < FAT32_EOC_MIN && guard++ < (1 << 20)) {
        fat32_read_cluster(cluster, cbuf);
        int cluster_bytes = fat32_spc * 512;
        for (int off = 0; off + 32 <= cluster_bytes; off += 32) {
            fat32_dirent_t* e = (fat32_dirent_t*)(cbuf + off);
            if ((unsigned char)e->name[0] == FAT32_ENTRY_END) return -1;
            if ((unsigned char)e->name[0] == FAT32_ENTRY_DELETED) continue;
            if (e->attr & FAT32_ATTR_LFN) continue;
            if (memcmp(e->name, old_short, 11) == 0) {
                memcpy(e->name, new_short, 11);
                fat32_write_cluster(cluster, cbuf);
                return 0;
            }
        }
        cluster = fat32_get_next(cluster);
    }
    return -1;
}

// --- VFS population ----------------------------------------------------------

static void fat32_populate_vfs_depth(uint32_t cluster, int vfs_parent_node, int depth) {
    // A crafted image can create directory cycles; never recurse deeper than
    // the VFS can name (mirrors the ext2 driver's depth cap).
    if (depth > 16) return;
    extern int vfs_create_node(const char* name, fs_type_t type, int parent);
    static unsigned char cbuf[FAT32_MAX_SPC * 512];
    uint32_t c = cluster;
    int guard = 0;
    char dname[256];
    while (c >= FAT32_ROOT_CLUSTER && c <= fat32_max_cluster &&
           c < FAT32_EOC_MIN && guard++ < (1 << 20)) {
        fat32_read_cluster(c, cbuf);
        int cluster_bytes = fat32_spc * 512;
        int end = 0;
        for (int off = 0; off + 32 <= cluster_bytes; off += 32) {
            fat32_dirent_t* e = (fat32_dirent_t*)(cbuf + off);
            if ((unsigned char)e->name[0] == FAT32_ENTRY_END) { end = 1; break; }
            if ((unsigned char)e->name[0] == FAT32_ENTRY_DELETED) continue;
            if (e->attr & FAT32_ATTR_LFN) continue;
            if (e->attr & FAT32_ATTR_LABEL) continue;
            if (e->name[0] == '.' && (e->name[1] == ' ' || e->name[1] == '.')) continue;
            fat32_dirent_name(e, dname, sizeof(dname));
            uint32_t fc = ((uint32_t)e->cluster_high << 16) | e->cluster_low;
            if (e->attr & FAT32_ATTR_DIR) {
                int nd = vfs_create_node(dname, FS_FAT32_DIR, vfs_parent_node);
                if (nd >= 0) {
                    fs_nodes[nd].data_sector = (int)fc;
                    fs_nodes[nd].size = 0;
                    fat32_populate_vfs_depth(fc, nd, depth + 1);
                }
            } else {
                int nf = vfs_create_node(dname, FS_FAT32_FILE, vfs_parent_node);
                if (nf >= 0) {
                    fs_nodes[nf].data_sector = (int)fc;
                    fs_nodes[nf].size = (int)e->size;
                }
            }
        }
        if (end) break;
        c = fat32_get_next(c);
    }
}

void fat32_populate_vfs(uint32_t root_cluster, int vfs_parent_node) {
    fat32_populate_vfs_depth(root_cluster, vfs_parent_node, 0);
}

// --- init + stats ------------------------------------------------------------

uint32_t fat32_root_cluster(void) { return fat32_root; }

int fat32_init(int drive) {
    write_serial_string("[FAT32] fat32_init start\n");
    fat32_drive = drive;

    unsigned char b[512];
    if (ata_read_sector_drive(drive, 0, b) != 0) {
        write_serial_string("[FAT32] no drive\n");
        return -1;
    }
    // Boot-sector magic is bytes 55 AA on disk (little-endian 0xAA55).
    if (b[510] != 0x55 || b[511] != 0xAA ||
        memcmp(b + FAT32_FS_TYPE_OFF, "FAT32   ", 8) != 0) {
        write_serial_string("[FAT32] not FAT32 (bad signature)\n");
        return -1;
    }

    fat32_bps = *(uint16_t*)(b + 11);
    fat32_spc = b[13];
    fat32_reserved = *(uint16_t*)(b + 14);
    fat32_num_fats = b[16];
    uint16_t tot16;
    memcpy(&tot16, b + 19, 2);
    memcpy(&fat32_sectors_per_fat, b + 36, 4);
    memcpy(&fat32_root, b + 44, 4);
    uint32_t tot32;
    memcpy(&tot32, b + 32, 4);

    // Sanitize the BPB before using it in arithmetic (the image is user data).
    if (fat32_bps != 512) { write_serial_string("[FAT32] only 512-byte sectors\n"); return -1; }
    if (fat32_spc == 0 || fat32_spc > FAT32_MAX_SPC) { write_serial_string("[FAT32] bad spc\n"); return -1; }
    if (fat32_reserved == 0 || fat32_num_fats == 0 || fat32_num_fats > 4) { write_serial_string("[FAT32] bad geometry\n"); return -1; }
    if (fat32_sectors_per_fat == 0) { write_serial_string("[FAT32] bad spf\n"); return -1; }
    if (fat32_root < FAT32_ROOT_CLUSTER) fat32_root = FAT32_ROOT_CLUSTER;

    fat32_total_sectors = tot32 ? tot32 : (uint32_t)tot16;
    if (fat32_total_sectors == 0) { write_serial_string("[FAT32] zero sectors\n"); return -1; }
    fat32_first_data = (uint32_t)fat32_reserved + (uint32_t)fat32_num_fats * fat32_sectors_per_fat;
    if (fat32_first_data >= fat32_total_sectors) { write_serial_string("[FAT32] bad first_data\n"); return -1; }
    fat32_max_cluster = FAT32_ROOT_CLUSTER + (fat32_total_sectors - fat32_first_data) / fat32_spc - 1;

    write_serial_string("[FAT32] ok: root=");
    write_serial_hex((unsigned char)(fat32_root & 0xFF));
    write_serial_string("\n");
    return 0;
}

int fat32_get_stats(uint32_t* total_clusters, uint32_t* free_clusters, uint32_t* cluster_bytes) {
    if (fat32_drive < 0 || fat32_max_cluster == 0) return -1;
    uint32_t total = fat32_max_cluster - FAT32_ROOT_CLUSTER + 1;
    uint32_t free = 0;
    for (uint32_t c = FAT32_ROOT_CLUSTER; c <= fat32_max_cluster; c++) {
        if (fat32_get_next(c) == FAT32_FREE_CLUSTER) free++;
    }
    *total_clusters = total;
    *free_clusters = free;
    *cluster_bytes = (uint32_t)fat32_spc * 512;
    return 0;
}
