// src/drivers/e1000e.c — Intel e1000/e1000e NIC driver (v38.170)
//
// Speaks the net_backend_t contract (net.h) with the same semantics rtl8139.c
// provides, so `QEMU -device e1000` gets identical networking (slirp, DHCP,
// TCP, the TLS fetch app) with zero protocol-layer changes. Priority-based
// registration means e1000e wins when both cards are present, and rtl8139
// remains the fallback.
//
// Register map (82540EM / 82574, both share the base layout):
//   0x0000 CTRL        — device control (RST, SLU, and PHY resets)
//   0x0008 STATUS      — link up / speed
//   0x0010 EECD        — EEPROM control (MAC lives in the EEPROM)
//   0x0100 RCTL        — receive control
//   0x2800 RDBAL/DBAH  — rx ring base (lo at 0x2800, hi at 0x2804)
//   0x2804 RDLEN       — rx ring length (bytes)
//   0x2810 RDH         — rx head (device-owned write pointer)
//   0x2818 RDT         — rx tail (driver-owned refill pointer)
//   0x2800 TCTL        — transmit control (real offset 0x0400)
//   0x3800 TDBAL/L     — tx ring base
//   0x3808 TDLEN
//   0x3810 TDH         — tx head
//   0x3818 TDT         — tx tail
//   0x00C0 ICR         — interrupt cause (W1C)
//   0x00D0 IMS         — interrupt mask set
//   0x010000 RAL/RAH   — the first receive address filter slot (also our MAC)
//
// Everything is MMIO (BAR0). The kernel identity map keeps address ==
// physical, so the descriptor rings can be handed to the NIC as their kernel
// addresses — the same trick virtio_blk and AHCI already use.

#include "../include/e1000e.h"
#include "../include/net.h"       // net_backend_t / net_register_backend()
#include "../include/pci.h"       // pci_read / pci_write / pci_devices
#include "../include/mem.h"       // kmalloc / kfree
#include "../include/serial.h"
#include "../include/utils.h"     // memset / memcpy
#include "../include/spinlock.h"
#include "../include/initcall.h"

uint8_t e1000e_mac[6];
int     e1000e_present_flag = 0;

static volatile uint32_t* regs = 0;         // MMIO base (identity-mapped)
static int nic_bus, nic_slot, nic_func;
static spinlock_t e1000e_lock = SPINLOCK_INIT;

// ---- Ring sizes (82574 accepts up to 4096 descriptors; 82540EM 128+) ----
// QEMU's e1000 model has no minimum beyond the layout, and 64 descriptors of
// 2 KB buffers covers the drainage the IRQ handler performs (8 frames/IRQ)
// with several laps of headroom.
#define RX_DESC_COUNT 64
#define TX_DESC_COUNT 64
#define RX_BUF_SIZE   2048

// Legacy rx descriptor (82540EM curve, what QEMU's e1000 exposes):
//   [0..3]  buffer address (physical)
//   [4..5]  length (written by the device)
//   [6..7]  checksum (device)
//   [8..15] status (bit0 DD = descriptor done) + errors + special
typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) rx_desc_t;

// Legacy tx descriptor — EXACTLY 16 bytes (QEMU's struct e1000_tx_desc):
//   [0..7]  buffer address,
//   [8..9]  length, [10] CSO, [11] CMD  (the 'lower' union member),
//   [12] STA (DD when the device is done), [13] CSS, [14..15] special
//   (the 'upper' union member). A single extra byte shifts every descriptor
//   after the first into misaligned fields: the device read garbage from
//   'addr' and transmitted a zero-L2 frame for every send after the first
//   (length was never even honored). Verify sizes on the host before boot:
//   sizeof must print 16.
typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;              // bit0 EOP, bit1 IFCS, bit3 RS (report status)
    uint8_t  sta;              // DD when done (device writes)
    uint8_t  css;
    uint16_t special;
} __attribute__((packed)) tx_desc_t;

