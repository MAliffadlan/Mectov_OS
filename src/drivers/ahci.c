// ============================================================
// AHCI / SATA driver (v38.50)
// ============================================================
// Intel-style AHCI: the HBA is a PCI BAR5 memory region; each port owns a
// command list (32 headers) the driver fills, the device fetches the command
// FIS + PRDT from a command table, and data lands straight in the PRD's
// physical regions — no port I/O in the data path at all.
//
// Model kept deliberately small and QEMU-real:
//   * command slot 0 only (we are single-command-at-a-time anyway),
//   * one PRD entry into a 64K-aligned static bounce buffer,
//   * completion by polling PxCI (mirrors the IDE DMA polling under
//     ata_lock; the OS has no per-port wait queue yet),
//   * LBA48 READ/WRITE DMA EXT opcodes (0x25/0x35).
//
// The PCI MMIO window (0xFE000000..0xFF000000, mem.c) covers BAR5 at its
// natural address, so register access is a plain volatile pointer.

#include "../include/ahci.h"
#include "../include/ata.h"      // ATA_BATCH_MAX, ata_batch_limit
#include "../include/pci.h"
#include "../include/mem.h"      // page_map for the BAR
#include "../include/serial.h"
#include "../include/utils.h"
#include "../include/spinlock.h"

static spinlock_t ahci_lock = SPINLOCK_INIT;
static uint32_t ahci_eflags;

// ---- HBA / port register offsets (bytes from BAR5) ----
#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_PI  0x0C
#define PX_REGS 0x100
#define PX_STRIDE 0x80
#define PX_CLB  0x00
#define PX_FB   0x08
#define PX_IS   0x10
#define PX_IE   0x14
#define PX_CMD  0x18
#define PX_TFD  0x20
#define PX_SIG  0x24
#define PX_SSTS 0x28
#define PX_SERR 0x30
#define PX_CI   0x38

#define GHC_AE   (1u << 31)
#define CMD_ST   (1u << 0)
#define CMD_SUD  (1u << 1)
#define CMD_FRE  (1u << 4)
#define CMD_CR   (1u << 15)
#define CMD_FR   (1u << 14)
#define IS_TFES (1u << 30)      // task file error
#define SSTS_DET (0x3)          // device detection: 3 = phy comm est.

static volatile uint32_t* ahci_mmio = 0;   // BAR5 kernel VA (identity)
static int ahci_ports[AHCI_MAX_PORTS];     // -1 unused, else PHYSICAL port index
static int ahci_ndrives = 0;

// ---- per-port DMA structures (physical, identity-mapped, aligned) ----
typedef struct {
    uint16_t flags;      // bits0-4 CFLBA(dwords), bit6 W, bit10 C
    uint16_t prdtl;      // PRDT entry count
    uint32_t prdbc;      // PRD byte count — device-written STATUS field that
                         // still OCCUPIES DW1: ctba lives at offset 8. (The
                         // bring-up bug: omitting this field put ctba in the
                         // PRDBC slot and the HBA read tbl_addr=0.)
    uint32_t ctba;       // command table base (128-aligned)
    uint32_t ctbau;
    uint32_t reserved[4];
} __attribute__((packed)) ahci_cmd_header_t;

typedef struct {
    uint32_t dba;        // data base
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc_i;      // bits0-21: byte count - 1; bit31: int on completion
} __attribute__((packed)) ahci_prdt_entry_t;

typedef struct {
    uint8_t  cfis[64];   // H2D FIS area
    uint8_t  acmd[16];
    uint8_t  reserved[48];
    ahci_prdt_entry_t prdt[1];
} __attribute__((packed, aligned(128))) ahci_cmd_table_t;

// One 4KB page per structure per port keeps every alignment rule trivially
// true (list 1K, FIS 256B, table 128B) and costs 12 KB per port.
typedef struct {
    ahci_cmd_header_t clist[32] __attribute__((aligned(1024)));
    uint8_t rfis[256] __attribute__((aligned(256)));
    ahci_cmd_table_t table __attribute__((aligned(4096)));
} ahci_port_mem_t;

