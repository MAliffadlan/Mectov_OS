#ifndef ATA_H
#define ATA_H

extern volatile int hdd_activity;

int ata_wait_bsy();
int ata_wait_drq();
void ata_read_sector(unsigned int lba, unsigned char* b);
int ata_read_sector_drive(int drive, unsigned int lba, unsigned char* b);
void ata_write_sector(unsigned int lba, unsigned char* b);
int ata_write_sector_drive(int drive, unsigned int lba, unsigned char* b);

#endif
