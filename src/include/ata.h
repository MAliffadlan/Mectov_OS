#ifndef ATA_H
#define ATA_H

void ata_wait_bsy();
void ata_wait_drq();
void ata_read_sector(unsigned int lba, unsigned char* b);
void ata_write_sector(unsigned int lba, unsigned char* b);

#endif