static ahci_port_mem_t ahci_mem[AHCI_MAX_PORTS] __attribute__((aligned(4096)));
// Bounce buffer shared by all ports (the ahci_lock serialises users) —
// 64K-aligned so one PRD entry never needs splitting.
static uint8_t ahci_bounce[65536] __attribute__((aligned(65536)));

static volatile uint32_t* px(int port, int reg) {
    return (volatile uint32_t*)((uint8_t*)ahci_mmio + PX_REGS + port * PX_STRIDE + reg);
}

// Bring one port's engine up: stop, hand it our command list + FIS receive
// areas, clear latched errors, start. Returns 0 or -1.
static int ahci_port_start(int port) {
    volatile uint32_t* cmd = px(port, PX_CMD);

    // Stop the engine (ST+FRE clear, wait CR/FR to drop) before touching
    // the list addresses — writing CLB while running is undefined.
    *cmd &= ~(CMD_ST | CMD_FRE);
    int t = 200000;
    while ((*cmd & (CMD_CR | CMD_FR)) && --t > 0);
    if (t == 0) {
        write_serial_string("[AHCI] port ");
        write_serial_hex((uint32_t)port);
        write_serial_string(" engine never stopped\n");
        return -1;
    }

    uint32_t mem_phys = (uint32_t)(uintptr_t)&ahci_mem[port];
    // CLB/FB are 64-bit (low reg + upper reg): clear the upper halves
    // explicitly — BIOS leftovers there make the HBA fetch the command list
    // from a garbage 64-bit address and PxCI hangs with no error bits.
    *px(port, PX_CLB) = mem_phys;                                   // 1K-aligned
    *px(port, PX_CLB + 4) = 0;
    *px(port, PX_FB)  = mem_phys + 1024;                            // 256-aligned
    *px(port, PX_FB + 4) = 0;
    // Clear every latched port interrupt + error, then arm the receiver.
    *px(port, PX_IS)   = 0xFFFFFFFF;
    *px(port, PX_SERR) = 0xFFFFFFFF;
    *px(port, PX_IE)   = 0;          // poll mode, like the IDE DMA path
    *cmd |= CMD_SUD | CMD_FRE | CMD_ST;
    return 0;
}

