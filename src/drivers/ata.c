#include "../include/ata.h"
#include "../include/io.h"
#include "../include/spinlock.h"
#include "../include/pci.h"
#include "../include/mem.h"
#include "../include/serial.h"
#include "../include/utils.h"   // memcpy for the DMA bounce path

// ata_lock serializes the shared IDE controller: two cores issuing command
// sequences concurrently would interleave port writes and corrupt the
// controller state machine. Process context (VFS, ext2, loader) uses irqsave;
// nothing calls ATA from IRQ context. Ordering: fd/vfs > ata_lock.
static spinlock_t ata_lock = SPINLOCK_INIT;
static uint32_t ata_eflags;

// Channel selection: drives 0-1 live on the primary channel (ports 0x1F0),
// drives 2-3 on the secondary (ports 0x170). The master/slave bit comes from
// the drive's parity (0/2 = master, 1/3 = slave).
static uint16_t ata_base_port(int drive) {
    return (drive & 2) ? 0x170 : 0x1F0;
}

int ata_wait_bsy_drive(int drive) { 
    uint16_t base = ata_base_port(drive);
    int timeout = 100000;
    while((inb(base + 7) & 0x80) && --timeout > 0);
    return timeout > 0 ? 0 : -1;
}
int ata_wait_drq_drive(int drive) { 
    uint16_t base = ata_base_port(drive);
    int timeout = 100000;
    while(!(inb(base + 7) & 0x08) && --timeout > 0) {
        // Check for error
        if (inb(base + 7) & 0x01) return -1;
    }
    return timeout > 0 ? 0 : -1;
}
int ata_wait_bsy() { return ata_wait_bsy_drive(0); }
int ata_wait_drq() { return ata_wait_drq_drive(0); }

volatile int hdd_activity = 0;

int ata_read_sector_drive(int drive, unsigned int lba, unsigned char* b) {
    uint16_t base = ata_base_port(drive);
    ata_eflags = spin_lock_irqsave(&ata_lock);
    hdd_activity = 10;
    if (ata_wait_bsy_drive(drive) < 0) { spin_unlock_irqrestore(&ata_lock, ata_eflags); return -1; } 
    outb(base + 6, ((drive & 1) ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F)); 
    outb(base + 2, 1); 
    outb(base + 3, (unsigned char)lba);
    outb(base + 4, (unsigned char)(lba >> 8)); 
    outb(base + 5, (unsigned char)(lba >> 16)); 
    outb(base + 7, 0x20);
    if (ata_wait_bsy_drive(drive) < 0) { spin_unlock_irqrestore(&ata_lock, ata_eflags); return -1; } 
    if (ata_wait_drq_drive(drive) < 0) { spin_unlock_irqrestore(&ata_lock, ata_eflags); return -1; }
    // One `rep insw` moves the whole 512-byte sector — under KVM that is a
    // SINGLE VM exit for the entire data phase instead of 256 (v38.25).
    unsigned char* p = b;
    int wc = 256;
    __asm__ volatile("cld; rep insw" : "+D"(p), "+c"(wc) : "d"(base) : "memory");
    spin_unlock_irqrestore(&ata_lock, ata_eflags);
    return 0;
}

