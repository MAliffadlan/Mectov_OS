/* M12: read-only filesystem layer, part 2 — ext2.
 *
 * This is the one place where "reuse the old line" is the right instinct: the
 * on-disk layout is a published format, not an OS design choice, so the parsing
 * here follows ext2 the way src/sys/ext2.c does — the difference is that this is
 * written for the 64-bit kernel's own block API and no longer has to cope with a
 * 32-bit sector cache.
 *
 * Scope: read-only. Superblock + group descriptors to find an inode, inode
 * direct / single-indirect / double-indirect block maps (triple is refused with
 * EFBIG — it needs >64 MiB files at 1 KiB blocks), hole-aware reads (a hole
 * reads as zeros, which is what the format promises), and a per-call path walk
 * over directory blocks.
 *
 * Feature policy: an *incompatible* feature bit we do not implement means the
 * structures on disk may be arranged differently, so mounting refuses instead of
 * guessing. mke2fs -t ext2 produces exactly the classic layout; ext4 with
 * extents/64bit is refused with the offending mask printed.
 */
#include "cpu64.h"
#include "spin64.h"

#define EX_SUPER_OFF 1024
#define EX_SUPER_MAGIC 0xEF53
#define EX_ROOT_INO 2
#define EX_DIRECT 12

/* Incompatible features this reader understands; anything else = refuse. */
#define EX_INCOMPAT_FILETYPE 0x2
#define EX_INCOMPAT_RECOVER 0x8
#define EX_INCOMPAT_FLEX_BG 0x20
#define EX_INCOMPAT_EA_INODE 0x200
#define EX_INCOMPAT_OK \
    (EX_INCOMPAT_FILETYPE | EX_INCOMPAT_RECOVER | EX_INCOMPAT_FLEX_BG | \
     EX_INCOMPAT_EA_INODE)

typedef struct {
    u16 mode;
    u32 size;
    u32 block[15]; /* 12 direct + single + double + triple */
} ex_inode_t;

static u32 rd32(const u8 *p, int off) {
    return (u32)p[off] | ((u32)p[off + 1] << 8) | ((u32)p[off + 2] << 16) |
           ((u32)p[off + 3] << 24);
}
static u16 rd16(const u8 *p, int off) { return (u16)(p[off] | (p[off + 1] << 8)); }

/* Read one filesystem block through the mount's device. */
static int ex_block(fs64_mount_t *m, u32 blk, void *buf) {
    return fs64_dev_read(m, blk, buf, m->block_size);
}

/* Byte offset of the group descriptor table (block after the superblock). */
static u32 ex_gdt_block(const fs64_mount_t *m) { return m->ex_first_block + 1; }

static int ex_inode(fs64_mount_t *m, u32 ino, ex_inode_t *out) {
    if (!ino || ino > m->ex_inodes) return -E64_ENOENT;
    u32 per_group = m->ex_ipg;
    u32 grp = (ino - 1) / per_group;
    u32 idx = (ino - 1) % per_group;

    u32 gdt_bytes = (m->ex_inodes + per_group - 1) / per_group * 32;
    u8 *buf = (u8 *)kmalloc(m->block_size);
    if (!buf) return -E64_ENOMEM;

    /* Descriptor lives at a byte offset inside the GDT (which itself starts at
     * a block boundary); read the containing block. */
    u32 gd_bytes = ex_gdt_block(m) * m->block_size + grp * 32;
    u32 gd_blk = gd_bytes / m->block_size;
    u32 gd_off = gd_bytes % m->block_size;
    (void)gdt_bytes;
    if (ex_block(m, gd_blk, buf)) {
        kfree(buf);
        return -E64_EIO;
    }
    u32 itable = rd32(buf, (int)gd_off + 8);
    if (!itable) {
        kfree(buf);
        return -E64_EIO;
    }

    u32 ino_bytes = idx * m->ex_inode_size;
    u32 ib = itable + ino_bytes / m->block_size;
    u32 io = ino_bytes % m->block_size;
    if (io + 128 > m->block_size) { /* 128 divides every legal block size */
        kfree(buf);
        return -E64_EIO;
    }
    if (ex_block(m, ib, buf)) {
        kfree(buf);
        return -E64_EIO;
    }
    const u8 *p = buf + io;
    out->mode = rd16(p, 0);
    out->size = rd32(p, 4);
    for (int i = 0; i < 15; i++) out->block[i] = rd32(p, 40 + i * 4);
    kfree(buf);
    return 0;
}

/* Entry `idx` of an (indirect) block of block numbers; 0 = hole. */
static u32 ex_ind(fs64_mount_t *m, u32 blk, u32 idx) {
    if (!blk) return 0;
    u32 per = m->block_size / 4;
    if (idx >= per) return 0;
    u8 *buf = (u8 *)kmalloc(m->block_size);
    if (!buf) return 0;
    u32 v = ex_block(m, blk, buf) ? 0 : rd32(buf, (int)idx * 4);
    kfree(buf);
    return v;
}