void ahci_init(void) {
    for (int i = 0; i < AHCI_MAX_PORTS; i++) ahci_ports[i] = -1;

    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t* d = &pci_devices[i];
        // Mass storage (01) / SATA (06) / AHCI vendor-specific IF (01).
        if (d->class_code != 0x01 || d->subclass != 0x06 || d->prog_if != 0x01) continue;

        uint32_t bar5 = pci_read(d->bus, d->slot, d->func, 0x24);
        if (bar5 & 1) continue;                     // must be memory space
        uint32_t base = bar5 & ~0xF;
        if (base < 0xFE000000 || base >= 0xFF000000) {
            // Outside the static MMIO window (unexpected on QEMU i440fx);
            // page_map() covers the whole window, so this is a bail-out.
            write_serial_string("[AHCI] BAR5 outside the MMIO window: ");
            write_serial_hex(base);
            write_serial_string("\n");
            return;
        }
        // The window maps the region from boot; make the mapping explicit
        // (idempotent), keeping the window's strong-uncached PCD|PWT flags
        // (PAGE_PRESENT|PAGE_RW alone would make the BAR page cacheable).
        page_map(base, base, PAGE_PRESENT | PAGE_RW | 0x18);
        uint32_t pcicmd = pci_read(d->bus, d->slot, d->func, 0x04);
        pci_write(d->bus, d->slot, d->func, 0x04, pcicmd | 0x6);

        ahci_mmio = (volatile uint32_t*)(uintptr_t)base;
        // AHCI mode on (must precede everything else on real HW).
        *(volatile uint32_t*)((uint8_t*)ahci_mmio + HBA_GHC) |= GHC_AE;
        uint32_t pi = *(volatile uint32_t*)((uint8_t*)ahci_mmio + HBA_PI);

        write_serial_string("[AHCI] controller @ ");
        write_serial_hex(base);
        write_serial_string(" PI=");
        write_serial_hex(pi);
        write_serial_string("\n");

        for (int p = 0; p < 32 && ahci_ndrives < AHCI_MAX_PORTS; p++) {
            if (!(pi & (1u << p))) continue;
            uint32_t ssts = *px(p, PX_SSTS);
            if ((ssts & SSTS_DET) != SSTS_DET) continue;   // nothing plugged
            uint32_t sig = *px(p, PX_SIG);
            // ATAPI (optical) signature 0xEB140101 — skip non-ATA devices.
            if ((sig >> 16) == 0xEB14) continue;
            if (ahci_port_start(p) < 0) continue;

            ahci_ports[ahci_ndrives] = p;   // remember the PHYSICAL port
            write_serial_string("[AHCI] port ");
            write_serial_hex((uint32_t)p);
            write_serial_string(" SATA sig=");
            write_serial_hex(sig);
            write_serial_string(" -> drive ");
            write_serial_hex((uint32_t)(AHCI_DRIVE_BASE + ahci_ndrives));
            write_serial_string("\n");
            ahci_ndrives++;
        }
        write_serial_string(ahci_ndrives ? "[AHCI] ready\n"
                                         : "[AHCI] no disks found\n");

        return;
    }
    write_serial_string("[AHCI] no AHCI controller (PCI)\n");
}

int ahci_present(void) { return ahci_ndrives > 0; }

// ---- command issue ----

// Build + fire one command on slot 0 of `port`. buf is the bounce buffer's
// KERNEL address; len is count*512. Returns 0 on success.
static int ahci_issue(int port, int is_write, uint32_t lba, int count,
                      uint8_t* kbuf) {
    ahci_port_mem_t* m = &ahci_mem[port];
    int len = count * 512;

    // Command header, slot 0: FIS is 5 dwords; C clears BSY on R_OK; W for
    // writes; one PRD entry into the bounce buffer.
    m->clist[0].flags = (uint16_t)(5 | (1u << 10) | (is_write ? (1u << 6) : 0));
    m->clist[0].prdtl = 1;
    m->clist[0].ctba = (uint32_t)(uintptr_t)&m->table;
    m->clist[0].ctbau = 0;

    // H2D FIS: type 0x27, C=1 (update the control registers), LBA48 fields.
    // Byte layout per the Serial ATA spec: [3] features, [4] sector count,
    // [5..7] LBA 23:0, [8] device, [9..11] LBA 47:24, [15] control.
    uint8_t* f = m->table.cfis;
    memset(f, 0, 64);
    f[0] = 0x27;                     // H2D register FIS
    f[1] = 0x80;                     // C=1 (update the shadow registers)
    f[2] = is_write ? 0x35 : 0x25;   // WRITE/READ DMA EXT
    // Field offsets per the SATA H2D FIS layout (NOT the ATA task-file
    // register order): LBA 23:0 at [4..6], device at [7], LBA 47:24 at
    // [8..10], count 7:0 at [12], count 15:8 at [13]. A one-byte shift
    // here used to put count into the LBA slot and device into LBA 31:24,
    // making the device read LBA 0x40000001 x 65536 sectors (all zeros).
    f[4] = (uint8_t)lba;             // LBA 7:0
    f[5] = (uint8_t)(lba >> 8);      // LBA 15:8
    f[6] = (uint8_t)(lba >> 16);     // LBA 23:16
    f[7] = 0x40;                     // device: LBA
    f[8] = (uint8_t)(lba >> 24);     // LBA 31:24
    f[9] = 0;                        // LBA 39:32
    f[10] = 0;                       // LBA 47:40
    f[12] = (uint8_t)count;          // sector count 7:0 (<= 128, fits)
    // [11] features high, [13] count 15:8, [14] ICC, [15] control: 0

    // One PRD entry: the 64K-aligned bounce buffer never crosses a boundary
    // and 128 sectors (64K) fits a single region.
    m->table.prdt[0].dba = (uint32_t)(uintptr_t)kbuf;
    m->table.prdt[0].dbau = 0;
    m->table.prdt[0].reserved = 0;
    m->table.prdt[0].dbc_i = ((uint32_t)len - 1) | (1u << 31);

    // Clear latched state, then issue slot 0 and poll for its completion.
    *px(port, PX_IS) = 0xFFFFFFFF;
    *px(port, PX_SERR) = 0xFFFFFFFF;
    // The port must be idle (BSY/DRQ clear) before the HBA will fetch the
    // command — a stuck task file makes PxCI hang forever.
    int idle = 200000;
    while ((*px(port, PX_TFD) & 0x88) && --idle > 0);
    if (idle == 0) {
        write_serial_string("[AHCI] port not idle before issue (TFD=");
        write_serial_hex(*px(port, PX_TFD));
        write_serial_string(")\n");
        return -1;
    }
    *px(port, PX_CI) = 1;

    int t = 5000000;
    while (--t > 0) {
        uint32_t ci = *px(port, PX_CI);
        uint32_t is = *px(port, PX_IS);
        if (is & IS_TFES) break;      // device error
        if ((ci & 1) == 0) break;     // slot finished
    }
    uint32_t is = *px(port, PX_IS);
    if (t == 0 || (is & IS_TFES) || (*px(port, PX_CI) & 1)) {
        write_serial_string("[AHCI] transfer error port ");
        write_serial_hex((uint32_t)port);
        write_serial_string(" IS=");
        write_serial_hex(is);
        write_serial_string("\n");
        // Best-effort recovery: clear errors and re-issue ST (the engine can
        // stall after a task-file error until restarted).
        *px(port, PX_IS) = 0xFFFFFFFF;
        *px(port, PX_SERR) = 0xFFFFFFFF;
        *px(port, PX_CMD) |= CMD_ST;
        return -1;
    }
    return 0;
}