// Multi-sector PIO read (v38.25). One command transfers up to ATA_BATCH_MAX
// contiguous sectors; the drive asserts DRQ once per sector, so the per-
// sector overhead (command setup + BSY latency) is paid once for the whole
// batch instead of once per sector. Sector count is clamped to the 128-
// sector LBA boundary per the ATA spec (a multi-sector transfer may not
// cross it) — QEMU tolerates crossing, real drives may not.
int ata_read_sectors_drive(int drive, unsigned int lba, int count, unsigned char* b) {
    uint16_t base = ata_base_port(drive);
    if (count < 1) return -1;
    count = ata_batch_limit(lba, count);
    // DMA fast path (v38.26): the BMIDE controller moves the data once per
    // command with zero per-sector port I/O. Falls back to PIO below on any
    // failure (controller absent, bounce alloc failed, transfer error).
    if (ata_dma_read_sectors_drive(drive, lba, count, b) == 0) return 0;

    ata_eflags = spin_lock_irqsave(&ata_lock);
    hdd_activity = 10;
    if (ata_wait_bsy_drive(drive) < 0) { spin_unlock_irqrestore(&ata_lock, ata_eflags); return -1; }
    outb(base + 6, ((drive & 1) ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    outb(base + 2, (unsigned char)count);
    outb(base + 3, (unsigned char)lba);
    outb(base + 4, (unsigned char)(lba >> 8));
    outb(base + 5, (unsigned char)(lba >> 16));
    outb(base + 7, 0x20);   // READ SECTORS (LBA28)
    if (ata_wait_bsy_drive(drive) < 0) { spin_unlock_irqrestore(&ata_lock, ata_eflags); return -1; }
    for (int s = 0; s < count; s++) {
        if (ata_wait_drq_drive(drive) < 0) { spin_unlock_irqrestore(&ata_lock, ata_eflags); return -1; }
        // One `rep insw` per sector: a single VM exit for the whole data
        // phase under KVM instead of 256 word-by-word exits (v38.25).
        unsigned char* p = b + s * 512;
        int wc = 256;
        __asm__ volatile("cld; rep insw" : "+D"(p), "+c"(wc) : "d"(base) : "memory");
    }
    // Drain any residual BSY before the caller issues the next command.
    ata_wait_bsy_drive(drive);
    spin_unlock_irqrestore(&ata_lock, ata_eflags);
    return 0;
}

// Multi-sector PIO write — the mirror of ata_read_sectors_drive: one
// command, DRQ pulses per sector, data written in count*512-byte chunks.
int ata_write_sectors_drive(int drive, unsigned int lba, int count, const unsigned char* b) {
    uint16_t base = ata_base_port(drive);
    if (count < 1) return -1;
    count = ata_batch_limit(lba, count);
    // DMA fast path (v38.26) — same contract as the read side above.
    if (ata_dma_write_sectors_drive(drive, lba, count, b) == 0) return 0;

    ata_eflags = spin_lock_irqsave(&ata_lock);
    hdd_activity = 10;
    if (ata_wait_bsy_drive(drive) < 0) { spin_unlock_irqrestore(&ata_lock, ata_eflags); return -1; }
    outb(base + 6, ((drive & 1) ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    outb(base + 2, (unsigned char)count);
    outb(base + 3, (unsigned char)lba);
    outb(base + 4, (unsigned char)(lba >> 8));
    outb(base + 5, (unsigned char)(lba >> 16));
    outb(base + 7, 0x30);   // WRITE SECTORS (LBA28)
    if (ata_wait_bsy_drive(drive) < 0) { spin_unlock_irqrestore(&ata_lock, ata_eflags); return -1; }
    for (int s = 0; s < count; s++) {
        if (ata_wait_drq_drive(drive) < 0) { spin_unlock_irqrestore(&ata_lock, ata_eflags); return -1; }
        // One `rep outsw` per sector (v38.25): single VM exit for the data
        // phase under KVM instead of 256 word-by-word exits.
        const unsigned char* p = b + s * 512;
        int wc = 256;
        __asm__ volatile("cld; rep outsw" : "+S"(p), "+c"(wc) : "d"(base) : "memory");
    }
    outb(base + 7, 0xE7);   // CACHE FLUSH
    ata_wait_bsy_drive(drive);
    spin_unlock_irqrestore(&ata_lock, ata_eflags);
    return 0;
}

// ============================================================================
// Bus-mastering DMA (v38.26)
// ============================================================================
// The PIIX3/PIIX4 IDE controllers QEMU models expose a bus-master (BMIDE)
// register block via PCI BAR4. Instead of the CPU moving each sector through
// the data port (even with `rep insw`, that is one VM exit per sector under
// KVM), the controller walks a Physical Region Descriptor table (PRDT) and
// moves the data itself; the CPU only programs the command block, the PRDT
// address and the BMIDE command register, then waits for completion.
//
// PRD entry (8 bytes, per the PCI IDE spec — QEMU hw/ide/pci.c):
//   [0..3]  physical address (4-byte aligned)
//   [4..7]  low 16 bits: byte count of this region; 0 means 0x10000 (64K).
//           A region must not cross a 64K boundary.
//           bit 31: EOT (end of table)
//
// BMIDE registers (I/O ports, relative to BAR4):
//   +0 command (bit0 = start/stop — QEMU's PIIX starts DMA on bit 0;
//              bit2 = direction on real hardware: 0 disk->mem read,
//              1 mem->disk write; bit3 = interrupt enable)
//   +2 status  (bit0 = active, bit1 = error, bit2 = interrupt,
//              bit5 = host error, bit6 = device error)
//   +4 PRDT physical address (dword)
//   +8 byte count (word; informational — the transfer length comes from the
//      command block's sector count register, which we set to `count`)
//
// Locking: the whole transfer runs under ata_lock with IRQs disabled (the
// driver's existing model — nothing calls ATA from IRQ context, and a timer
// preempting us mid-transfer would deadlock on the held spinlock). So the
// completion wait POLLS the BMIDE status register instead of blocking on the
// IRQ. The IRQ14/15 lines are still fully wired (unmasked in the PIC, routed
// through the IOAPIC, handlers registered): they acknowledge + clear the
// latched controller interrupt so real hardware never keeps re-asserting, and
// the flag they set is a redundant completion signal. DMA is a pure win over
// PIO on the transfer itself — the wait is a few status reads either way.

#define ATA_DMA_BATCH_MAX 128          // 128 sectors = 64K = one PRD region max
#define BMIDE_CMD   0
#define BMIDE_STATUS 2
#define BMIDE_PRDT   4
#define BMIDE_BCNT   8

// A static PRDT (physically contiguous .bss, 64K-aligned so the table itself
// never straddles a 64K boundary). Two entries cover a worst-case 64K bounce
// buffer split at a 64K boundary.
typedef struct {
    uint32_t phys_addr;
    uint32_t size;   // low 16 bits = byte count (0 = 64K); bit 31 = EOT
} __attribute__((packed)) prd_entry_t;

static prd_entry_t ata_prdt[2] __attribute__((aligned(65536)));
static uint16_t bmide_base = 0;    // I/O base of the BMIDE block (BAR4)
static volatile int dma_irq_seen = 0;   // set by the IRQ14/15 handler

int ata_dma_ready(void) {
    return bmide_base != 0;
}

void ata_dma_irq_primary(registers_t* r) {
    (void)r;
    dma_irq_seen = 1;
    // Clear the latched controller interrupt (write-back of the status
    // register) so the level-triggered line de-asserts.
    if (bmide_base) outb(bmide_base + BMIDE_STATUS, 0x04);
}
void ata_dma_irq_secondary(registers_t* r) {
    (void)r;
    dma_irq_seen = 1;
    // Secondary channel sits at BAR4 + 8.
    if (bmide_base) outb(bmide_base + 8 + BMIDE_STATUS, 0x04);
}

// Find the PCI IDE controller and set up bus-mastering DMA. Must be called
// after pci_scan() (kernel_main). Safe to call even if no IDE controller is
// present — the driver then stays on the PIO path.
void ata_dma_init(void) {
    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t* d = &pci_devices[i];
        // Mass-storage controller (class 01), IDE (subclass 01) or ATA
        // (subclass 05), with a bus-master capable programming interface.
        if (d->class_code != 0x01) continue;
        if (d->subclass != 0x01 && d->subclass != 0x05) continue;
        if (!(d->prog_if & 0x80)) continue;   // bit 7 = bus mastering
        uint32_t bar4 = pci_read(d->bus, d->slot, d->func, 0x20);
        if (!(bar4 & 1)) continue;            // must be I/O space
        bmide_base = (uint16_t)(bar4 & ~3);
        // Enable bus mastering in the PCI command register (bit 2).
        uint32_t cmd = pci_read(d->bus, d->slot, d->func, 0x04);
        pci_write(d->bus, d->slot, d->func, 0x04, cmd | (1 << 2));
        write_serial_string("[ATA] BMIDE DMA ready, BAR4=");
        write_serial_hex(bmide_base);
        write_serial_string("\n");
        return;
    }
}

// Fill ata_prdt for `count*512` contiguous bytes at kernel address `buf`
// (the bounce buffer — identity-mapped, so its address IS its physical
// address). Splits at 64K boundaries; sets EOT on the last entry.
static int ata_build_prdt(unsigned char* buf, int count) {
    uint32_t phys = (uint32_t)(uintptr_t)buf;
    uint32_t remaining = (uint32_t)count * 512;
    int n = 0;
    while (remaining > 0 && n < 2) {
        uint32_t region = 0x10000 - (phys & 0xFFFF);
        if (region > remaining) region = remaining;
        ata_prdt[n].phys_addr = phys;
        ata_prdt[n].size = (region == 0x10000) ? 0 : region;
        phys += region;
        remaining -= region;
        n++;
    }
    if (remaining > 0) return -1;
    ata_prdt[n - 1].size |= 0x80000000;  // EOT
    return 0;
}

// One DMA transfer of `count` contiguous sectors (count <= 128). `buf` is a
// KERNEL bounce buffer (identity-mapped, contiguous). Returns 0 on success.
// On failure returns -1 and leaves the controller in a known state (status
// cleared) so the caller can retry with PIO.
static int ata_dma_transfer(int drive, unsigned int lba, int count,
                            unsigned char* bounce, int is_write) {
    uint16_t base = ata_base_port(drive);
    uint16_t bm = bmide_base + ((drive & 2) ? 8 : 0);
    if (count < 1 || count > ATA_DMA_BATCH_MAX) return -1;
    if (ata_build_prdt(bounce, count) < 0) return -1;

    dma_irq_seen = 0;
    // Clear any stale INT/ERR latch from an earlier transfer (QEMU's status
    // writeback: val bit 1/2 clear those bits; bits 5/6 are SET by the write
    // value, so write 0x06, never 0xFF).
    outb(bm + BMIDE_STATUS, 0x06);
    // 1. Issue the DMA command in the command block (sector count = count).
    if (ata_wait_bsy_drive(drive) < 0) return -1;
    outb(base + 6, ((drive & 1) ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    outb(base + 2, (unsigned char)count);
    outb(base + 3, (unsigned char)lba);
    outb(base + 4, (unsigned char)(lba >> 8));
    outb(base + 5, (unsigned char)(lba >> 16));
    outb(base + 7, is_write ? 0xCA : 0xC8);   // WRITE DMA / READ DMA
    if (ata_wait_bsy_drive(drive) < 0) return -1;

    // 2. Program the PRDT, then start the controller. QEMU's PIIX starts
    //    DMA on command bit 0 (BM_CMD_START) and only reacts to an EDGE of
    //    that bit (it compares against the value latched in bm->cmd from the
    //    previous write), so we must first write start=0, then start=1. The
    //    transfer direction comes from the ATA command opcode (0xC8/0xCA),
    //    not this register. Bit 2 is the direction for real hardware, bit 3
    //    is the interrupt enable.
    outl(bm + BMIDE_PRDT, (uint32_t)(uintptr_t)ata_prdt);
    outw(bm + BMIDE_BCNT, (uint16_t)count);
    outb(bm + BMIDE_CMD, 0x08 | (is_write ? 0x04 : 0x00));       // stop
    outb(bm + BMIDE_CMD, 0x08 | 0x01 | (is_write ? 0x04 : 0x00)); // start

    // 3. Wait for completion. QEMU can finish the whole transfer inside one
    //    VM exit (between two of our port reads), so the active bit (0x01)
    //    may be set AND cleared before the first status poll — it must not be
    //    the only completion signal. The latched INTERRUPT bit (0x02) is the
    //    authoritative completion flag: QEMU sets it via bmdma_irq() when the
    //    drive raises its IRQ, and we cleared any stale latch above. Error
    //    bits are 1 (error), 5 (host error), 6 (device error).
    int timeout = 2000000;
    uint8_t st = 0;
    int started = 0;
    while (--timeout > 0) {
        st = inb(bm + BMIDE_STATUS);
        if (st & 0x62) break;                // real error bits
        if (st & 0x04) break;                // INT latched -> completed
        if (st & 0x01) started = 1;
        else if (started) break;             // active seen then cleared
    }
    if (timeout <= 0 || (st & 0x62)) {
        // Clear the latched INT/ERR bits. QEMU's status writeback is
        // `(val & 0x60) | ... | (~val & (ERR|INT))`, so write 0x06 (clear
        // bits 1+2) — never 0xFF, which would SET bits 5/6 (host/dev err).
        outb(bm + BMIDE_STATUS, 0x06);
        return -1;
    }
    // 4. Clear the latched interrupt; wait for the drive to finish BSY.
    outb(bm + BMIDE_STATUS, 0x04);
    if (is_write) {
        outb(base + 7, 0xE7);                // CACHE FLUSH after WRITE DMA
        ata_wait_bsy_drive(drive);
    }
    ata_wait_bsy_drive(drive);
    return 0;
}

// Public DMA entry points. The bounce buffer is a preallocated 64K-aligned
// static buffer (identity-mapped, so its address IS its physical address and
// a <=64K transfer never crosses a 64K boundary — the PRDT is always a
// single region). v38.47: this replaces a per-I/O kmalloc/kfree plus a
// byte-at-a-time copy loop of up to 64K; the copy itself stays under
// ata_lock because the buffer is now shared by all CPUs.
// `buf` is the caller's buffer (any address). Returns 0 on success, -2 when
// DMA is unavailable (caller keeps PIO), -1 on transfer failure (caller may
// fall back to PIO for the same range).
static unsigned char dma_bounce[65536] __attribute__((aligned(65536)));

int ata_dma_read_sectors_drive(int drive, unsigned int lba, int count, unsigned char* buf) {
    if (!ata_dma_ready() || count < 1 || count > ATA_DMA_BATCH_MAX) return -2;

    ata_eflags = spin_lock_irqsave(&ata_lock);
    hdd_activity = 10;
    int rc = ata_dma_transfer(drive, lba, count, dma_bounce, 0);
    if (rc == 0) {
        memcpy(buf, dma_bounce, (uint32_t)count * 512);
    }
    spin_unlock_irqrestore(&ata_lock, ata_eflags);
    (void)ata_prdt;   // PRDT inspected by the driver; nothing else reads it
    return rc;
}

int ata_dma_write_sectors_drive(int drive, unsigned int lba, int count, const unsigned char* buf) {
    if (!ata_dma_ready() || count < 1 || count > ATA_DMA_BATCH_MAX) return -2;

    ata_eflags = spin_lock_irqsave(&ata_lock);
    hdd_activity = 10;
    memcpy(dma_bounce, buf, (uint32_t)count * 512);
    int rc = ata_dma_transfer(drive, lba, count, dma_bounce, 1);
    spin_unlock_irqrestore(&ata_lock, ata_eflags);
    return rc;
}

void ata_read_sector(unsigned int lba, unsigned char* b) {
    ata_read_sector_drive(0, lba, b);
}
int ata_write_sector_drive(int drive, unsigned int lba, unsigned char* b) {
    uint16_t base = ata_base_port(drive);
    ata_eflags = spin_lock_irqsave(&ata_lock);
    hdd_activity = 10; // set activity frames
    if (ata_wait_bsy_drive(drive) < 0) { spin_unlock_irqrestore(&ata_lock, ata_eflags); return -1; }
    outb(base + 6, ((drive & 1) ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F)); outb(base + 2, 1); outb(base + 3, (unsigned char)lba);
    outb(base + 4, (unsigned char)(lba >> 8)); outb(base + 5, (unsigned char)(lba >> 16)); outb(base + 7, 0x30);
    if (ata_wait_bsy_drive(drive) < 0) { spin_unlock_irqrestore(&ata_lock, ata_eflags); return -1; }
    if (ata_wait_drq_drive(drive) < 0) { spin_unlock_irqrestore(&ata_lock, ata_eflags); return -1; }
    // One `rep outsw` per sector (v38.25): single VM exit for the data phase.
    const unsigned char* pw = b;
    int wc = 256;
    __asm__ volatile("cld; rep outsw" : "+S"(pw), "+c"(wc) : "d"(base) : "memory");
    outb(base + 7, 0xE7); ata_wait_bsy_drive(drive);
    spin_unlock_irqrestore(&ata_lock, ata_eflags);
    return 0;
}
void ata_write_sector(unsigned int lba, unsigned char* b) {
    ata_write_sector_drive(0, lba, b);
}