// The rings and buffers are static .bss with 4 KB alignment — the same shape
// virtio_blk uses. The kernel identity map makes address == physical, so the
// ring base can be written into RDBAL/TDBAL directly, and kmalloc's 16-byte
// granularity (which put a 64-desc ring somewhere no alignment rule of the
// e1000 was enforceable on the first bring-up) never enters the picture.
#define TX_BUF_SIZE 4096
static rx_desc_t rx_ring[RX_DESC_COUNT] __attribute__((aligned(4096)));
static tx_desc_t tx_ring[TX_DESC_COUNT] __attribute__((aligned(4096)));
static uint8_t   rx_bufs[RX_DESC_COUNT * RX_BUF_SIZE] __attribute__((aligned(4096)));
static uint8_t   tx_bufs[TX_DESC_COUNT * TX_BUF_SIZE] __attribute__((aligned(4096)));
static int        rx_tail_idx = 0;
static int        tx_head_idx = 0;          // next slot to fill
static int        tx_tail_idx = 0;          // last slot handed to the device
static int        tx_free = TX_DESC_COUNT;  // descriptors not yet done

static inline uint32_t rd32(uint32_t off) { return regs[off / 4]; }
static inline void     wr32(uint32_t off, uint32_t v) { regs[off / 4] = v; }

// ---- MMIO helpers done. Init picks the card up from PCI. ----

static int e1000e_find_pci(void) {
    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t* d = &pci_devices[i];
        if (d->vendor_id != 0x8086) continue;
        // 0x100E = 82540EM (QEMU `-device e1000`), 0x10D3 = 82574L
        // (QEMU `-device e1000e`), 0x1533/0x1570 etc share the layout too.
        if (d->device_id == 0x100E || d->device_id == 0x10D3 ||
            d->device_id == 0x1533) {
            nic_bus = d->bus; nic_slot = d->slot; nic_func = d->func;
            return 1;
        }
    }
    return 0;
}

// Assigned MMIO BAR0 address (SeaBIOS has programmed it by the time we run);
// handle the 64-bit memory form by folding in the next BAR's dword.
static uint64_t e1000e_bar0(void) {
    uint32_t lo = pci_read(nic_bus, nic_slot, nic_func, 0x10);
    if (lo & 1) return lo & ~3u;                    // I/O BAR (82540EM sometimes)
    uint64_t base = lo & ~0xFu;
    if (((lo >> 1) & 3) == 2) {                     // 64-bit memory BAR
        uint32_t hi = pci_read(nic_bus, nic_slot, nic_func, 0x14);
        base |= ((uint64_t)hi) << 32;
    }
    return base;
}

static int e1000e_send(const uint8_t* frame, uint32_t len);
static int e1000e_poll_rx(uint8_t* out_buf, uint32_t max_len);

static uint16_t e1000e_irq_clear(void) {
    if (!e1000e_present_flag) return 0;
    uint32_t icr = rd32(0x00C0);
    if (icr) wr32(0x00C0, icr);           // write-1-to-clear
    // Map to the protocol layer's ISR contract: bit0 = "rx available".
    // The e1000's RX daemon sets ICR.RXT (bit 7) or RXQ0 (bit 0 on 82574).
    return (icr & 0xFF) ? 1 : 0;
}

static void e1000e_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = e1000e_mac[i];
}

static net_backend_t e1000e_backend = {
    .send = e1000e_send, .poll_rx = e1000e_poll_rx,
    .irq_clear = e1000e_irq_clear, .get_mac = e1000e_get_mac,
    .present = 0, .name = "e1000e",
};

