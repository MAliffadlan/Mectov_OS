// src/drivers/virtio_net.c — virtio-net NIC driver (v38.174)
//
// The third net_backend_t: rtl8139 (#1) and e1000e (#2) emulate real cards,
// this one is paravirtual. net.h's vtable was introduced for exactly this —
// the protocol layer does not change, only send/poll_rx/irq_clear/get_mac.
//
// Transport: LEGACY/transitional virtio-pci (virtio 0.9.5), one I/O BAR and
// PIO registers, mirroring src/drivers/virtio_blk.c's vblk_hw_init(). Two
// queues — 0 = receiveq, 1 = transmitq (§5.1.2) — each with its own
// page-aligned ring block, because QUEUE_PFN is a frame number.
//
// Poll-only, like virtio_blk: avail.flags = NO_INTERRUPT, so the device never
// raises INTx and no IRQ is wired. net_poll() (src/drivers/net.c) drains up to
// four frames per call from the main loop through net_backend->poll_rx().
//
// The 10-byte virtio_net_hdr in front of every packet is the bring-up
// landmine (see virtio_net.h): TX length includes it, RX length includes it.

#include "../include/virtio_net.h"
#include "../include/virtio.h"
#include "../include/net.h"
#include "../include/initcall.h"
#include "../include/pci.h"
#include "../include/serial.h"
#include "../include/utils.h"    // memcpy/memset
#include "../include/io.h"       // inb/outb/inw/outw/inl/outl
#include "../include/spinlock.h"

// Queue indices (virtio 0.9.5 §5.1.2).
#define VNET_Q_RX       0
#define VNET_Q_TX       1

#define VNET_QSIZE_MAX  256         // largest QueueNum we lay out a ring for
// Ring memory per queue: desc[qsz] (16B) + avail (6+2*qsz), then the used
// ring (6+8*qsz) on the next 4K boundary. Worst case qsz=256: 4096+518 ->
// used@8192 (2054B) -> ends at 10246, so 12KB covers it (same bound as the
// blk driver's VBLK_QMEM).
#define VNET_QMEM       12288
#define VNET_ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define VNET_POLL_TRIES 2000000

// Descriptors we actually keep posted. The RING is laid out for the device's
// QueueNum (QEMU reports 256), but there is no reason to pin 256 * 1.5KB of
// .bss for buffers: net_poll() drains four frames per call, so 64 slots is
// far more than a poll cycle can consume.
#define VNET_RX_POST    64

