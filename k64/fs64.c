/* M12: read-only filesystem layer, part 1 — core + ISO9660.
 *
 * Two things ride on this:
 *   - the boot CD (ATAPI, 2048-byte blocks, ISO9660 with level-1 names), which
 *     needs nothing but the M11 driver — a filesystem the kernel can mount on
 *     any machine it boots on, with no disk image at all;
 *   - an ATA disk with ext2 (k64/ext64.c), which is what the old line's VFS
 *     actually stored things on.
 *
 * Shape: a tiny mount table plus one ops table per filesystem type. Paths are
 * absolute, '/'-separated, handled per call (no open-file table yet) — a
 * read-only first cut that the Ring-3 API in syscall64.c can drive directly.
 * Writes, caching, long names (Rock Ridge/Joliet) and a unified namespace are
 * explicitly not here yet.
 *
 * Error codes are negative errno numbers so the syscall layer can pass them
 * through: -2 ENOENT, -5 EIO, -12 ENOMEM, -20 ENOTDIR, -21 EISDIR, -22 EINVAL,
 * -27 EFBIG (a structure this build refuses to read).
 */
#include "cpu64.h"
#include "spin64.h"


static fs64_mount_t fs_mounts[FS64_MAX_MOUNTS];
static int fs_n;
static spin64_t fs_lock = SPIN64_INIT; /* one read at a time: shared bounce bufs */

/* ---- block device helper shared by both backends ---- */

/* Read `bytes` of filesystem block `block` from the mount's device. Blocks must
 * be a whole number of device sectors (512B ATA / 2048B CD): true for ext2
 * with 1K/4K blocks on a disk and for ISO9660 on a CD, refused otherwise. */
int fs64_dev_read(fs64_mount_t *m, u64 block, void *buf, u32 bytes) {
    const blk64_dev_t *d = blk64_dev(m->blk);
    if (!d || !d->sector_size) return -E64_EIO;
    if (m->block_size % d->sector_size) return -E64_EFBIG;
    u32 per = m->block_size / d->sector_size;
    u32 sectors = (bytes + m->block_size - 1) / m->block_size * per;
    return blk64_read(m->blk, block * per, sectors, buf);
}

int fs64_dev_sector_size(int blk) {
    const blk64_dev_t *d = blk64_dev(blk);
    return d ? (int)d->sector_size : 0;
}

/* ---- mount table ---- */

int fs64_mounts(void) { return fs_n; }

const fs64_mount_t *fs64_mount(int mnt) {
    if (mnt < 0 || mnt >= fs_n) return 0;
    return &fs_mounts[mnt];
}

static const fs64_ops_t *ops_of(const fs64_mount_t *m) {
    if (m->type == FS64_ISO9660) return &fs64_iso_ops;
    if (m->type == FS64_EXT2) return &fs64_ext_ops;
    return 0;
}

/* A backend reserves a slot, fills it in, and only then commits it: a mount
 * that fails halfway is never visible to the path API (the slot is simply
 * reused, already zeroed, by the next attempt). */
fs64_mount_t *fs64_new_mount(void) {
    if (fs_n >= FS64_MAX_MOUNTS) return 0;
    fs64_mount_t *m = &fs_mounts[fs_n];
    k64_memzero(m, sizeof(*m));
    return m;
}

int fs64_commit_mount(void) {
    if (fs_n >= FS64_MAX_MOUNTS) return -E64_ENOMEM;
    return fs_n++;
}

/* ---- path API ---- */

int fs64_lookup(int mnt, const char *path, fs64_node_t *out) {
    const fs64_mount_t *m = fs64_mount(mnt);
    const fs64_ops_t *o = m ? ops_of(m) : 0;
    if (!o || !out) return -E64_EINVAL;
    /* Root is a node in both filesystems; let the backend resolve it so the
     * caller never has to special-case ''. */
    if (!path || !*path || k64_streq(path, "/")) path = "/";
    u64 f = spin64_lock_irqsave(&fs_lock);
    int rc = o->lookup((fs64_mount_t *)m, path, out);
    spin64_unlock_irqrestore(&fs_lock, f);
    return rc;
}

