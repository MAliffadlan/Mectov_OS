#ifndef AHCI_H
#define AHCI_H

#include "types.h"

// AHCI (SATA) driver (v38.50). The controller is found on PCI as class 01 /
// subclass 06 / prog_if 01; its register block is BAR5 (memory-mapped, in the
// identity-mapped PCI MMIO window). Ports with a SATA disk attached are
// registered as drives 4+ on the existing ATA sector API — ext2/FAT32/pcache
// and the runtime mount() work on SATA disks with zero changes.
//
// Transfers use command slot 0 with a single PRD entry into a 64K-aligned
// bounce buffer (the whole 128-sector batch fits one region), issued as
// READ/WRITE DMA EXT (LBA48-capable, 8-bit sector count) and completed by
// polling PxCI like the IDE DMA path — everything runs under ahci_lock with
// IRQs off, mirroring the ata.c model.

#define AHCI_DRIVE_BASE 4          // drives 0-3 are IDE; 4+ are AHCI ports
#define AHCI_MAX_PORTS  4          // ports we bring up (QEMU ich9 exposes 6)

// Detect + bring up the controller (call after pci_scan, e.g. next to
// ata_dma_init). Safe when no controller exists: logs and leaves.
void ahci_init(void);
int  ahci_present(void);           // 1 when a controller was initialised

// Sector API on an AHCI drive number (drive as passed to the ATA layer,
// i.e. AHCI_DRIVE_BASE + port). count is clamped like the IDE batch API.
// Returns 0 on success, -1 on error (controller absent is also -1).
int ahci_read_sectors(int drive, uint32_t lba, int count, uint8_t* buf);
int ahci_write_sectors(int drive, uint32_t lba, int count, const uint8_t* buf);

#endif