void e1000e_init(void) {
    write_serial_string("[E1000E] probing for Intel NIC\n");
    if (!e1000e_find_pci()) {
        write_serial_string("[E1000E] no Intel NIC found\n");
        return;
    }

    uint64_t bar = e1000e_bar0();
    write_serial_string("[E1000E] BAR0 = ");
    write_serial_hex((uint32_t)bar);
    write_serial_string("\n");
    if (bar == 0) {
        write_serial_string("[E1000E] BAR0 unassigned\n");
        return;
    }
    regs = (volatile uint32_t*)(uintptr_t)bar;

    // Enable bus mastering + memory space in the PCI command register.
    uint32_t cmd = pci_read(nic_bus, nic_slot, nic_func, 0x04);
    cmd |= (1 << 1) | (1 << 2);           // MEM space + bus master
    pci_write(nic_bus, nic_slot, nic_func, 0x04, cmd);

    // Global reset (CTRL.RST bit 26), then wait for it to clear.
    wr32(0x0000, (1u << 26));
    int wait = 100000;
    while (wait-- > 0 && (rd32(0x0000) & (1u << 26))) { }
    write_serial_string("[E1000E] reset done\n");

    // MAC: QEMU + SeaBIOS auto-load the EEPROM into RAL/RAH at reset
    // (0x5400/0x5404 on both the 82540EM and the 82574).
    uint32_t ral_lo = rd32(0x5400);
    uint32_t rah_hi = rd32(0x5404);
    e1000e_mac[0] = (uint8_t)(ral_lo & 0xFF);
    e1000e_mac[1] = (uint8_t)((ral_lo >> 8) & 0xFF);
    e1000e_mac[2] = (uint8_t)((ral_lo >> 16) & 0xFF);
    e1000e_mac[3] = (uint8_t)((ral_lo >> 24) & 0xFF);
    e1000e_mac[4] = (uint8_t)(rah_hi & 0xFF);
    e1000e_mac[5] = (uint8_t)((rah_hi >> 8) & 0xFF);
    // RAH bit31 (Address Valid) gates the unicast receive filter: without it
    // the MAC filter silently drops every frame addressed to us — the DHCP
    // DISCOVER goes out (TX works) and the OFFER never arrives. QEMU models
    // the EEPROM autoload, but a SeaBIOS build may leave AV clear; set it from
    // software so the filter is live either way.
    if (!(rah_hi & (1u << 31))) {
        wr32(0x5404, rah_hi | (1u << 31));
        write_serial_string("[E1000E] RAH AV was clear, set\n");
    }
    write_serial_string("[E1000E] MAC ");
    for (int i = 0; i < 6; i++) {
        write_serial_hex(e1000e_mac[i]);
        if (i < 5) write_serial(':');
    }
    write_serial('\n');

    // Rings + buffers are static; just zero them and point each rx descriptor
    // at its own buffer.
    memset((void*)rx_ring, 0, sizeof(rx_ring));
    memset((void*)tx_ring, 0, sizeof(tx_ring));
    for (int i = 0; i < RX_DESC_COUNT; i++) {
        rx_ring[i].addr = (uint64_t)(uintptr_t)(rx_bufs + (uint32_t)i * RX_BUF_SIZE);
    }
    rx_tail_idx = 0;
    tx_head_idx = tx_tail_idx = 0;
    tx_free = TX_DESC_COUNT;

    // Rx ring (legacy layout on these devices).
    wr32(0x2800, (uint32_t)(uintptr_t)rx_ring);                  // RDBAL
    wr32(0x2804, 0);                                            // RDBAH
    wr32(0x2808, RX_DESC_COUNT * sizeof(rx_desc_t));            // RDLEN
    wr32(0x2810, 0);                       // RDH (device-owned, reset to 0)
    // RCTL: EN | SBP? no strip CRC | LPE (long pkt up to 16k) | BAM | BSIZE=2048
    //       BSEX=0 -> buffer size = 2048 (bits 16..17 = 00b, so plain 2048)
    //       SECRC? no. UPE/MPE? (unicast/multicast promiscuous) — BAM covers the
    //       broadcast slirp needs.
    // RCTL: EN | UPE | MPE | LPE | BAM — full promiscuous + broadcast. The
    // unicast filter + RAH.AV handle адреса matching on real hardware, but
    // during bring-up the whole filter stack is one thing to suspect, and
    // slirp's traffic is addressed to us anyway. Trim later if profiling
    // ever cares.
    wr32(0x0100, (1u << 1)      // EN
              | (1u << 3)       // UPE: unicast promiscuous
              | (1u << 4)       // MPE: multicast promiscuous
              | (1u << 5)       // LPE
              | (1u << 15));    // BAM

    // Tx ring.
    wr32(0x3800, (uint32_t)(uintptr_t)tx_ring);                  // TDBAL
    wr32(0x3804, 0);                                            // TDBAH
    wr32(0x3808, TX_DESC_COUNT * sizeof(tx_desc_t));            // TDLEN
    wr32(0x3810, 0);                                            // TDH
    wr32(0x3818, 0);                                            // TDT
    // TCTL: EN | PSP (pad short packets) | CT=0x0F collision threshold |
    //       COLD=0x3F (full duplex: 64 collisions)
    wr32(0x0400, (1u << 1)       // EN
              | (1u << 3)        // PSP
              | (0x0Fu << 4)     // CT
              | (0x3Fu << 12));  // COLD

    // Interrupts: RXT (rx timer, bit7) + RXQ0 (bit0, 82574) + LSC (link, bit2).
    wr32(0x00D0, (1u << 7) | (1u << 0) | (1u << 2));
    wr32(0x00C0, 0xFFFFFFFFu);        // clear anything pending

    // Point RDT at the last pre-filled slot so the device may consume them;
    // RDT semantics here are INCLUSIVE-ish ('stop one before it'), see the
    // refill comment below. Walking RDT forward one slot per refill is the
    // standard 8254xx flow.
    wr32(0x2818, RX_DESC_COUNT - 1);

    // TEMP-DEBUG: monitor the descriptor ring's tail/head from the timer tick
    // side too, so we see whether the device ever consumes anything besides
    // descriptor 0.
    extern volatile uint32_t timer_ticks;
    (void)timer_ticks;

    e1000e_present_flag = 1;
    e1000e_backend.present = 1;
    net_register_backend(&e1000e_backend);
    write_serial_string("[E1000E] registered as net backend\n");
}