/* Logical -> physical block. 0 means a hole (or an unimplemented depth). */
static u32 ex_bmap(fs64_mount_t *m, const ex_inode_t *in, u32 logical,
                   int *unsupported) {
    u32 per = m->block_size / 4;
    if (logical < EX_DIRECT) return in->block[logical];
    logical -= EX_DIRECT;
    if (logical < per) return ex_ind(m, in->block[12], logical);
    logical -= per;
    if (logical < per * per) {
        u32 first = ex_ind(m, in->block[13], logical / per);
        return ex_ind(m, first, logical % per);
    }
    if (unsupported) *unsupported = 1; /* triple indirect: >64MiB at 1K blocks */
    return 0;
}

/* Hole-aware data read from an inode (works for files and directories). */
static long ex_data_read(fs64_mount_t *m, const ex_inode_t *in, u64 off,
                         void *buf, u32 len, int *unsupported) {
    if (off >= in->size) return 0;
    if (off + len > in->size) len = (u32)(in->size - off);
    u8 *blk = (u8 *)kmalloc(m->block_size);
    if (!blk) return -E64_ENOMEM;
    long done = 0;
    while (done < (long)len) {
        u64 pos = off + (u64)done;
        u32 lb = (u32)(pos / m->block_size);
        u32 inb = (u32)(pos % m->block_size);
        u32 take = m->block_size - inb;
        if (take > len - (u32)done) take = len - (u32)done;
        u32 pb = ex_bmap(m, in, lb, unsupported);
        if (pb) {
            if (ex_block(m, pb, blk)) {
                kfree(blk);
                return done ? done : -E64_EIO;
            }
            k64_memcpy((u8 *)buf + done, blk + inb, take);
        } else {
            k64_memzero((u8 *)buf + done, take); /* hole reads as zeros */
        }
        done += take;
    }
    kfree(blk);
    return done;
}

/* Walk one directory's blocks looking for `want` (case-sensitive, like ext2). */
typedef struct {
    const char *want;
    fs64_node_t *out;
    int found;
} ex_find_t;

static int ex_dir_scan(fs64_mount_t *m, const ex_inode_t *dir, ex_find_t *find,
                       u32 *index_out, fs64_dirent_t *enum_out) {
    u32 seen = 0;
    int unsupported = 0;
    u8 *blk = (u8 *)kmalloc(m->block_size);
    if (!blk) return -E64_ENOMEM;
    for (u32 lb = 0; lb * m->block_size < dir->size; lb++) {
        u32 pb = ex_bmap(m, dir, lb, &unsupported);
        if (!pb) continue;
        if (ex_block(m, pb, blk)) {
            kfree(blk);
            return -E64_EIO;
        }
        u32 off = 0;
        while (off + 8 <= m->block_size) {
            u32 ino = rd32(blk, (int)off);
            u16 rlen = rd16(blk, (int)off + 4);
            u8 nlen = blk[off + 6];
            if (!rlen || off + rlen > m->block_size) break;
            if (ino && nlen && off + 8 + nlen <= m->block_size) {
                const char *name = (const char *)&blk[off + 8];
                if (find) {
                    char tmp[64];
                    int i = 0;
                    for (; i < nlen && i < 63; i++) tmp[i] = name[i];
                    tmp[i] = '\0';
                    if (i == nlen && k64_streq(tmp, find->want)) {
                        find->out->id = ino;
                        find->found = 1;
                        kfree(blk);
                        return 1;
                    }
                } else if (enum_out) {
                    if (seen == *index_out) {
                        int i = 0;
                        for (; i < nlen && i < (int)sizeof(enum_out->name) - 1; i++)
                            enum_out->name[i] = name[i];
                        enum_out->name[i] = '\0';
                        enum_out->id = ino;
                        enum_out->is_dir = 0; /* filled by the caller */
                        enum_out->size = 0;
                        kfree(blk);
                        return 1;
                    }
                    seen++;
                }
            }
            off += rlen;
        }
    }
    kfree(blk);
    return 0;
}

int fs64_ext_ops_lookup(fs64_mount_t *m, const char *path, fs64_node_t *out) {
    ex_inode_t in;
    if (ex_inode(m, EX_ROOT_INO, &in)) return -E64_EIO;
    if (k64_streq(path, "/")) {
        out->id = EX_ROOT_INO;
        out->size = in.size;
        out->dir_bytes = in.size;
        out->is_dir = 1;
        return 0;
    }
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[64];
        int n = 0;
        while (*p && *p != '/' && n < 63) comp[n++] = *p++;
        comp[n] = '\0';
        while (*p && *p != '/') p++;
        if (n >= 63) return -E64_ENOENT;
        if (!(in.mode & 0x4000)) return -E64_ENOTDIR; /* S_IFDIR */
        ex_find_t f = { comp, out, 0 };
        int rc = ex_dir_scan(m, &in, &f, 0, 0);
        if (rc < 0) return rc;
        if (!f.found) return -E64_ENOENT;
        ex_inode_t next;
        if (ex_inode(m, out->id, &next)) return -E64_EIO;
        out->is_dir = (next.mode & 0x4000) != 0;
        out->size = next.size;
        out->dir_bytes = next.size;
        in = next;
    }
    return 0;
}

