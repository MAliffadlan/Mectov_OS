#include "../include/ata.h"
#include "../include/io.h"

void ata_wait_bsy() { while(inb(0x1F7) & 0x80); }
void ata_wait_drq() { while(!(inb(0x1F7) & 0x08)); }

volatile int hdd_activity = 0;

void ata_read_sector(unsigned int lba, unsigned char* b) {
    hdd_activity = 10; // set activity frames
    ata_wait_bsy(); outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F)); outb(0x1F2, 1); outb(0x1F3, (unsigned char)lba);
    outb(0x1F4, (unsigned char)(lba >> 8)); outb(0x1F5, (unsigned char)(lba >> 16)); outb(0x1F7, 0x20);
    ata_wait_bsy(); ata_wait_drq();
    for (int i = 0; i < 256; i++) { unsigned short word = inw(0x1F0); b[i * 2] = (unsigned char)word; b[i * 2 + 1] = (unsigned char)(word >> 8); }
}
void ata_write_sector(unsigned int lba, unsigned char* b) {
    hdd_activity = 10; // set activity frames
    ata_wait_bsy(); outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F)); outb(0x1F2, 1); outb(0x1F3, (unsigned char)lba);
    outb(0x1F4, (unsigned char)(lba >> 8)); outb(0x1F5, (unsigned char)(lba >> 16)); outb(0x1F7, 0x30);
    ata_wait_bsy(); ata_wait_drq();
    for (int i = 0; i < 256; i++) { unsigned short word = b[i * 2] | (b[i * 2 + 1] << 8); outw(0x1F0, word); }
    outb(0x1F7, 0xE7); ata_wait_bsy();
}