// ---- public sector API (drive = AHCI_DRIVE_BASE + port-slot) ----

int ahci_read_sectors(int drive, uint32_t lba, int count, uint8_t* buf) {
    int slot = drive - AHCI_DRIVE_BASE;
    if (!ahci_mmio || slot < 0 || slot >= AHCI_MAX_PORTS || ahci_ports[slot] < 0)
        return -1;
    int port = ahci_ports[slot];   // slot index -> PHYSICAL port
    if (count < 1) return -1;
    count = ata_batch_limit(lba, count);

    ahci_eflags = spin_lock_irqsave(&ahci_lock);
    hdd_activity = 10;
    int rc = ahci_issue(port, 0, lba, count, ahci_bounce);
    if (rc == 0) memcpy(buf, ahci_bounce, (uint32_t)count * 512);
    spin_unlock_irqrestore(&ahci_lock, ahci_eflags);
    return rc;
}

int ahci_write_sectors(int drive, uint32_t lba, int count, const uint8_t* buf) {
    int slot = drive - AHCI_DRIVE_BASE;
    if (!ahci_mmio || slot < 0 || slot >= AHCI_MAX_PORTS || ahci_ports[slot] < 0)
        return -1;
    int port = ahci_ports[slot];   // slot index -> PHYSICAL port
    if (count < 1) return -1;
    count = ata_batch_limit(lba, count);

    ahci_eflags = spin_lock_irqsave(&ahci_lock);
    hdd_activity = 10;
    memcpy(ahci_bounce, buf, (uint32_t)count * 512);
    int rc = ahci_issue(port, 1, lba, count, ahci_bounce);
    spin_unlock_irqrestore(&ahci_lock, ahci_eflags);
    return rc;
}