// Hand one buffer back to the device (rx refill). RDT = the last descriptor
// the device may use; walking it forward tail-by-tail is the standard way.
static void e1000e_rx_refill(int idx) {
    // Order matters: the descriptor must be dead to the device (status cleared,
    // buffer address restored) BEFORE the tail write tells the hardware the
    // slot is back in play — otherwise it can start DMA into a buffer whose
    // descriptor still says 'done'.
    rx_ring[idx].status = 0;
    rx_ring[idx].length = 0;
    rx_ring[idx].errors = 0;
    rx_ring[idx].special = 0;
    rx_ring[idx].addr = (uint64_t)(uintptr_t)(rx_bufs + (uint32_t)idx * RX_BUF_SIZE);
    __asm__ __volatile__("" ::: "memory");
    // RDT is EXCLUSIVE: the hardware may consume descriptors [RDH, RDT).
    // The slot just refilled only enters play when the tail moves PAST it, so
    // writing RDT = idx said "stop before idx" and the device halted right
    // after the first frame (RDH=1, RDT=0 — nothing owned, OFFER received,
    // everything after lost). Writing idx+1 hands the refilled slot over and
    // everything between it and RDH besides. Init's RDT = RDLEN-1 with every
    // descriptor pre-filled follows the same rule (0..len-2 consumable; the
    // last slot joins the ring on the first refill).
    // Keep exactly ONE descriptor un-consumable (RDT never equals RDH —
    // an e1000 with RDH == RDT has an EMPTY ring and stops receiving).
    // RDT = one past the slot just refilled, unless that lands exactly on
    // RDH; then stop one earlier (that slot stays reserved until the next
    // refill picks it up).
    // RDT = one BEFORE the slot just refilled: the hardware owns [RDH, RDT),
    // which now includes every consumed-but-refilled descriptor WITHOUT ever
    // hitting RDH (write RDT=RDH and the e1000 declares the ring empty and
    // stops — a ring where tail catches head during refill). The_NEXT DESCRIPTOR
    // joins the ring on its own refill, one slot of slack at all times.
    wr32(0x2818, (uint32_t)((idx + RX_DESC_COUNT - 1) % RX_DESC_COUNT));
}