typedef struct {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vnet_desc_t;                      // 16 bytes

typedef struct {
    uint8_t* mem;                   // page-aligned ring block (QUEUE_PFN)
    uint16_t qsize;                 // device QueueNum (ring layout derives from it)
    uint32_t avail_off;             // 16*qsize
    uint32_t used_off;              // 4K-aligned past desc+avail
    uint16_t last_used;             // poll cursor into the used ring
    uint16_t avail_idx;             // our avail-ring producer cursor
} vnet_q_t;

static int      vnet_in_use;
static uint16_t vnet_iobase;
static vnet_q_t vnet_rxq, vnet_txq;

static uint8_t vnet_rx_qmem[VNET_QMEM] __attribute__((aligned(4096)));
static uint8_t vnet_tx_qmem[VNET_QMEM] __attribute__((aligned(4096)));

static uint8_t vnet_rx_bufs[VNET_RX_POST][VNET_BUF_SIZE] __attribute__((aligned(16)));
static uint8_t vnet_tx_buf[VNET_BUF_SIZE] __attribute__((aligned(16)));

uint8_t virtio_net_mac[6] = {0};
int     virtio_net_present_flag = 0;

static net_backend_t vnet_backend;
static spinlock_t vnet_lock = SPINLOCK_INIT;

// ---- raw ring accessors ----
// The ring sizes vary with the device's QueueNum, so fixed C structs cannot
// describe them; these mirror virtio_blk.c's vblk_* accessors exactly.
// avail: flags@+0, idx@+2, ring[]@+4 (2 bytes/entry)
// used:  flags@+0, idx@+2, elem[]@+4 (8 bytes/entry: id@+0, len@+4)
static vnet_desc_t* vnet_desc(vnet_q_t* q, int i) {
    return (vnet_desc_t*)&q->mem[(uint32_t)i * 16];
}
static uint16_t vnet_used_idx(vnet_q_t* q) {
    uint16_t v; memcpy(&v, &q->mem[q->used_off + 2], 2); return v;
}
static uint32_t vnet_used_id(vnet_q_t* q, uint16_t idx) {
    uint32_t v;
    memcpy(&v, &q->mem[q->used_off + 4 + (uint32_t)(idx % q->qsize) * 8], 4);
    return v;
}
static uint32_t vnet_used_len(vnet_q_t* q, uint16_t idx) {
    uint32_t v;
    memcpy(&v, &q->mem[q->used_off + 4 + (uint32_t)(idx % q->qsize) * 8 + 4], 4);
    return v;
}
// Publish one descriptor to the avail ring and advance our producer cursor.
// The idx store is what the device reads, so it goes last.
static void vnet_avail_post(vnet_q_t* q, uint16_t head) {
    uint16_t slot = (uint16_t)(q->avail_idx % q->qsize);
    memcpy(&q->mem[q->avail_off + 4 + (uint32_t)slot * 2], &head, 2);
    q->avail_idx++;
    memcpy(&q->mem[q->avail_off + 2], &q->avail_idx, 2);
}
static void vnet_barrier(void) { __asm__ __volatile__("" ::: "memory"); }

static void vnet_notify(int qidx) {
    outw(vnet_iobase + VIRTIO_IO_QUEUE_NOTIFY, (uint16_t)qidx);
}

// Lay out one queue from the size the DEVICE reports and hand it over.
// Legacy PCI has no size register the driver could set: the device computes
// every ring address from its own QueueNum, so a driver-side ring of a
// different size would put avail/used where the device never looks.
static int vnet_queue_init(vnet_q_t* q, uint16_t io, int qidx) {
    outw(io + VIRTIO_IO_QUEUE_SEL, (uint16_t)qidx);
    uint16_t qsz = inw(io + VIRTIO_IO_QUEUE_NUM);
    if (qsz < 8 || qsz > VNET_QSIZE_MAX) return -1;

    q->qsize = qsz;
    q->avail_off = (uint32_t)qsz * 16;
    q->used_off = VNET_ALIGN_UP(q->avail_off + 6 + (uint32_t)qsz * 2, 4096);
    if (q->used_off + 6 + (uint32_t)qsz * 8 > VNET_QMEM) return -1;

    memset(q->mem, 0, VNET_QMEM);
    // Poll-only: ask the device not to raise INTx on consume.
    uint16_t noint = VIRTQ_AVAIL_F_NO_INTERRUPT;
    memcpy(&q->mem[q->avail_off], &noint, 2);
    vnet_barrier();

    uint32_t pfn = (uint32_t)(uintptr_t)q->mem >> 12;
    if (pfn == 0) return -1;
    outl(io + VIRTIO_IO_QUEUE_PFN, pfn);

    q->last_used = 0;
    q->avail_idx = 0;
    return 0;
}

// Post the RX buffers the device may fill.
static void vnet_rx_fill(void) {
    for (int i = 0; i < VNET_RX_POST; i++) {
        vnet_desc_t* d = vnet_desc(&vnet_rxq, i);
        d->addr_lo = (uint32_t)(uintptr_t)vnet_rx_bufs[i];
        d->addr_hi = 0;
        d->len = VNET_BUF_SIZE;
        d->flags = VIRTQ_DESC_F_WRITE;   // device writes into this buffer
        d->next = 0;
        vnet_avail_post(&vnet_rxq, (uint16_t)i);
    }
    vnet_barrier();
    vnet_notify(VNET_Q_RX);
}

// ---- net_backend_t implementation ----

static int vnet_send(const uint8_t* frame, uint32_t len) {
    if (!vnet_in_use || len == 0 || len > 1514) return -1;
    uint32_t nf = spin_lock_irqsave(&vnet_lock);

    // Header first, then the frame — the descriptor length counts BOTH.
    memset(vnet_tx_buf, 0, VNET_HDR_SIZE);
    memcpy(vnet_tx_buf + VNET_HDR_SIZE, frame, len);

    vnet_desc_t* d = vnet_desc(&vnet_txq, 0);
    d->addr_lo = (uint32_t)(uintptr_t)vnet_tx_buf;
    d->addr_hi = 0;
    d->len = VNET_HDR_SIZE + len;
    d->flags = 0;                        // device READS this buffer
    d->next = 0;
    vnet_barrier();
    vnet_avail_post(&vnet_txq, 0);
    vnet_barrier();
    vnet_notify(VNET_Q_TX);

    // The buffer is reused, so wait (bounded) for the device to release it.
    int ok = -1;
    for (uint32_t t = 0; t < VNET_POLL_TRIES; t++) {
        if (vnet_used_idx(&vnet_txq) != vnet_txq.last_used) {
            vnet_txq.last_used++;
            ok = 0;
            break;
        }
        __asm__ __volatile__("pause");
    }
    spin_unlock_irqrestore(&vnet_lock, nf);
    return (ok == 0) ? (int)len : -1;
}

// One frame per call (the vtable contract), 0 when nothing is ready.
static int vnet_poll_rx(uint8_t* out_buf, uint32_t max_len) {
    if (!vnet_in_use) return 0;
    int got = 0;
    uint32_t nf = spin_lock_irqsave(&vnet_lock);

    if (vnet_used_idx(&vnet_rxq) != vnet_rxq.last_used) {
        uint32_t id  = vnet_used_id(&vnet_rxq, vnet_rxq.last_used);
        uint32_t len = vnet_used_len(&vnet_rxq, vnet_rxq.last_used);
        vnet_rxq.last_used++;

        // len counts the 10-byte header too; the frame begins after it.
        if (id < VNET_RX_POST && len > VNET_HDR_SIZE) {
            uint32_t copy = len - VNET_HDR_SIZE;
            if (copy > max_len) copy = max_len;
            memcpy(out_buf, vnet_rx_bufs[id] + VNET_HDR_SIZE, copy);
            got = (int)copy;
        }
        // Hand the slot back either way: a malformed or oversized frame must
        // not strand a descriptor, or the queue drains for good.
        if (id < VNET_RX_POST) {
            vnet_avail_post(&vnet_rxq, (uint16_t)id);
            vnet_barrier();
            vnet_notify(VNET_Q_RX);
        }
    }
    spin_unlock_irqrestore(&vnet_lock, nf);
    return got;
}

// Poll-only: the device is told not to raise INTx, so there is never an
// interrupt cause to read or clear. Present for the vtable.
static uint16_t vnet_irq_clear(void) { return 0; }

static void vnet_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = virtio_net_mac[i];
}