int fs64_ext_ops_readdir(fs64_mount_t *m, const fs64_node_t *dir, u32 index,
                         fs64_dirent_t *out) {
    ex_inode_t in;
    if (ex_inode(m, dir->id, &in)) return -E64_EIO;
    u32 idx = index;
    fs64_dirent_t tmp;
    int rc = ex_dir_scan(m, &in, 0, &idx, &tmp);
    if (rc <= 0) return rc;
    /* A directory entry is a name plus an inode number; its type and size live
     * in the inode, so resolve it before handing the entry out. */
    ex_inode_t child;
    if (ex_inode(m, tmp.id, &child)) return -E64_EIO;
    *out = tmp;
    out->is_dir = (child.mode & 0x4000) != 0;
    out->size = child.size;
    return 1;
}

long fs64_ext_ops_read(fs64_mount_t *m, const fs64_node_t *n, u64 off,
                       void *buf, u32 len) {
    ex_inode_t in;
    if (ex_inode(m, n->id, &in)) return -E64_EIO;
    int unsupported = 0;
    long got = ex_data_read(m, &in, off, buf, len, &unsupported);
    if (unsupported) return -E64_EFBIG;
    return got;
}

const fs64_ops_t fs64_ext_ops = {
    fs64_ext_ops_lookup,
    fs64_ext_ops_readdir,
    fs64_ext_ops_read,
};

int fs64_mount_ext2(int blk) {
    const blk64_dev_t *d = blk64_dev(blk);
    if (!d || d->atapi) return -E64_EINVAL;
    int ssz = fs64_dev_sector_size(blk);
    if (ssz <= 0 || ssz > EX_SUPER_OFF) return -E64_EFBIG;

    /* The superblock sits at byte 1024, which is not a block boundary in 4K
     * block filesystems — read it straight off the device instead. */
    u8 *sb = (u8 *)kmalloc(1024);
    if (!sb) return -E64_ENOMEM;
    if (blk64_read(blk, EX_SUPER_OFF / ssz, 1024 / ssz, sb)) {
        kfree(sb);
        return -E64_EIO;
    }
    int rc = -E64_EINVAL;
    do {
        if (rd16(sb, 56) != EX_SUPER_MAGIC) break;
        u32 incompat = rd32(sb, 96);
        if (incompat & ~(u32)EX_INCOMPAT_OK) {
            s_puts("[K64] fs: ext2 unsupported features mask=");
            s_hex32(incompat & ~(u32)EX_INCOMPAT_OK);
            s_puts("\n");
            rc = -E64_EFBIG;
            break;
        }
        u32 log = rd32(sb, 24);
        if (log > 2) {
            rc = -E64_EFBIG; /* >4K blocks: not legal for ext2 */
            break;
        }
        fs64_mount_t *m = fs64_new_mount();
        if (!m) {
            rc = -E64_ENOMEM;
            break;
        }
        m->type = FS64_EXT2;
        m->blk = blk;
        m->block_size = 1024u << log;
        m->blocks = rd32(sb, 4);
        m->ex_inodes = rd32(sb, 0);
        m->ex_first_block = rd32(sb, 20);
        m->ex_bpg = rd32(sb, 32);
        m->ex_ipg = rd32(sb, 40);
        u32 rev = rd32(sb, 76);
        u32 isz = rev >= 1 ? rd32(sb, 88) : 128;
        if (isz < 128 || isz > 1024 || (isz & (isz - 1))) {
            rc = -E64_EFBIG;
            break;
        }
        m->ex_inode_size = isz;
        k64_memzero(m->label, sizeof(m->label));
        for (int i = 0; i < 16 && sb[120 + i]; i++) m->label[i] = (char)sb[120 + i];
        int idx = fs64_commit_mount();
        if (idx < 0) {
            rc = idx;
            break;
        }
        s_puts("[K64] fs: mount");
        s_dec64((u64)idx);
        s_puts(" ext2 blk=");
        s_dec64((u64)blk);
        s_puts(" blocks=");
        s_dec64(m->blocks);
        s_puts(" block_size=");
        s_dec64(m->block_size);
        s_puts(" inodes=");
        s_dec64(m->ex_inodes);
        s_puts(" label=\"");
        s_puts(m->label);
        s_puts("\"\n");
        rc = idx;
    } while (0);
    kfree(sb);
    return rc;
}