int fs64_readdir(int mnt, const char *path, u32 index, fs64_dirent_t *out) {
    const fs64_mount_t *m = fs64_mount(mnt);
    const fs64_ops_t *o = m ? ops_of(m) : 0;
    if (!o || !out) return -E64_EINVAL;
    fs64_node_t dir;
    int rc = fs64_lookup(mnt, path, &dir);
    if (rc) return rc;
    if (!dir.is_dir) return -E64_ENOTDIR;
    u64 f = spin64_lock_irqsave(&fs_lock);
    rc = o->readdir((fs64_mount_t *)m, &dir, index, out);
    spin64_unlock_irqrestore(&fs_lock, f);
    return rc;
}

long fs64_read(int mnt, const char *path, u64 off, void *buf, u32 len) {
    const fs64_mount_t *m = fs64_mount(mnt);
    const fs64_ops_t *o = m ? ops_of(m) : 0;
    if (!o || (!buf && len)) return -E64_EINVAL;
    fs64_node_t n;
    int rc = fs64_lookup(mnt, path, &n);
    if (rc) return rc;
    if (n.is_dir) return -E64_EISDIR;
    if (off >= n.size) return 0;
    if (off + len > n.size) len = (u32)(n.size - off);
    u64 f = spin64_lock_irqsave(&fs_lock);
    long got = o->read((fs64_mount_t *)m, &n, off, buf, len);
    spin64_unlock_irqrestore(&fs_lock, f);
    return got;
}

/* Streaming hash over up to max_len bytes (0 = whole file). The host gate
 * recomputes the same FNV value from its own copy of the file. */
u64 fs64_hash(int mnt, const char *path, u64 max_len, long *rc_out) {
    long rc = 0;
    fs64_node_t n;
    u64 h = K64_FNV_OFFSET;
    rc = fs64_lookup(mnt, path, &n);
    if (rc) {
        if (rc_out) *rc_out = rc;
        return 0;
    }
    if (n.is_dir) {
        if (rc_out) *rc_out = -E64_EISDIR;
        return 0;
    }
    u64 want = max_len && max_len < n.size ? max_len : n.size;
    u8 *buf = (u8 *)kmalloc(4096);
    if (!buf) {
        if (rc_out) *rc_out = -E64_ENOMEM;
        return 0;
    }
    u64 done = 0;
    while (done < want) {
        u64 chunk = want - done > 4096 ? 4096 : want - done;
        long got = fs64_read(mnt, path, done, buf, (u32)chunk);
        if (got <= 0) {
            rc = got ? got : -E64_EIO;
            break;
        }
        h = k64_fnv1a64_update(h, buf, (u64)got);
        done += (u64)got;
    }
    kfree(buf);
    if (rc_out) *rc_out = rc;
    return h;
}

/* ---- ISO9660 backend ---------------------------------------------------- */

/* Level-1 names look like "MYOS64.BIN;1": strip the ";N" version suffix and a
 * trailing '.', and compare case-insensitively — grub-mkrescue's ISOs carry no
 * Rock Ridge here, so long names are a documented limitation rather than a
 * silent failure (lookup simply reports ENOENT). */
static void iso_norm(const char *name, int len, char *out, int outsz) {
    int n = 0;
    for (int i = 0; i < len && n < outsz - 1; i++) {
        char c = name[i];
        if (c == ';') break;
        if (c >= 'a' && c <= 'z') c -= 32;
        out[n++] = c;
    }
    while (n > 0 && (out[n - 1] == '.' || out[n - 1] == ' ')) n--;
    out[n] = '\0';
}

typedef int (*iso_cb_t)(const char *name, u32 lba, u32 size, int is_dir,
                        void *ctx);

#define ISO_BLOCK 2048

/* Walk a directory extent. Records may not cross a block boundary: a record
 * with length 0 means "skip to the next block". */