// ---- bring-up ----

static int vnet_hw_init(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t bar0 = pci_read(bus, slot, func, 0x10);
    if (!(bar0 & 1)) return -1;                 // legacy needs an I/O BAR
    uint16_t io = (uint16_t)(bar0 & ~3u);
    if (io == 0) return -1;
    vnet_iobase = io;

    // Enable I/O space + bus mastering (same pattern as rtl8139/virtio-blk).
    uint32_t cmd = pci_read(bus, slot, func, 0x04);
    pci_write(bus, slot, func, 0x04, cmd | 0x5);

    // Reset, then ACK + DRIVER.
    outb(io + VIRTIO_IO_STATUS, 0);
    outb(io + VIRTIO_IO_STATUS, VIRTIO_S_ACK);
    outb(io + VIRTIO_IO_STATUS, VIRTIO_S_ACK | VIRTIO_S_DRIVER);

    // Negotiate a minimal subset: the MAC only. Deliberately NOT MRG_RXBUF
    // (it would append num_buffers to the header) and not VERSION_1.
    uint32_t feat = inl(io + VIRTIO_IO_DEVICE_FEATS);
    uint32_t want = feat & VNET_F_MAC;
    outl(io + VIRTIO_IO_GUEST_FEATS, want);
    outb(io + VIRTIO_IO_STATUS,
         VIRTIO_S_ACK | VIRTIO_S_DRIVER | VIRTIO_S_FEATURES_OK);
    if (!(inb(io + VIRTIO_IO_STATUS) & VIRTIO_S_FEATURES_OK)) return -1;

    vnet_rxq.mem = vnet_rx_qmem;
    vnet_txq.mem = vnet_tx_qmem;
    if (vnet_queue_init(&vnet_rxq, io, VNET_Q_RX) != 0) return -1;
    if (vnet_queue_init(&vnet_txq, io, VNET_Q_TX) != 0) return -1;

    // MAC: the first 6 bytes of the device config space (legacy layout).
    if (want & VNET_F_MAC) {
        uint32_t lo = inl(io + VIRTIO_IO_CONFIG);
        uint16_t hi = inw(io + VIRTIO_IO_CONFIG + 4);
        virtio_net_mac[0] = (uint8_t)(lo & 0xFF);
        virtio_net_mac[1] = (uint8_t)((lo >> 8) & 0xFF);
        virtio_net_mac[2] = (uint8_t)((lo >> 16) & 0xFF);
        virtio_net_mac[3] = (uint8_t)((lo >> 24) & 0xFF);
        virtio_net_mac[4] = (uint8_t)(hi & 0xFF);
        virtio_net_mac[5] = (uint8_t)((hi >> 8) & 0xFF);
    } else {
        // The device offered no MAC. Synthesize a deterministic
        // locally-administered one (bit 1 of the first octet set) rather
        // than shipping an all-zero address: DHCP would send chaddr 00:00:..
        // and ARP would answer for a MAC nobody owns.
        virtio_net_mac[0] = 0x02;
        virtio_net_mac[1] = 0x00;
        virtio_net_mac[2] = bus;
        virtio_net_mac[3] = slot;
        virtio_net_mac[4] = func;
        virtio_net_mac[5] = 0x01;
        write_serial_string("[VIRTIO] net: no F_MAC, using synthesized MAC\n");
    }

    (void)inb(io + VIRTIO_IO_ISR);              // ack any stale interrupt
    outb(io + VIRTIO_IO_STATUS,
         VIRTIO_S_ACK | VIRTIO_S_DRIVER | VIRTIO_S_FEATURES_OK | VIRTIO_S_DRIVER_OK);

    vnet_rx_fill();
    (void)inb(io + VIRTIO_IO_ISR);
    return 0;
}

