#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include "types.h"

// VirtIO block driver (transitional/legacy PCI, poll-only). Disks behind a
// virtio-blk controller register as drives 12+ on the existing ATA sector
// API — ext2/FAT32/pcache and the runtime mount() work on them unchanged:
//
//     mount /vblk fat32 12
//
// QEMU opt-in (NOT a default drive — no guest change without it):
//     -drive file=virtio.img,format=raw,if=none,id=vd0
//     -device virtio-blk-pci,drive=vd0,disable-modern=on
// or `MECTOV_VIRTIO=1 ./run.sh`. Poll-only like the AHCI path: no IRQ
// wiring, the avail ring runs with NO_INTERRUPT so the device never
// asserts a shared INTx line the IDT has no stub for.

#define VIRTIO_BLK_BASE 12         // drives 0-3 IDE, 4-7 AHCI, 8-11 USB, 12+ virtio
#define VIRTIO_BLK_MAX  4          // controllers brought up (PCI order)

void virtio_blk_init(void);        // probe PCI + negotiate (call after pci_scan)
int  virtio_blk_present(void);     // 1 when at least one disk is attached

// Sector API on a virtio drive number (VIRTIO_BLK_BASE + index). count is
// chunked internally, so callers may pass any ata_batch_limit() run.
// Returns 0 on success, -1 on error (no device is also -1).
int virtio_blk_read_sectors(int drive, uint32_t lba, int count, uint8_t* buf);
int virtio_blk_write_sectors(int drive, uint32_t lba, int count, const uint8_t* buf);

#endif