// Drain finished rx descriptors, one frame per poll_rx call, exactly like
// rtl8139_poll_rx: each call returns one packet or 0 if none are ready.
static int e1000e_poll_rx(uint8_t* out_buf, uint32_t max_len) {
    if (!e1000e_present_flag) return 0;
    int got = 0;
    uint32_t nf = spin_lock_irqsave(&e1000e_lock);

    // Walk descriptors starting at the tail; DD (bit0 of status) set means the
    // device finished with it.
    int head = (int)rd32(0x2810);         // RDH — device's current position
    (void)head;
    int scanned = 0;
    while (scanned++ < RX_DESC_COUNT) {
        rx_desc_t* d = &rx_ring[rx_tail_idx];
        if (!(d->status & 0x01)) break;   // not done yet
        uint16_t len = d->length;         // bytes written into the buffer
        if (len == 0 || len > RX_BUF_SIZE) {
            // Malformed: skip rather than loop forever on a bad descriptor.
            e1000e_rx_refill(rx_tail_idx);
            rx_tail_idx = (rx_tail_idx + 1) % RX_DESC_COUNT;
            continue;
        }
        uint32_t copy = len;
        if (copy > max_len) copy = max_len;
        memcpy(out_buf, rx_bufs + (uint32_t)rx_tail_idx * RX_BUF_SIZE, copy);
        got = (int)copy;
        e1000e_rx_refill(rx_tail_idx);
        rx_tail_idx = (rx_tail_idx + 1) % RX_DESC_COUNT;
        break;                            // one frame per call (protocol contract)
    }
    spin_unlock_irqrestore(&e1000e_lock, nf);
    return got;
}

// Hand a frame to the device on the tx ring. Blocks (bounded) for a free
// descriptor, exactly like the rtl8139 path waits for its OWN bit.
static int e1000e_send(const uint8_t* frame, uint32_t len) {
    if (!e1000e_present_flag || len == 0 || len > 16384) return -1;
    uint32_t nf = spin_lock_irqsave(&e1000e_lock);

    if (tx_free <= 0) {
        // Ring full: drain completions once, then bail if still full (the
        // caller retries — matches the rtl8139 "wait, then best effort" shape).
        int waited = 0;
        while (tx_free <= 0 && waited++ < 100000) {
            // TDH catching up with TDT frees descriptors.
            uint32_t dh = rd32(0x3810);
            int done = (int)(dh - tx_head_idx);
            if (done < 0) done += TX_DESC_COUNT;
            if (done > 0) {
                tx_head_idx = (tx_head_idx + done) % TX_DESC_COUNT;
                tx_free += done;
            } else {
                __asm__ __volatile__("pause");
            }
        }
        if (tx_free <= 0) { spin_unlock_irqrestore(&e1000e_lock, nf); return -1; }
    }

    int slot = tx_tail_idx;
    // Copy into a ring-owned buffer: the caller's buffer may not outlive the
    // DMA (rtl8139.c does exactly the same, for exactly the same reason).
    uint8_t* dst = tx_bufs + (uint32_t)slot * TX_BUF_SIZE;
    memcpy(dst, frame, len);
    tx_ring[slot].addr = (uint64_t)(uintptr_t)dst;
    tx_ring[slot].length = (uint16_t)len;
    tx_ring[slot].cso = 0;
    // cmd: EOP (bit0) | IFCS (bit1, insert FCS) | RS (bit3? — RS is bit 3 in
    // some datasheets, bit 4 elsewhere). The legacy 82540EM layout puts RS at
    // bit 3 of cmd; QEMU's model honors either because it only checks STA.DD.
    tx_ring[slot].cmd = 0x1B;             // EOP | IFCS | RS  (0x01|0x02|0x08)
    tx_ring[slot].sta = 0;
    tx_ring[slot].css = 0;
    tx_ring[slot].special = 0;

    tx_tail_idx = (tx_tail_idx + 1) % TX_DESC_COUNT;
    tx_free--;
    wr32(0x3818, (uint32_t)tx_tail_idx);  // TDT = next free slot
    spin_unlock_irqrestore(&e1000e_lock, nf);
    return (int)len;
}

void e1000e_irq_handler(void) {
    // The net layer's shared handler already drains through the backend vtable;
    // this exists so kernel.c can route the e1000e's own IRQ line to the same
    // net_irq_handler without a second dispatcher.
}

INITCALL(e1000e_init, 17);