void virtio_net_init(void) {
    vnet_in_use = 0;
    virtio_net_present_flag = 0;

    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t* d = &pci_devices[i];
        if (d->vendor_id != VIRTIO_PCI_VENDOR) continue;
        if (d->device_id != VIRTIO_PCI_DEV_NET_LEGACY) continue;  // modern (0x104x): skip
        if (vnet_hw_init(d->bus, d->slot, d->func) != 0) {
            write_serial_string("[VIRTIO] net init failed\n");
            continue;
        }

        vnet_in_use = 1;
        virtio_net_present_flag = 1;
        vnet_backend.send     = vnet_send;
        vnet_backend.poll_rx  = vnet_poll_rx;
        vnet_backend.irq_clear = vnet_irq_clear;
        vnet_backend.get_mac  = vnet_get_mac;
        vnet_backend.present  = 1;
        vnet_backend.name     = "virtio-net";
        // Registers with net.c AND the device registry (class NET).
        net_register_backend(&vnet_backend);

        write_serial_string("[VIRTIO] net io=");
        write_serial_hex((uint32_t)vnet_iobase);
        write_serial_string(" mac=");
        for (int m = 0; m < 6; m++) {
            write_serial_hex(virtio_net_mac[m]);
            if (m < 5) write_serial(':');
        }
        write_serial_string("\n");
        break;                                  // one NIC is enough
    }
}

int virtio_net_present(void) { return vnet_in_use; }

// Level 18: after e1000e (17), before net_init (20) — the backend must be
// registered before the protocol layer starts touching hardware.
INITCALL(virtio_net_init, 18);
