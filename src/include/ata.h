#ifndef ATA_H
#define ATA_H

extern volatile int hdd_activity;

int ata_wait_bsy();
int ata_wait_drq();
void ata_read_sector(unsigned int lba, unsigned char* b);
int ata_read_sector_drive(int drive, unsigned int lba, unsigned char* b);
void ata_write_sector(unsigned int lba, unsigned char* b);
int ata_write_sector_drive(int drive, unsigned int lba, unsigned char* b);
// Multi-sector PIO (v38.25): read `count` CONTIGUOUS sectors with ONE ATA
// command — the drive pulses DRQ once per sector, so sectors 2..count skip
// the per-sector command setup + BSY latency. `count` is clamped to
// ATA_BATCH_MAX and never crosses the 128-sector LBA boundary. Returns 0 on
// success, -1 on error. buf must hold count*512 bytes.
#define ATA_BATCH_MAX 16
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
