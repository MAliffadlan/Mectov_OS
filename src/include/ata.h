#ifndef ATA_H
#define ATA_H

#include "idt.h"  // registers_t (for the IRQ entry points)

extern volatile int hdd_activity;

int ata_wait_bsy();
int ata_wait_drq();
void ata_read_sector(unsigned int lba, unsigned char* b);
int ata_read_sector_drive(int drive, unsigned int lba, unsigned char* b);
void ata_write_sector(unsigned int lba, unsigned char* b);
int ata_write_sector_drive(int drive, unsigned int lba, unsigned char* b);

// Bus-mastering DMA (v38.26): when the PCI IDE controller exposes a BMIDE
// BAR, contiguous runs are moved by the controller's PRD engine instead of
// port I/O. See ata.c for the details; these let callers stay unchanged.
void ata_dma_init(void);                 // detect BMIDE + enable bus mastering
int  ata_dma_ready(void);                // 1 when a DMA transfer just succeeded
// Return 0 on success (data in buf), -1 on DMA failure (caller keeps the PIO
// path as fallback), -2 when BMIDE is not present/usable.
int ata_dma_read_sectors_drive(int drive, unsigned int lba, int count, unsigned char* buf);
int ata_dma_write_sectors_drive(int drive, unsigned int lba, int count, const unsigned char* buf);
// IRQ entry points (INT 46 = IRQ14 primary, INT 47 = IRQ15 secondary): clear
// the latched BMIDE interrupt + record completion.
void ata_dma_irq_primary(registers_t* r);
void ata_dma_irq_secondary(registers_t* r);
// Multi-sector PIO (v38.25): read `count` CONTIGUOUS sectors with ONE ATA
// command — the drive pulses DRQ once per sector, so sectors 2..count skip
// the per-sector command setup + BSY latency. `count` is clamped to
// ATA_BATCH_MAX and never crosses the 128-sector LBA boundary. Returns 0 on
// success, -1 on error. buf must hold count*512 bytes.
#define ATA_BATCH_MAX 128
int ata_read_sectors_drive(int drive, unsigned int lba, int count, unsigned char* b);
int ata_write_sectors_drive(int drive, unsigned int lba, int count, const unsigned char* b);
// The batch size a caller should actually hand to a multi-sector command:
// clamped to ATA_BATCH_MAX and to the 128-sector LBA boundary (a
// multi-sector transfer may not cross it). Callers MUST advance by the
// value returned, not by the raw count they asked for.
static inline int ata_batch_limit(unsigned int lba, int count) {
    if (count > ATA_BATCH_MAX) count = ATA_BATCH_MAX;
    int boundary = 128 - (int)(lba & 127);
    if (count > boundary) count = boundary;
    return (count < 1) ? 1 : count;
}

#endif
