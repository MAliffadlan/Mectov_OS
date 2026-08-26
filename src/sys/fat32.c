// src/sys/fat32.c — FAT32 read/write driver (512-byte sectors, <=16 spc).
//
// Mirrors the ext2 integration pattern: fat32_init() validates the BPB,
// fat32_populate_vfs() maps the volume into the VFS tree as FS_FAT32_* nodes,
// and the VFS layer dispatches reads/writes/creates/removes here. The first
// cluster of each object lives in the VFS node's data_sector field (like
// ext2's ext2_inode). All disk I/O goes through ata_*_sector_drive(), which
// takes ata_lock internally — safe under the vfs_lock held by callers.
//
// LFN: directory scanning reassembles long file names from the 0x0F prefix
// entries (stored in reverse order — the first entry in scan order holds the
// LAST chunk of the name; each carries a checksum of the 8.3 short name that
// must match before the LFN is trusted). Entries created here write an LFN
// chain whenever the name does not round-trip through its uppercase 8.3 form,
// matching Windows behavior; mtools-created volumes (which use ~1-style short
// names) are matched by their reassembled long name, not the generated 8.3.
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
    // Multi-sector PIO (v38.25): one command per up-to-16-sector run.
    int done = 0;
    while (done < count) {
        int batch = ata_batch_limit(lba + done, count - done);
        if (ata_read_sectors_drive(fat32_drive, lba + done, batch,
                                   buf + done * 512) != 0) {
            memset(buf + done * 512, 0, batch * 512);
        }
        done += batch;
    }
}
static void fat32_write_sectors(uint32_t lba, const unsigned char* buf, int count) {
    // Multi-sector PIO (v38.25): one command per up-to-16-sector run.
    int done = 0;
    while (done < count) {
        int batch = ata_batch_limit(lba + done, count - done);
        ata_write_sectors_drive(fat32_drive, lba + done, batch,
                                buf + done * 512);
        done += batch;
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

// --- name handling (SFN 8.3 + LFN) ------------------------------------------

static char fat32_toupper_char(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    return c;
}

// Case-insensitive ASCII compare of two C strings.
static int fat32_ci_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (fat32_toupper_char(*a) != fat32_toupper_char(*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

// Convert a VFS name to an 8.3 short name (space-padded, uppercased) in
// out[11]. Base names longer than 8 chars get the Windows "~1" tail so they
// stay visually familiar; matching never relies on this form (LFN names are
// compared instead), it only has to be valid and unique-ish.
static void fat32_short_name(const char* name, char* out) {
    char base[8], ext[3];
    int bl = 0, el = 0;
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
    if (bl > 8) bl = 8;
    if (el > 3) el = 3;
    // Long base: first 6 chars + "~1" (Windows convention).
    if (dot >= 0 && dot < namelen - 1) {
        int base_len = dot;
        if (base_len > 8) {
            base[6] = '~';
            base[7] = '1';
            bl = 8;
        }
    } else {
        if (namelen > 8) {
            base[6] = '~';
            base[7] = '1';
            bl = 8;
        }
    }
    for (int i = bl; i < 8; i++) base[i] = ' ';
    for (int i = el; i < 3; i++) ext[i] = ' ';
    memcpy(out, base, 8);
    memcpy(out + 8, ext, 3);
}

// Build a display name "NAME.EXT" from a dirent's 8.3 fields.
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

// FAT spec checksum of an 8.3 short name (11 bytes).
static uint8_t fat32_checksum(const unsigned char* sfn11) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sfn11[i]);
    }
    return sum;
}

// Extract the 13 UTF-16LE characters of an LFN entry (raw 32-byte buffer).
static void fat32_lfn_chars(const unsigned char* e, uint16_t out[13]) {
    int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (int i = 0; i < 13; i++) {
        out[i] = (uint16_t)(e[pos[i]] | (e[pos[i] + 1] << 8));
    }
}

// Number of LFN entries a name needs (0 if it round-trips through 8.3).
static int fat32_lfn_count(const char* name) {
    int len = 0;
    while (name[len]) len++;
    return (len + 12) / 13;
}

// Does `name` need an LFN entry? Yes when its uppercase 8.3 form loses
// information (lowercase, too long, or characters the 8.3 form mangles).
static int fat32_needs_lfn(const char* name) {
    char sfn[11];
    fat32_short_name(name, sfn);
    // Rebuild the round-tripped name and compare.
    char rt[40];
    int n = 0;
    for (int i = 0; i < 8 && sfn[i] != ' ' && n < 39; i++) rt[n++] = sfn[i];
    int has_ext = 0;
    for (int i = 8; i < 11; i++) if (sfn[i] != ' ') has_ext = 1;
    if (has_ext && n < 38) {
        rt[n++] = '.';
        for (int i = 8; i < 11 && sfn[i] != ' ' && n < 39; i++) rt[n++] = sfn[i];
    }
    rt[n] = '\0';
    return !fat32_ci_eq(name, rt);
}

// Write 13 UTF-16LE characters of a name chunk into an LFN entry. `remaining`
// is how many real characters of the full name start at `s` (1..13) — the
// rest is a 0x0000 terminator right after the real characters plus 0xFFFF
// padding. Never reads past the name's end (the source string may be shorter
// than 13 chars and must not be scanned past its NUL).
static void fat32_put_lfn_chars(const char* s, int remaining, unsigned char* e) {
    int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    if (remaining > 13) remaining = 13;
    if (remaining < 0) remaining = 0;
    int last = remaining - 1; // index of the last real character
    for (int i = 0; i < 13; i++) {
        uint16_t c;
        if (i < remaining) {
            c = (unsigned char)s[i];
        } else if (i == last + 1) {
            c = 0x0000; // terminator right after the real characters
        } else {
            c = 0xFFFF; // padding
        }
        e[pos[i]] = (unsigned char)(c & 0xFF);
        e[pos[i] + 1] = (unsigned char)((c >> 8) & 0xFF);
    }
}

// --- directory scanning (LFN-aware) -----------------------------------------

// One located directory entry: the real (SFN) entry plus its full name.
typedef struct {
    fat32_dirent_t dirent;   // the SFN entry
    char name[256];          // reassembled name (LFN if valid, else 8.3)
    uint32_t cluster;        // cluster holding the SFN entry
    int offset;              // byte offset of the SFN entry within cluster
    uint32_t lfn_cluster;    // cluster holding the first LFN entry
    int lfn_offset;          // byte offset of the first LFN entry, or -1
} fat32_entry_t;

typedef struct {
    uint16_t chars[130];     // accumulated UTF-16LE characters
    int count;               // highest char index + 1 seen
    uint8_t cksum;           // expected SFN checksum
    uint32_t lfn_cluster;    // cluster of the first LFN entry
    int lfn_offset;          // offset of the first LFN entry
    int pending;             // LFN chain awaiting its SFN entry
} fat32_scan_t;

// Walk every real dirent in a directory chain, calling cb with the
// reassembled name. Returns 1 if cb returned nonzero (early stop).
static int fat32_scan_dir(uint32_t dir_cluster,
                          int (*cb)(const fat32_entry_t* ent, void* arg),
                          void* arg) {
    // Heap-buffered on purpose: populate recurses into subdirectories from
    // inside the callback, and a static/shared buffer would be clobbered by
    // the nested scan (entries after the first subdirectory would be skipped).
    unsigned char* cbuf = (unsigned char*)kmalloc(fat32_spc * 512);
    if (!cbuf) return 0;
    uint32_t cluster = dir_cluster;
    int guard = 0;
    int result = 0;
    fat32_scan_t st;
    memset(&st, 0, sizeof(st));
    while (cluster >= FAT32_ROOT_CLUSTER && cluster <= fat32_max_cluster &&
           cluster < FAT32_EOC_MIN && guard++ < (1 << 20)) {
        fat32_read_cluster(cluster, cbuf);
        int cluster_bytes = fat32_spc * 512;
        int end = 0;
        for (int off = 0; off + 32 <= cluster_bytes; off += 32) {
            unsigned char* raw = cbuf + off;
            unsigned char first = raw[0];
            if (first == FAT32_ENTRY_END) {
                st.pending = 0; // orphaned LFN chain, drop
                end = 1;
                break;
            }
            if (first == FAT32_ENTRY_DELETED) { st.pending = 0; continue; }
            if (raw[11] == FAT32_ATTR_LFN) {
                if (!st.pending) {
                    st.pending = 1;
                    st.cksum = raw[13];
                    st.lfn_cluster = cluster;
                    st.lfn_offset = off;
                    st.count = 0;
                    memset(st.chars, 0, sizeof(st.chars));
                }
                int seq = first & 0x1F;
                if (seq >= 1 && seq <= 20) {
                    int base = (seq - 1) * 13;
                    uint16_t chunk[13];
                    fat32_lfn_chars(raw, chunk);
                    for (int i = 0; i < 13; i++) {
                        if (base + i < 130) st.chars[base + i] = chunk[i];
                    }
                    if (base + 13 > st.count) st.count = base + 13;
                }
                continue;
            }
            // A real (non-LFN) entry.
            fat32_dirent_t* e = (fat32_dirent_t*)raw;
            if (e->attr & FAT32_ATTR_LABEL) { st.pending = 0; continue; }
            if (e->name[0] == '.' && (e->name[1] == ' ' || e->name[1] == '.')) { st.pending = 0; continue; }

            fat32_entry_t ent;
            memset(&ent, 0, sizeof(ent));
            memcpy(&ent.dirent, e, sizeof(fat32_dirent_t));
            ent.cluster = cluster;
            ent.offset = off;
            ent.lfn_offset = -1;
            if (st.pending) {
                if (fat32_checksum((const unsigned char*)e->name) == st.cksum) {
                    int n = 0;
                    for (int i = 0; i < st.count && i < 129 && n < 255; i++) {
                        uint16_t c = st.chars[i];
                        if (c == 0) break;
                        ent.name[n++] = (c < 0x80) ? (char)c : '?';
                    }
                    ent.name[n] = '\0';
                    ent.lfn_cluster = st.lfn_cluster;
                    ent.lfn_offset = st.lfn_offset;
                }
                st.pending = 0;
            }
            if (!ent.name[0]) {
                fat32_dirent_name(&ent.dirent, ent.name, sizeof(ent.name));
            }
            if (cb && cb(&ent, arg)) { result = 1; goto out; }
        }
        if (end) break;
        cluster = fat32_get_next(cluster);
    }
out:
    kfree(cbuf);
    return result;
}

// --- lookup ------------------------------------------------------------------

typedef struct { const char* name; fat32_entry_t* out; } fat32_find_arg_t;
static int fat32_find_cb(const fat32_entry_t* ent, void* arg) {
    fat32_find_arg_t* a = (fat32_find_arg_t*)arg;
    if (fat32_ci_eq(ent->name, a->name)) {
        *a->out = *ent;
        return 1;
    }
    return 0;
}
// Find an entry by its full (LFN or 8.3) name, case-insensitively.
// Returns 1 found, 0 not found.
static int fat32_find_entry(uint32_t dir_cluster, const char* name, fat32_entry_t* out) {
    fat32_find_arg_t a = { name, out };
    return fat32_scan_dir(dir_cluster, fat32_find_cb, &a);
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

// Offset-aware read (v38.53 fd fix): copy len bytes starting at byte `offset`
// of the cluster chain into buf. Skips whole clusters with the chain walk,
// then streams only the sectors inside the window, so lseek()+read() on a
// /fat32 file no longer restarts at offset 0. Returns bytes copied (>=0).
int fat32_read_file_range(uint32_t first_cluster, int offset, char* buf, int len) {
    if (len <= 0 || offset < 0) return 0;
    uint32_t csz = (uint32_t)fat32_spc * 512;
    uint32_t skip = (uint32_t)offset / csz;
    uint32_t in_off = (uint32_t)offset % csz;

    uint32_t cluster = first_cluster;
    int guard = 0;
    while (skip > 0 && guard++ < (1 << 20)) {
        if (cluster < FAT32_ROOT_CLUSTER || cluster > fat32_max_cluster ||
            cluster >= FAT32_EOC_MIN) return 0;          // offset past EOF
        cluster = fat32_get_next(cluster);
    }

    int total = 0;
    guard = 0;
    while (total < len && guard++ < (1 << 20)) {
        if (cluster < FAT32_ROOT_CLUSTER || cluster > fat32_max_cluster ||
            cluster >= FAT32_EOC_MIN) break;             // EOF mid-window
        uint32_t sector = fat32_cluster_sector(cluster);
        for (uint32_t i = in_off / 512; i < fat32_spc && total < len; i++) {
            unsigned char tmp[512];
            fat32_read_sectors(sector + i, tmp, 1);
            uint32_t s_off = (i == in_off / 512) ? in_off % 512 : 0;
            int chunk = len - total;
            if (chunk > 512 - (int)s_off) chunk = 512 - (int)s_off;
            memcpy(buf + total, tmp + s_off, chunk);
            total += chunk;
        }
        in_off = 0;
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
    fat32_entry_t ent;
    if (!fat32_find_entry(parent_cluster, name, &ent)) return -1;
    static unsigned char cbuf[FAT32_MAX_SPC * 512];
    fat32_read_cluster(ent.cluster, cbuf);
    fat32_dirent_t* e = (fat32_dirent_t*)(cbuf + ent.offset);
    e->cluster_high = (uint16_t)((first_cluster >> 16) & 0xFFFF);
    e->cluster_low = (uint16_t)(first_cluster & 0xFFFF);
    e->size = size;
    fat32_write_cluster(ent.cluster, cbuf);
    return 0;
}

// --- public: create / remove / rename ----------------------------------------

// Find a run of `need` consecutive free dirent slots (deleted entries or the
// end marker — the end marker region is free for ever). Returns 0 with the
// start cluster/offset, or -1 if no run fits.
static int fat32_find_free_run(uint32_t dir_cluster, int need,
                               uint32_t* out_cluster, int* out_off) {
    static unsigned char cbuf[FAT32_MAX_SPC * 512];
    uint32_t cluster = dir_cluster;
    int guard = 0;
    uint32_t run_cluster = 0;
    int run_off = -1, run = 0;
    while (cluster >= FAT32_ROOT_CLUSTER && cluster <= fat32_max_cluster &&
           cluster < FAT32_EOC_MIN && guard++ < (1 << 20)) {
        fat32_read_cluster(cluster, cbuf);
        int cluster_bytes = fat32_spc * 512;
        int end = 0;
        for (int off = 0; off + 32 <= cluster_bytes; off += 32) {
            unsigned char first = cbuf[off];
            if (first == FAT32_ENTRY_END) {
                // A run starting here (or at the pending deleted run) is free.
                if (run == 0) { *out_cluster = cluster; *out_off = off; }
                else { *out_cluster = run_cluster; *out_off = run_off; }
                return 0;
            }
            if (first == FAT32_ENTRY_DELETED) {
                if (run == 0) { run_cluster = cluster; run_off = off; }
                run++;
                if (run >= need) { *out_cluster = run_cluster; *out_off = run_off; return 0; }
            } else {
                run = 0;
            }
        }
        if (end) break;
        cluster = fat32_get_next(cluster);
    }
    return -1;
}

// Write a complete entry (optional LFN chain + SFN) for `name` into a free
// slot run of parent_cluster, pointing at the caller-provided cluster/size
// (used both for create — fresh cluster, size 0 — and rename — old cluster).
static int fat32_write_dirent(uint32_t parent_cluster, const char* name, int is_dir,
                              uint32_t cluster, uint32_t size) {
    char sfn[11];
    fat32_short_name(name, sfn);
    int nlfn = fat32_needs_lfn(name) ? fat32_lfn_count(name) : 0;

    uint32_t slot_cluster = 0;
    int slot_off = -1;
    if (fat32_find_free_run(parent_cluster, nlfn + 1, &slot_cluster, &slot_off) != 0) {
        // Extend the directory chain by one cluster and write at its start.
        uint32_t tail = parent_cluster;
        int guard = 0;
        while (fat32_get_next(tail) >= FAT32_ROOT_CLUSTER &&
               fat32_get_next(tail) < FAT32_EOC_MIN && guard++ < (1 << 20)) {
            tail = fat32_get_next(tail);
        }
        uint32_t nxt = fat32_alloc_cluster();
        if (nxt < FAT32_ROOT_CLUSTER) return -1;
        fat32_set_next(tail, nxt);
        static unsigned char zbuf[FAT32_MAX_SPC * 512];
        memset(zbuf, 0, fat32_spc * 512);
        fat32_write_cluster(nxt, zbuf);
        slot_cluster = nxt;
        slot_off = 0;
    }

    static unsigned char cbuf[FAT32_MAX_SPC * 512];
    fat32_read_cluster(slot_cluster, cbuf);
    int off = slot_off;
    uint8_t ck = fat32_checksum((const unsigned char*)sfn);
    int namelen = 0;
    while (name[namelen]) namelen++;

    // LFN entries first (entry i holds chunk (nlfn-1-i), sequence nlfn-i).
    // The 0x40 bit marks the FIRST entry of the chain (farthest from the
    // SFN, holding the last chunk, highest sequence); every other entry is a
    // plain sequence number — mtools/Windows walk the chain from the SFN and
    // stop when they hit the 0x40 terminator, so setting it everywhere would
    // truncate the name after the first entry.
    for (int i = 0; i < nlfn; i++) {
        unsigned char* le = cbuf + off;
        memset(le, 0, 32);
        le[0] = (unsigned char)(((i == 0) ? 0x40 : 0x00) | (nlfn - i));
        int chunk_start = (nlfn - 1 - i) * 13;
        int remaining = namelen - chunk_start;
        fat32_put_lfn_chars(name + chunk_start, remaining, le);
        le[11] = FAT32_ATTR_LFN;
        le[12] = 0;
        le[13] = ck;
        off += 32;
    }
    // SFN entry.
    fat32_dirent_t* e = (fat32_dirent_t*)(cbuf + off);
    memset(e, 0, sizeof(fat32_dirent_t));
    memcpy(e->name, sfn, 11);
    e->attr = is_dir ? FAT32_ATTR_DIR : FAT32_ATTR_ARCHIVE;
    e->cluster_low = (uint16_t)(cluster & 0xFFFF);
    e->cluster_high = (uint16_t)((cluster >> 16) & 0xFFFF);
    e->size = size;
    fat32_write_cluster(slot_cluster, cbuf);
    return 0;
}

uint32_t fat32_create_entry(uint32_t parent_cluster, const char* name, int is_dir) {
    if (!name || !name[0]) return 0;
    fat32_entry_t chk;
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

    if (fat32_write_dirent(parent_cluster, name, is_dir, new_cluster, 0) != 0) {
        fat32_free_chain(new_cluster);
        return 0;
    }
    return new_cluster;
}

// Mark every entry in [start_cluster,start_off) .. (end_cluster,end_off)
// deleted, walking forward through the chain (LFN prefix + SFN removal).
static void fat32_delete_range(uint32_t start_cluster, int start_off,
                               uint32_t end_cluster, int end_off) {
    static unsigned char cbuf[FAT32_MAX_SPC * 512];
    uint32_t c = start_cluster;
    int off = start_off;
    int guard = 0;
    while (c >= FAT32_ROOT_CLUSTER && c <= fat32_max_cluster && guard++ < (1 << 20)) {
        fat32_read_cluster(c, cbuf);
        int cluster_bytes = fat32_spc * 512;
        int dirty = 0;
        while (off + 32 <= cluster_bytes) {
            if (c == end_cluster && off >= end_off) {
                if (dirty) fat32_write_cluster(c, cbuf);
                return;
            }
            cbuf[off] = FAT32_ENTRY_DELETED;
            dirty = 1;
            off += 32;
        }
        if (dirty) fat32_write_cluster(c, cbuf);
        c = fat32_get_next(c);
        off = 0;
    }
}

int fat32_remove_entry(uint32_t parent_cluster, const char* name) {
    fat32_entry_t ent;
    if (!fat32_find_entry(parent_cluster, name, &ent)) return -1;
    uint32_t fc = ((uint32_t)ent.dirent.cluster_high << 16) | ent.dirent.cluster_low;
    fat32_free_chain(fc);
    if (ent.lfn_offset >= 0) {
        fat32_delete_range(ent.lfn_cluster, ent.lfn_offset, ent.cluster, ent.offset + 32);
    } else {
        static unsigned char cbuf[FAT32_MAX_SPC * 512];
        fat32_read_cluster(ent.cluster, cbuf);
        cbuf[ent.offset] = FAT32_ENTRY_DELETED;
        fat32_write_cluster(ent.cluster, cbuf);
    }
    return 0;
}

int fat32_rename_entry(uint32_t parent_cluster, const char* old_name, const char* new_name) {
    fat32_entry_t ent;
    if (!fat32_find_entry(parent_cluster, old_name, &ent)) return -1;
    int is_dir = (ent.dirent.attr & FAT32_ATTR_DIR) != 0;
    uint32_t fc = ((uint32_t)ent.dirent.cluster_high << 16) | ent.dirent.cluster_low;
    uint32_t sz = ent.dirent.size;
    // Remove the old entry (LFN + SFN), then write a fresh one at the freed
    // run pointing at the same cluster/size — the data itself is untouched.
    if (fat32_remove_entry(parent_cluster, old_name) != 0) return -1;
    if (fat32_write_dirent(parent_cluster, new_name, is_dir, fc, sz) != 0) {
        fat32_write_dirent(parent_cluster, old_name, is_dir, fc, sz); // best effort restore
        return -1;
    }
    return 0;
}

// --- VFS population ----------------------------------------------------------

static void fat32_populate_vfs_depth(uint32_t cluster, int vfs_parent_node, int depth);

typedef struct { int vfs_parent; int depth; } fat32_pop_arg_t;
static int fat32_populate_cb(const fat32_entry_t* ent, void* arg) {
    fat32_pop_arg_t* p = (fat32_pop_arg_t*)arg;
    extern int vfs_create_node(const char* name, fs_type_t type, int parent);
    uint32_t fc = ((uint32_t)ent->dirent.cluster_high << 16) | ent->dirent.cluster_low;
    if (ent->dirent.attr & FAT32_ATTR_DIR) {
        int nd = vfs_create_node(ent->name, FS_FAT32_DIR, p->vfs_parent);
        if (nd >= 0) {
            fs_nodes[nd].data_sector = (int)fc;
            fs_nodes[nd].size = 0;
            fat32_populate_vfs_depth(fc, nd, p->depth + 1);
        }
    } else {
        int nf = vfs_create_node(ent->name, FS_FAT32_FILE, p->vfs_parent);
        if (nf >= 0) {
            fs_nodes[nf].data_sector = (int)fc;
            fs_nodes[nf].size = (int)ent->dirent.size;
        }
    }
    return 0;
}

static void fat32_populate_vfs_depth(uint32_t cluster, int vfs_parent_node, int depth) {
    // A crafted image can create directory cycles; never recurse deeper than
    // the VFS can name (mirrors the ext2 driver's depth cap).
    if (depth > 16) return;
    fat32_pop_arg_t p = { vfs_parent_node, depth };
    fat32_scan_dir(cluster, fat32_populate_cb, &p);
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
