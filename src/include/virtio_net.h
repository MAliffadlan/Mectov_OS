#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include "types.h"

// virtio-net — the third net_backend_t implementation (see net.h).
//
// Paravirtual, so it costs no emulation: the guest and the hypervisor share
// the ring directly instead of emulating a register-level card. This is the
// NIC a KVM guest would normally boot on.
//
// Only the LEGACY/transitional PCI transport is targeted (one I/O BAR, PIO
// registers, no MSI-X) — the same interface virtio-blk uses, and the same
// reason: the kernel has no MSI support. Attach it with
//
//   -netdev user,id=n0 -device virtio-net-pci,disable-modern=on,netdev=n0
//
// Modern-only devices (0x1040+) are skipped by the probe.
//
// Unlike rtl8139/e1000e this driver is POLL-ONLY: it asks the device never to
// raise INTx (avail.flags = NO_INTERRUPT) and lets net_poll() drain it, the
// same way virtio-blk is driven. No IRQ is wired.

void virtio_net_init(void);
int  virtio_net_present(void);

extern uint8_t virtio_net_mac[6];
extern int     virtio_net_present_flag;

// Device feature bits we care about (virtio 0.9.5 §5.1.3).
#define VNET_F_MAC      (1u << 5)   // device supplies a MAC in config space

// The legacy virtio-net packet header (10 bytes).
//
// Every buffer handed to the device AND every buffer it writes back begins
// with one of these. This is the single most likely bring-up mistake:
//   - TX descriptor length is VNET_HDR_SIZE + frame_len, not frame_len;
//   - the RX used-ring length INCLUDES the header, so the Ethernet frame
//     starts at buf + VNET_HDR_SIZE.
//
// It is exactly 10 bytes because this driver negotiates neither
// VIRTIO_NET_F_MRG_RXBUF (which would append a 16-bit num_buffers field)
// nor VIRTIO_F_VERSION_1 (modern). Negotiating either would silently change
// this struct and shift every frame by two bytes.
typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} virtio_net_hdr_t;

#define VNET_HDR_SIZE   10
#define VNET_BUF_SIZE   (VNET_HDR_SIZE + 1520)   // header + one Ethernet frame

#endif // VIRTIO_NET_H