static int iso_dir_iter(fs64_mount_t *m, u32 lba, u32 dsize, iso_cb_t cb,
                        void *ctx) {
    u8 *buf = (u8 *)kmalloc(ISO_BLOCK);
    if (!buf) return -E64_ENOMEM;
    int rc = 0;
    for (u32 b = 0; b < (dsize + ISO_BLOCK - 1) / ISO_BLOCK && !rc; b++) {
        if (fs64_dev_read(m, lba + b, buf, ISO_BLOCK)) {
            rc = -E64_EIO;
            break;
        }
        u32 off = 0;
        while (off < ISO_BLOCK) {
            u8 rlen = buf[off];
            if (rlen == 0) break; /* rest of this block is padding */
            if (off + rlen > ISO_BLOCK) {
                rc = -E64_EIO;
                break;
            }
            u32 ext = (u32)buf[off + 2] | ((u32)buf[off + 3] << 8) |
                      ((u32)buf[off + 4] << 16) | ((u32)buf[off + 5] << 24);
            u32 sz = (u32)buf[off + 10] | ((u32)buf[off + 11] << 8) |
                     ((u32)buf[off + 12] << 16) | ((u32)buf[off + 13] << 24);
            int is_dir = (buf[off + 25] & 0x02) != 0;
            int nlen = buf[off + 32];
            if (33 + nlen > rlen) {
                rc = -E64_EIO;
                break;
            }
            /* Name bytes 0/1 are the "." and ".." pseudo entries: every real
             * name starts with a printable character. */
            if (nlen > 0 && buf[off + 33] > 1) {
                char name[128];
                iso_norm((const char *)&buf[off + 33], nlen, name,
                         (int)sizeof(name));
                if (name[0]) {
                    rc = cb(name, ext, sz, is_dir, ctx);
                    if (rc) break;
                }
            }
            off += rlen;
        }
    }
    kfree(buf);
    return rc;
}

typedef struct {
    const char *want;
    fs64_node_t *out;
    int found;
} iso_find_t;

static int iso_find_cb(const char *name, u32 lba, u32 size, int is_dir,
                       void *ctx) {
    iso_find_t *f = (iso_find_t *)ctx;
    if (!k64_streq_ci(name, f->want)) return 0;
    f->out->id = lba;
    f->out->size = is_dir ? 0 : size;
    f->out->dir_bytes = is_dir ? size : 0;
    f->out->is_dir = is_dir;
    f->found = 1;
    return 1; /* stop */
}

int fs64_iso_ops_lookup(fs64_mount_t *m, const char *path, fs64_node_t *out) {
    u32 lba = m->root_lba, dsize = m->root_size;
    if (k64_streq(path, "/")) {
        out->id = lba;
        out->size = 0;
        out->dir_bytes = dsize;
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
        while (*p && *p != '/') p++; /* over-long component: reject below */
        if (n >= 63) return -E64_ENOENT;
        iso_find_t f = { comp, out, 0 };
        int rc = iso_dir_iter(m, lba, dsize, iso_find_cb, &f);
        if (rc < 0) return rc;
        if (!f.found) return -E64_ENOENT;
        if (*p) { /* more components: this one must be a directory */
            if (!out->is_dir) return -E64_ENOTDIR;
            lba = out->id;
            dsize = out->dir_bytes;
        }
    }
    return 0;
}

typedef struct {
    u32 want;
    u32 seen;
    fs64_dirent_t *out;
    int done;
} iso_enum_t;

static int iso_enum_cb(const char *name, u32 lba, u32 size, int is_dir,
                       void *ctx) {
    iso_enum_t *e = (iso_enum_t *)ctx;
    if (e->seen++ < e->want) return 0;
    u64 i = 0;
    for (; name[i] && i < sizeof(e->out->name) - 1; i++)
        e->out->name[i] = name[i];
    e->out->name[i] = '\0';
    e->out->size = is_dir ? 0 : size;
    e->out->id = lba;
    e->out->is_dir = is_dir;
    e->done = 1;
    return 1;
}

int fs64_iso_ops_readdir(fs64_mount_t *m, const fs64_node_t *dir, u32 index,
                         fs64_dirent_t *out) {
    iso_enum_t e = { index, 0, out, 0 };
    int rc = iso_dir_iter(m, dir->id, dir->dir_bytes, iso_enum_cb, &e);
    if (rc < 0) return rc;
    return e.done ? 1 : 0;
}

