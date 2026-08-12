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
    for (int i = 0; i < 256; i++) { 
        unsigned short word = inw(base); 
        b[i * 2] = (unsigned char)word; 
        b[i * 2 + 1] = (unsigned char)(word >> 8); 
    }
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
    for (int i = 0; i < 256; i++) { unsigned short word = b[i * 2] | (b[i * 2 + 1] << 8); outw(base, word); }
    outb(base + 7, 0xE7); ata_wait_bsy_drive(drive);
    spin_unlock_irqrestore(&ata_lock, ata_eflags);
    return 0;
}
void ata_write_sector(unsigned int lba, unsigned char* b) {
    ata_write_sector_drive(0, lba, b);
}
