#include "../include/ata.h"
#include "../include/io.h"
#include "../include/spinlock.h"

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