long fs64_iso_ops_read(fs64_mount_t *m, const fs64_node_t *n, u64 off,
                       void *buf, u32 len) {
    u8 *tmp = (u8 *)kmalloc(ISO_BLOCK);
    if (!tmp) return -E64_ENOMEM;
    long done = 0;
    while (done < (long)len) {
        u64 pos = off + (u64)done;
        u32 blk = (u32)(pos / ISO_BLOCK);
        u32 in = (u32)(pos % ISO_BLOCK);
        u32 take = ISO_BLOCK - in;
        if (take > len - (u32)done) take = len - (u32)done;
        if (fs64_dev_read(m, (u64)n->id + blk, tmp, ISO_BLOCK)) {
            kfree(tmp);
            return done ? done : -E64_EIO;
        }
        k64_memcpy((u8 *)buf + done, tmp + in, take);
        done += take;
    }
    kfree(tmp);
    return done;
}

const fs64_ops_t fs64_iso_ops = {
    fs64_iso_ops_lookup,
    fs64_iso_ops_readdir,
    fs64_iso_ops_read,
};

int fs64_mount_iso(int blk) {
    const blk64_dev_t *d = blk64_dev(blk);
    if (!d || !d->atapi) return -E64_EINVAL;
    fs64_mount_t *m = fs64_new_mount();
    if (!m) return -E64_ENOMEM;
    u8 *pvd = (u8 *)kmalloc(ISO_BLOCK);
    if (!pvd) return -E64_ENOMEM;
    int rc = blk64_read(blk, 16, 1, pvd);
    if (rc || pvd[0] != 1 || !k64_memeq(&pvd[1], "CD001", 5)) {
        /* Say *what* was wrong (read error vs. wrong bytes): a mount failure
         * with no evidence is the kind of bug that costs an evening. */
        s_puts("[K64] fs: PVD at lba16 unreadable rc=");
        s_dec64((u64)(long)rc);
        s_puts(" type=");
        s_dec64((u64)pvd[0]);
        s_puts(" id=");
        s_write((const char *)&pvd[1], 5);
        s_puts("\n");
        kfree(pvd);
        return -E64_EIO;
    }
    m->type = FS64_ISO9660;
    m->blk = blk;
    m->block_size = ISO_BLOCK;
    m->root_lba = (u32)pvd[158] | ((u32)pvd[159] << 8) |
                  ((u32)pvd[160] << 16) | ((u32)pvd[161] << 24);
    m->root_size = (u32)pvd[166] | ((u32)pvd[167] << 8) |
                   ((u32)pvd[168] << 16) | ((u32)pvd[169] << 24);
    m->blocks = 0;
    for (int i = 0; i < 32; i++) m->label[i] = (char)pvd[40 + i];
    m->label[32] = '\0';
    kfree(pvd);
    int idx = fs64_commit_mount();
    if (idx < 0) return idx;
    s_puts("[K64] fs: mount");
    s_dec64((u64)idx);
    s_puts(" iso9660 blk=");
    s_dec64((u64)blk);
    s_puts(" root=");
    s_dec64(m->root_lba);
    s_puts(" block_size=");
    s_dec64(m->block_size);
    s_puts(" label=\"");
    s_puts(m->label);
    s_puts("\"\n");
    return idx;
}

/* ---- init ---- */

void fs64_init(void) {
    fs_n = 0;
    int iso = blk64_iso_slot();
    if (iso >= 0 && fs64_mount_iso(iso) < 0)
        s_puts("[K64] fs: ISO9660 mount failed (no valid PVD)\n");
    /* Every ATA device is a candidate: the M11 test disk simply has no ext2
     * magic and is skipped, a real ext2 image mounts. */
    for (int s = 0; s < 4; s++) {
        const blk64_dev_t *d = blk64_dev(s);
        if (!d || d->atapi) continue;
        if (fs64_mount_ext2(s) >= 0) break;
    }
    if (!fs_n) s_puts("[K64] fs: nothing mounted\n");
}

/* ---- selftest (pre-STI, after blk64_selftest) ---- */

static void fs_fail(const char *what) {
    s_raws("[K64] FAIL: fs ");
    s_raws(what);
    s_raws("\n");
    for (;;) __asm__ __volatile__("cli; hlt");
}

