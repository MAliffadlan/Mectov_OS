/* M11: 64-bit block layer — legacy ATA PIO, with ATAPI for the boot CD.
 *
 * Why legacy ATA first: a block layer is only worth having on a machine where
 * a disk is actually reachable. On QEMU's q35 every `-cdrom`/`-drive` hangs
 * off the ICH9 *AHCI* controller (MMIO, BAR5) and there is no legacy channel
 * at all; on `-machine pc` the same devices appear at the familiar 0x1F0 /
 * 0x170 IDE ports. run64.sh therefore runs the 64-bit kernel on pc (the
 * machine the 32-bit line has always used) and AHCI becomes a second backend
 * behind blk64_read() the day a q35 disk is needed.
 *
 * Scope: IDENTIFY + read. PIO, polling, no DMA, no IRQ14/15, no writes (nothing
 * needs to write a disk yet), no partition parsing (that is M12's job). Reads
 * land straight in the caller's buffer, 512-byte ATA sectors and 2048-byte CD
 * sectors — the driver reports which, so callers never guess.
 *
 * Lock: blk_lock serialises the controller, same reason as the 32-bit
 * ata_lock — two CPUs issuing a command sequence at once interleave port
 * writes and corrupt the controller's state machine. Leaf lock: nothing else
 * is taken inside it.
 */
#include "cpu64.h"
#include "spin64.h"

/* Register offsets from the channel base. */
#define R_DATA 0
#define R_ERR 1  /* features / error */
#define R_CNT 2
#define R_LBA0 3
#define R_LBA1 4
#define R_LBA2 5
#define R_DH 6 /* drive/head (unit + LBA high bits) */
#define R_CMD 7
#define R_ALT 0x206 /* alternate status (base + 0x206 = 0x3F6 / 0x376) */

#define ST_BSY 0x80
#define ST_DRDY 0x40
#define ST_DF 0x20
#define ST_DRQ 0x08
#define ST_ERR 0x01

#define CMD_IDENTIFY 0xEC
#define CMD_IDENTIFY_PKT 0xA1
#define CMD_READ 0x20      /* LBA28 */
#define CMD_READ_EXT 0x24  /* LBA48 */
#define CMD_PACKET 0xA0

#define BLK_SLOTS 4
#define BLK_TIMEOUT 200000

/* blk64_dev_t lives in cpu64.h: the layer above (M12) shares it. */
static blk64_dev_t blk_devs[BLK_SLOTS];
static int blk_n;
static int blk_iso, blk_disk; /* first ATAPI / first ATA slot, -1 when absent */
static spin64_t blk_lock = SPIN64_INIT;

/* ---- port I/O (per-file, like idt64.c / isr64.c) ---- */

