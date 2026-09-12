#ifndef VIRTIO_H
#define VIRTIO_H

#include "types.h"

// Legacy/transitional PCI transport (virtio 0.9.5). We deliberately target
// the LEGACY interface only: one I/O BAR, PIO register access, no MSI-X —
// the kernel has no MSI support (see xhci.h), and QEMU exposes the legacy
// interface for `-device virtio-{blk,net}-pci,disable-modern=on`
// (vendor 0x1AF4, legacy device IDs 0x1000/0x1001). Modern-only devices
// (0x1040+) are skipped by the drivers.

#define VIRTIO_PCI_VENDOR        0x1AF4
#define VIRTIO_PCI_DEV_NET_LEGACY 0x1000
#define VIRTIO_PCI_DEV_BLK_LEGACY 0x1001

// BAR0 I/O-register offsets (bytes).
#define VIRTIO_IO_DEVICE_FEATS  0x00    // 32-bit: device feature bits
#define VIRTIO_IO_GUEST_FEATS   0x04    // 32-bit: negotiated subset
#define VIRTIO_IO_QUEUE_PFN     0x08    // 32-bit: virtqueue page frame number
#define VIRTIO_IO_QUEUE_NUM     0x0C    // 16-bit: max queue size (device)
#define VIRTIO_IO_QUEUE_SEL     0x0E    // 16-bit: current queue select
#define VIRTIO_IO_QUEUE_NOTIFY  0x10    // 16-bit: notify queue by index
#define VIRTIO_IO_STATUS        0x12    // 8-bit: device status
#define VIRTIO_IO_ISR           0x13    // 8-bit: interrupt status (read-to-ack)
#define VIRTIO_IO_CONFIG        0x14    // device-specific config space

// Device-status bits.
#define VIRTIO_S_ACK            0x01    // guest knows the device
#define VIRTIO_S_DRIVER         0x02    // guest knows how to drive it
#define VIRTIO_S_DRIVER_OK      0x04    // driver is set up and ready
#define VIRTIO_S_FEATURES_OK    0x08    // negotiated features confirmed
#define VIRTIO_S_NEEDS_RESET    0x40
#define VIRTIO_S_FAILED         0x80

// Virtqueue descriptor flags.
#define VIRTQ_DESC_F_NEXT       1       // chained with the next descriptor
#define VIRTQ_DESC_F_WRITE      2       // device writes (vs device reads)
// Available-ring flags.
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1    // don't raise INTx on consume

#endif