void fs64_selftest(void) {
    if (!fs_n) {
        s_puts("[K64] fs: no filesystem mounted, M12 selftest skipped\n");
        return;
    }

    /* 1. ISO9660 on the boot CD: walk the root directory, then descend two
     *    levels and check the kernel binary we booted from is really there. */
    int iso = -1, ext = -1;
    for (int i = 0; i < fs_n; i++) {
        if (fs_mounts[i].type == FS64_ISO9660) iso = i;
        if (fs_mounts[i].type == FS64_EXT2) ext = i;
    }

    if (iso >= 0) {
        fs64_dirent_t e;
        int saw_boot = 0, n = 0;
        for (u32 i = 0;; i++) {
            int rc = fs64_readdir(iso, "/", i, &e);
            if (rc == 0) break;
            if (rc < 0) fs_fail("ISO root directory unreadable");
            n++;
            if (k64_streq_ci(e.name, "BOOT") && e.is_dir) saw_boot = 1;
            if (n > 256) fs_fail("ISO root directory never ends");
        }
        if (!saw_boot) fs_fail("ISO root has no BOOT directory");

        fs64_node_t f;
        if (fs64_lookup(iso, "/boot/myos64.bin", &f))
            fs_fail("ISO /boot/myos64.bin not found");
        if (f.is_dir || f.size < 4096) fs_fail("ISO kernel image size wrong");
        u8 magic[4];
        if (fs64_read(iso, "/boot/myos64.bin", 0, magic, 4) != 4)
            fs_fail("ISO kernel image unreadable");
        if (!(magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' &&
              magic[3] == 'F'))
            fs_fail("ISO kernel image is not an ELF");

        long hrc = 0;
        u64 h = fs64_hash(iso, "/boot/myos64.bin", 0, &hrc);
        if (hrc) fs_fail("ISO kernel image hash failed");
        s_puts("[K64] fs: iso root entries=");
        s_dec64((u64)n);
        s_puts(" /boot/myos64.bin size=");
        s_dec64(f.size);
        s_puts(" fnv=");
        s_hex64(h);
        s_puts("\n");
    }

    /* 2. ext2 on the test disk: known small file, a nested path, an indirect
     *    block file (40KB in 1K blocks), and the error paths. */
    if (ext >= 0) {
        const char *hello = "hello from ext2\n";
        char buf[64];
        long got = fs64_read(ext, "/hello.txt", 0, buf, sizeof(buf));
        if (got != 16 || !k64_memeq(buf, hello, 16))
            fs_fail("ext2 /hello.txt content mismatch");

        fs64_node_t nest;
        if (fs64_lookup(ext, "/sub/nested.txt", &nest) || nest.is_dir)
            fs_fail("ext2 nested path not resolved");

        fs64_node_t big;
        if (fs64_lookup(ext, "/big.bin", &big) || big.size != 40960)
            fs_fail("ext2 /big.bin missing or wrong size");
        long hrc = 0;
        u64 h = fs64_hash(ext, "/big.bin", 0, &hrc);
        if (hrc) fs_fail("ext2 /big.bin hash failed");
        long hrc2 = 0;
        u64 h2 = fs64_hash(ext, "/sub/deep/leaf.bin", 0, &hrc2);
        if (hrc2) fs_fail("ext2 /sub/deep/leaf.bin hash failed");

        /* Edge cases: a missing path, a directory read, and past-EOF. */
        fs64_node_t nf;
        if (fs64_lookup(ext, "/nope.txt", &nf) != -E64_ENOENT)
            fs_fail("missing path did not report ENOENT");
        if (fs64_read(ext, "/sub", 0, buf, 4) != -E64_EISDIR)
            fs_fail("reading a directory did not report EISDIR");
        if (fs64_read(ext, "/hello.txt", 4096, buf, 4) != 0)
            fs_fail("read past EOF did not return 0");

        s_puts("[K64] fs: ext2 label=\"");
        s_puts(fs_mounts[ext].label);
        s_puts("\" /hello.txt=\"hello from ext2\" /big.bin size=");
        s_dec64(big.size);
        s_puts(" fnv=");
        s_hex64(h);
        s_puts(" leaf fnv=");
        s_hex64(h2);
        s_puts("\n");
    } else {
        s_puts("[K64] fs: no ext2 device attached, ext2 gate skipped\n");
    }

    s_puts("[K64] M12 FS SELFTEST OK (mount/readdir/lookup/descend/read/eof/"
            "errors exact) mounts=");
    s_dec64((u64)fs_n);
    s_puts("\n");
}