static inline void outb(u16 port, u8 v) {
    __asm__ __volatile__("outb %0, %1" ::"a"(v), "Nd"(port));
}
static inline u8 inb(u16 port) {
    u8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw(u16 port, u16 v) {
    __asm__ __volatile__("outw %0, %1" ::"a"(v), "Nd"(port));
}
/* rep insw counts WORDS, not bytes: 512B = 256 words, 2048B = 1024 words.
 * (Getting this wrong reads half a sector and leaves the rest of the caller's
 * buffer stale — the reason these two are named constants, not literals.) */
static inline void ins_words(u16 port, void *buf, u32 words) {
    __asm__ __volatile__("rep insw"
                         : "+D"(buf), "+c"(words)
                         : "d"(port)
                         : "memory");
}
#define ATA_SECTOR_WORDS 256  /* 512 bytes */
#define CD_BLOCK_WORDS 1024   /* 2048 bytes */

/* ---- channel/unit helpers ---- */

static u16 slot_base(int s) { return (s & 2) ? 0x170 : 0x1F0; }
/* Drive/head select: 101 in bits 7-5, bit 4 = DRV (0 master, 1 slave). Same
 * value for ATA and ATAPI — the unit is chosen by the command + select, not a
 * separate "packet" bit. */
static u8 slot_dh(int s) { return (u8)(0xA0 | ((s & 1) << 4)); }

/* 400ns settle: four reads of the alternate status register. */
static void slot_settle(u16 base) {
    for (int i = 0; i < 4; i++) (void)inb(base + R_ALT);
}

/* Wait until BSY clears. 0 ok, -1 timeout, -2 device error. */
static int wait_ready(u16 base) {
    for (int i = 0; i < BLK_TIMEOUT; i++) {
        u8 st = inb(base + R_ALT);
        if (!(st & ST_BSY)) return (st & (ST_ERR | ST_DF)) ? -2 : 0;
    }
    return -1;
}

/* Wait for a data request. 0 ok, -1 timeout, -2 error. */
static int wait_drq(u16 base) {
    for (int i = 0; i < BLK_TIMEOUT; i++) {
        u8 st = inb(base + R_ALT);
        if (st & (ST_ERR | ST_DF)) return -2;
        if (!(st & ST_BSY) && (st & ST_DRQ)) return 0;
    }
    return -1;
}

/* ---- IDENTIFY (device or packet device), parses model/serial/capacity ---- */

static int ata_identify(int s, u16 *id, u8 cmd) {
    u16 base = slot_base(s);
    outb(base + R_DH, slot_dh(s));
    slot_settle(base);
    outb(base + R_CNT, 0);
    outb(base + R_LBA0, 0);
    outb(base + R_LBA1, 0);
    outb(base + R_LBA2, 0);
    outb(base + R_CMD, cmd);
    /* Absent unit: the status port floats at 0x00/0xFF — neither BSY nor DRQ
     * ever comes up, so fail fast instead of burning the DRQ timeout. */
    u8 st = inb(base + R_ALT);
    if (st == 0x00 || st == 0xFF) return -1;
    if (wait_drq(base)) return -2;
    ins_words(base + R_DATA, id, 256); /* IDENTIFY: 256 words = 512 bytes */
    return 0;
}

/* ATA IDENTIFY strings are word-swapped: high byte first. */
static void id_string(const u16 *id, int first_word, int words, char *out) {
    int n = 0;
    for (int w = 0; w < words; w++) {
        u16 v = id[first_word + w];
        out[n++] = (char)(v >> 8);
        out[n++] = (char)(v & 0xFF);
    }
    out[n] = '\0';
    for (int i = n - 1; i >= 0 && out[i] == ' '; i--) out[i] = '\0';
}

/* ---- reads ---- */

/* One ATA sector (512B) at `lba`, LBA28 (0x20) or LBA48 (0x24). */
static int ata_read_sector(int s, u64 lba, void *buf, int use48) {
    u16 base = slot_base(s);
    if (wait_ready(base)) return -2;

    if (use48) {
        u8 cnt_hi = 0, cnt_lo = 1;
        outb(base + R_CNT, cnt_hi);
        outb(base + R_LBA0, (u8)(lba >> 24));
        outb(base + R_LBA1, (u8)(lba >> 32));
        outb(base + R_LBA2, (u8)(lba >> 40));
        outb(base + R_CNT, cnt_lo);
        outb(base + R_LBA0, (u8)lba);
        outb(base + R_LBA1, (u8)(lba >> 8));
        outb(base + R_LBA2, (u8)(lba >> 16));
        outb(base + R_DH, (u8)(0x40 | slot_dh(s))); /* LBA48 select */
        outb(base + R_CMD, CMD_READ_EXT);
    } else {
        outb(base + R_CNT, 1);
        outb(base + R_LBA0, (u8)lba);
        outb(base + R_LBA1, (u8)(lba >> 8));
        outb(base + R_LBA2, (u8)(lba >> 16));
        outb(base + R_DH,
             (u8)(0xE0 | ((s & 1) << 4) | ((lba >> 24) & 0x0F)));
        outb(base + R_CMD, CMD_READ);
    }
    if (wait_drq(base)) return -3;
    ins_words(base + R_DATA, buf, ATA_SECTOR_WORDS);
    return 0;
}

/* The device is in a phase we did not expect. Dump the three registers that
 * describe it — interrupt reason, error and status — because "ATAPI failed"
 * with no evidence means re-deriving the transfer state machine by hand. */
static void atapi_stuck(const char *where, u16 base) {
    s_puts("[K64] blk: ATAPI stuck in ");
    s_puts(where);
    s_puts(" phase ir=");
    s_hex32(inb(base + R_CNT));
    s_puts(" err=");
    s_hex32(inb(base + R_ERR));
    s_puts(" st=");
    s_hex32(inb(base + R_ALT));
    s_puts("\n");
}

/* One 2048-byte CD block via ATAPI READ(12) (packet command 0xA0). */
static int atapi_read_block(int s, u32 lba, void *buf) {
    u16 base = slot_base(s);
    if (wait_ready(base)) return -2;
    outb(base + R_DH, slot_dh(s));
    slot_settle(base);
    /* Packet interface: features (DMA off), then the byte count limit for the
     * transfer we are about to ask for, then PACKET, then the 12-byte CDB.
     * The limit must be the real size: zero does not mean "unlimited" on all
     * implementations (QEMU caps the data phase with it). */
    outb(base + R_ERR, 0);  /* features */
    outb(base + R_CNT, 0);  /* no DMA */
    outb(base + R_LBA0, (u8)(2048 & 0xFF));  /* byte count limit low */
    outb(base + R_LBA1, (u8)(2048 >> 8));    /* byte count limit high */
    outb(base + R_CMD, CMD_PACKET);
    slot_settle(base);
    if (wait_drq(base)) return -3;
    /* Phase check #1: right after PACKET the interrupt reason must say
     * "command phase, host -> device" (0x01) — the device is waiting for the
     * 12-byte CDB. Any other value means the transfer state machine is not
     * where we think it is (0x03 would be a REQUEST SENSE waiting for us). */
    if ((inb(base + R_CNT) & 0x03) != 0x01) {
        atapi_stuck("packet", base);
        return -6;
    }

    /* READ(12): [opcode, flags, LBA(4, big endian), blocks(4), control].
     * The transfer length is a 4-byte big-endian FIELD at CDB[6..9], not a
     * single byte: this first went out with the count in CDB[8], which asks
     * for 0x00000100 = 256 blocks. Byte 0 still came back correct, so the M11
     * single-command hash gate passed while 255 blocks sat pending — M12's
     * second packet command is what exposed it (the device was never idle). */
    u8 cdb[12];
    for (int i = 0; i < 12; i++) cdb[i] = 0;
    cdb[0] = 0xA8;
    cdb[2] = (u8)(lba >> 24);
    cdb[3] = (u8)(lba >> 16);
    cdb[4] = (u8)(lba >> 8);
    cdb[5] = (u8)lba;
    cdb[6] = 0;
    cdb[7] = 0;
    cdb[8] = 0;
    cdb[9] = 1; /* transfer length, low byte: one 2048-byte block */
    for (int i = 0; i < 12; i += 2)
        outw(base + R_DATA, (u16)(cdb[i] | (cdb[i + 1] << 8)));

    if (wait_drq(base)) return -4;
    /* Phase check #2: now it must be "data, device -> host" (0x02) before the
     * data port carries the payload. */
    if ((inb(base + R_CNT) & 0x03) != 0x02) {
        atapi_stuck("cdb", base);
        return -7;
    }
    ins_words(base + R_DATA, buf, CD_BLOCK_WORDS);
    /* Let the device finish the packet before the next command. */
    if (wait_ready(base)) return -5;
    /* The packet must be *complete* now. If DRQ is still up the device is
     * sitting in a data phase we did not consume (wrong transfer length, short
     * read) — the bytes in `buf` would still look right, and the failure would
     * only surface on the next command, so refuse it here instead. */
    if (inb(base + R_ALT) & ST_DRQ) {
        atapi_stuck("post-data", base);
        return -8;
    }
    return 0;
}

/* ---- public API ---- */

/* path: 0 = auto (LBA48 only when the LBA needs it), 1 = force LBA28,
 * 2 = force LBA48. */
int blk64_read_common(int slot, u64 lba, u32 count, void *buf, int path);

int blk64_count(void) { return blk_n; }

const blk64_dev_t *blk64_dev(int slot) {
    if (slot < 0 || slot >= BLK_SLOTS || !blk_devs[slot].present) return 0;
    return &blk_devs[slot];
}

int blk64_iso_slot(void) { return blk_iso; }
int blk64_disk_slot(void) { return blk_disk; }

int blk64_read(int slot, u64 lba, u32 count, void *buf) {
    return blk64_read_common(slot, lba, count, buf, 0);
}

/* The gate reads one sector through the 28-bit and the 48-bit command and
 * requires identical bytes, so neither path can rot unnoticed. */
int blk64_read28(int slot, u64 lba, u32 count, void *buf) {
    return blk64_read_common(slot, lba, count, buf, 1);
}

int blk64_read48(int slot, u64 lba, u32 count, void *buf) {
    return blk64_read_common(slot, lba, count, buf, 2);
}

int blk64_read_common(int slot, u64 lba, u32 count, void *buf, int path) {
    if (slot < 0 || slot >= BLK_SLOTS) return -1;
    blk64_dev_t *d = &blk_devs[slot];
    if (!d->present || !count) return -1;
    /* A CD's size comes from its filesystem, not from IDENTIFY, so only a
     * plain ATA disk gets a capacity check here. */
    if (!d->atapi && lba + count > d->sectors) return -1;

    u64 f = spin64_lock_irqsave(&blk_lock);
    int rc = 0;
    if (d->atapi) {
        if (d->sector_size != 2048) {
            rc = -2; /* no other block size is reachable through READ(12) here */
        } else {
            if (lba + count > 0xFFFFFFFFULL) {
                rc = -1;
            } else {
                for (u32 i = 0; i < count && !rc; i++)
                    rc = atapi_read_block(slot, (u32)(lba + i),
                                          (u8 *)buf + (u64)i * 2048);
            }
        }
    } else {
        int use48 = path == 2 ? d->lba48
                              : (path == 0 && d->lba48 &&
                                 (lba + count > 0x0FFFFFFFULL));
        if (path == 2 && !d->lba48) rc = -1; /* asked for LBA48, drive has none */
        else if (!use48 && (lba + count > 0x0FFFFFFFULL)) {
            rc = -1; /* beyond LBA28 and no LBA48 support */
        } else {
            for (u32 i = 0; i < count && !rc; i++)
                rc = ata_read_sector(slot, lba + i,
                                     (u8 *)buf + (u64)i * 512, use48);
        }
    }
    spin64_unlock_irqrestore(&blk_lock, f);
    return rc;
}

/* ---- probe ---- */

void blk64_init(void) {
    u16 id[256];
    blk_n = 0;
    blk_iso = blk_disk = -1;
    for (int s = 0; s < BLK_SLOTS; s++) blk_devs[s].present = 0;
    for (int s = 0; s < BLK_SLOTS; s++) {
        blk64_dev_t *d = &blk_devs[s];
        u64 f = spin64_lock_irqsave(&blk_lock);
        /* Packet first: an ATAPI device answers IDENTIFY DEVICE with a
         * mismatch that leaves the channel dirty on some parts, whereas
         * IDENTIFY PACKET DEVICE on a plain ATA drive just aborts cleanly. */
        int atapi = ata_identify(s, id, CMD_IDENTIFY_PKT) == 0;
        int rc = atapi ? 0 : ata_identify(s, id, CMD_IDENTIFY);
        spin64_unlock_irqrestore(&blk_lock, f);
        if (rc) continue;

        d->present = 1;
        d->atapi = atapi;
        d->sector_size = atapi ? 2048 : 512;
        if (atapi) {
            d->lba48 = 0;
            d->sectors = 0; /* the CD's size comes from the filesystem */
        } else {
            d->lba48 = (id[83] & (1 << 10)) != 0;
            d->sectors = ((u64)id[103] << 48) | ((u64)id[102] << 32) |
                         ((u64)id[101] << 16) | (u64)id[100];
            if (!d->lba48 || !d->sectors)
                d->sectors = ((u64)id[61] << 16) | (u64)id[60];
        }
        id_string(id, 27, 20, d->model);
        id_string(id, 10, 10, d->serial);
        blk_n++;
        if (atapi && blk_iso < 0) blk_iso = s;
        if (!atapi && blk_disk < 0) blk_disk = s;

        s_puts("[K64] blk: ");
        s_puts((s & 2) ? "ide1" : "ide0");
        s_puts((s & 1) ? " slave " : " master ");
        s_puts(atapi ? "ATAPI " : "ATA   ");
        s_puts("model=\"");
        s_puts(d->model);
        s_puts("\" sector=");
        s_dec64(d->sector_size);
        if (atapi) {
            s_puts(" (cd)\n");
        } else {
            s_puts(" sectors=");
            s_dec64(d->sectors);
            s_puts(" lba48=");
            s_putc(d->lba48 ? '1' : '0');
            s_puts("\n");
        }
    }
    if (!blk_n) s_puts("[K64] blk: no ATA/ATAPI device found\n");
}

/* ---- selftest (pre-STI, after heap64_selftest: it reads into heap buffers) --- */

#define FNV_OFFSET 0xCBF29CE484222325ULL
#define FNV_PRIME 0x100000001B3ULL

/* Test-only hash: mirrors scripts/mk_blkdisk.py so the host can check that the
 * bytes the guest read are byte-identical to the file it handed QEMU. */
static u64 fnv1a64(const u8 *p, u64 len) {
    u64 h = FNV_OFFSET;
    for (u64 i = 0; i < len; i++) h = (h ^ p[i]) * FNV_PRIME;
    return h;
}

static void blk_fail(const char *what) {
    s_raws("[K64] FAIL: blk ");
    s_raws(what);
    s_raws("\n");
    for (;;) __asm__ __volatile__("cli; hlt");
}

static int blk_same(const u8 *a, const u8 *b, u64 n) {
    for (u64 i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

void blk64_selftest(void) {
    if (!blk_n) {
        s_puts("[K64] blk: no device attached, M11 selftest skipped "
               "(run64.sh attaches blkdisk.img + the ISO)\n");
        return;
    }

    /* 1. The boot CD: read the ISO9660 Primary Volume Descriptor with ATAPI
     *    READ(12) and check it structurally — that is a filesystem the kernel
     *    itself can recognise, not just bytes that came back. */
    int iso_ok = 0;
    u64 iso_hash = 0;
    if (blk_iso >= 0) {
        u8 *pvd = (u8 *)kmalloc(2048);
        if (!pvd) blk_fail("no buffer for the ISO PVD");
        int rc = blk64_read(blk_iso, 16, 1, pvd);
        /* On failure, say exactly what came back: rc, the first 8 bytes and
         * the ATAPI interrupt reason/error pair, so a future regression is
         * one boot away from a diagnosis instead of a guess. */
        if (rc || pvd[0] != 1) {
            u16 base = slot_base(blk_iso);
            s_puts("[K64] blk: iso PVD read rc=");
            s_dec64((u64)(long)rc);
            s_puts(" ir=");
            s_hex32(inb(base + R_CNT));
            s_puts(" err=");
            s_hex32(inb(base + R_ERR));
            s_puts(" first=");
            for (int i = 0; i < 8; i++) {
                s_hex32(pvd[i]);
                s_putc(' ');
            }
            s_puts("\n");
        }
        if (!rc && pvd[0] == 1 &&
            pvd[1] == 'C' && pvd[2] == 'D' && pvd[3] == '0' && pvd[4] == '0' &&
            pvd[5] == '1') {
            /* Read it *again* on a fresh packet command and compare: the disk
             * path has had a re-read check since M11, the CD did not — and a
             * packet interface that only survives one command looks fine until
             * something above it issues a second one. */
            u8 *pvd2 = (u8 *)kmalloc(2048);
            int rc2 = pvd2 ? blk64_read(blk_iso, 16, 1, pvd2) : -1;
            s_puts("[K64] blk: iso reread rc=");
            s_dec64((u64)(long)rc2);
            s_puts(" stable=");
            s_putc((rc2 == 0 && blk_same(pvd, pvd2, 2048)) ? '1' : '0');
            s_puts("\n");
            int stable = rc2 == 0 && blk_same(pvd, pvd2, 2048);
            kfree(pvd2);
            iso_hash = fnv1a64(pvd, 2048);
            iso_ok = stable;
            char vol[33];
            int n = 0;
            for (int i = 0; i < 32; i++) {
                char c = (char)pvd[40 + i];
                vol[n++] = c;
            }
            vol[32] = '\0';
            for (int i = 31; i >= 0 && (vol[i] == ' ' || vol[i] == '\0'); i--)
                vol[i] = '\0';
            s_puts("[K64] blk: iso PVD lba16 type=1 id=CD001 vol=\"");
            s_puts(vol);
            s_puts("\" fnv=");
            s_hex64(iso_hash);
            s_puts("\n");
        }
        kfree(pvd);
    }
    if (!iso_ok) blk_fail("could not read a valid ISO9660 PVD from the CD");

    /* 2. The ATA disk: same sector through LBA28 and LBA48 must be identical,
     *    and the magic in sector 0 tells us it is *our* test disk. */
    if (blk_disk >= 0) {
        u8 *s0 = (u8 *)kmalloc(512);
        u8 *a28 = (u8 *)kmalloc(512);
        u8 *a48 = (u8 *)kmalloc(512);
        u8 *again = (u8 *)kmalloc(512);
        if (!s0 || !a28 || !a48 || !again) blk_fail("no buffer for the disk reads");

        if (blk64_read(blk_disk, 0, 1, s0)) blk_fail("disk sector 0 read failed");
        const char *magic = "MECTOV-BLK-64";
        if (!blk_same(s0, (const u8 *)magic, 13)) blk_fail("blkdisk magic missing");

        if (blk64_read28(blk_disk, 40, 1, a28)) blk_fail("LBA28 read failed");
        if (blk64_read48(blk_disk, 40, 1, a48)) blk_fail("LBA48 read failed");
        if (blk64_read28(blk_disk, 40, 1, again)) blk_fail("second read failed");

        /* Print the evidence before judging it: if the two paths ever disagree
         * again, the log already says where and how. */
        int same = blk_same(a28, a48, 512);
        int stable = blk_same(a28, again, 512);
        u64 h28 = fnv1a64(a28, 512);
        s_puts("[K64] blk: disk sector0 magic=ok sectors=");
        s_dec64(blk_devs[blk_disk].sectors);
        s_puts(" lba28 fnv=" );
        s_hex64(h28);
        s_puts(" lba48 fnv=");
        s_hex64(fnv1a64(a48, 512));
        s_puts(" same=");
        s_putc(same ? '1' : '0');
        s_puts(" stable=");
        s_putc(stable ? '1' : '0');
        if (!same || !stable) {
            int at = 0;
            while (at < 512 && a28[at] == a48[at]) at++;
            s_puts(" firstdiff=");
            s_dec64((u64)at);
            s_puts(" b28=");
            s_hex32(a28[at]);
            s_puts(" b48=");
            s_hex32(a48[at]);
        }
        s_puts("\n");
        if (!same) blk_fail("LBA28 and LBA48 disagree");
        if (!stable) blk_fail("same sector read twice differs");
        kfree(s0);
        kfree(a28);
        kfree(a48);
        kfree(again);
    } else {
        s_puts("[K64] blk: no ATA disk attached, LBA28/48 gate skipped\n");
    }

    s_puts("[K64] M11 BLK SELFTEST OK (identify/iso-pvd/lba28/lba48/reread "
            "exact) devices=");
    s_dec64((u64)blk_n);
    s_puts("\n");
}
