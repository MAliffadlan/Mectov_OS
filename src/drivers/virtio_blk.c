// ============================================================
// VirtIO-Blk driver — transitional (legacy PCI) block device, poll-only
// ============================================================
// One virtqueue (queue 0), single-command-at-a-time under vblk_lock with
// IRQs off, mirroring the AHCI poll model: no IRQ wiring, no wait queues.
// A 16KB bounce buffer keeps every descriptor address physically
// contiguous (the queue + bounce live in static .bss below 4GB, where the
// kernel's identity map makes address == physical, exactly like the AHCI
// port memory). Transfers are chunked to 32 sectors so the bounce stays
// small; callers still hand us whole ata_batch_limit() runs.
//
// Request layout per transfer: 16-byte header {type,reserved,sector} ->
// data (n*512) -> 1 status byte. Reads use VIRTIO_BLK_T_IN, writes
// VIRTIO_BLK_T_OUT followed by VIRTIO_BLK_T_FLUSH when the device offered
// VIRTIO_BLK_F_FLUSH at negotiate time (mirrors the IDE CACHE FLUSH the
// ATA path issues after every write).

#include "../include/virtio_blk.h"
#include "../include/virtio.h"
#include "../include/ata.h"      // ATA_BATCH_MAX, hdd_activity
#include "../include/pci.h"
#include "../include/serial.h"
#include "../include/utils.h"    // memcpy/memset
#include "../include/io.h"       // inb/outb/inw/outw/inl/outl
#include "../include/spinlock.h"

static spinlock_t vblk_lock = SPINLOCK_INIT;
static uint32_t vblk_eflags;

// ---- block-protocol constants ----
#define VBLK_T_IN       0
#define VBLK_T_OUT      1
#define VBLK_T_FLUSH    4
#define VBLK_F_FLUSH    (1u << 9)

#define VBLK_S_OK       0
#define VBLK_S_IOERR    1
#define VBLK_S_UNSUPP   2

#define VBLK_QSIZE_MAX  256         // largest QueueNum we lay out for
// Queue memory per device: desc[qsz] (16B each) + avail (6+2*qsz), then the
// used ring (6+8*qsz) on the next 4K boundary, then request + status.
// Worst case (qsz=256): 4096+518 -> used@8192 (2054B) -> req@10246. The 12KB
// buffer covers it; offsets are computed from the DEVICE's QueueNum because
// legacy PCI has no size register — the device derives every address from
// the size it reported.
#define VBLK_QMEM       12288
#define VBLK_ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define VBLK_CHUNK      32          // sectors per transfer (16KB bounce)
#define VBLK_POLL_TRIES 2000000

// ---- virtqueue layout (legacy, all guest memory) ----
typedef struct {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vq_desc_t;                        // 16 bytes

typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} vblk_req_t;                       // 16 bytes

typedef struct {
    int in_use;
    uint16_t iobase;
    uint16_t qsize;                 // device QueueNum (layout derived from it)
    uint32_t avail_off;             // 16*qsize
    uint32_t used_off;              // 4K-aligned past desc+avail
    uint32_t req_off;               // past the used ring
    uint32_t st_off;                // req_off + 16
    uint16_t last_used;             // poll cursor into the used ring
    uint64_t capacity;              // sectors (device config)
    int flush_ok;                   // VIRTIO_BLK_F_FLUSH negotiated
} vblk_dev_t;

static vblk_dev_t vblk_devs[VIRTIO_BLK_MAX];
static uint8_t vblk_qmem[VIRTIO_BLK_MAX][VBLK_QMEM] __attribute__((aligned(4096)));
static uint8_t vblk_bounce[VBLK_CHUNK * 512] __attribute__((aligned(4096)));

// Raw offset accessors (the ring sizes vary with the device QueueNum, so
// fixed C structs cannot describe them).
static vq_desc_t* vblk_desc(int d, int i) {
    return (vq_desc_t*)&vblk_qmem[d][(uint32_t)i * 16];
}
static uint16_t vblk_avail_idx(int d) {
    uint16_t v; memcpy(&v, &vblk_qmem[d][vblk_devs[d].avail_off + 2], 2); return v;
}
static void vblk_avail_set(int d, uint16_t idx, uint16_t head) {
    memcpy(&vblk_qmem[d][vblk_devs[d].avail_off + 4 + (uint32_t)idx * 2], &head, 2);
}
static void vblk_avail_bump(int d) {
    uint16_t v = vblk_avail_idx(d) + 1;
    memcpy(&vblk_qmem[d][vblk_devs[d].avail_off + 2], &v, 2);
}
static uint16_t vblk_used_idx(int d) {
    uint16_t v; memcpy(&v, &vblk_qmem[d][vblk_devs[d].used_off + 2], 2); return v;
}
static uint32_t vblk_used_id(int d, uint16_t idx) {
    uint32_t v;
    memcpy(&v, &vblk_qmem[d][vblk_devs[d].used_off + 4 + (uint32_t)idx * 8], 4);
    return v;
}
static vblk_req_t* vblk_req(int d) { return (vblk_req_t*)&vblk_qmem[d][vblk_devs[d].req_off]; }
static uint8_t* vblk_status(int d) { return &vblk_qmem[d][vblk_devs[d].st_off]; }

static void vblk_barrier(void) { __asm__ __volatile__("" ::: "memory"); }

// Bring up one legacy interface. Returns 0 with dev filled, or -1.
static int vblk_hw_init(vblk_dev_t* dev, uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t bar0 = pci_read(bus, slot, func, 0x10);
    if (!(bar0 & 1)) return -1;                 // legacy needs an I/O BAR
    uint16_t io = (uint16_t)(bar0 & ~3u);
    if (io == 0) return -1;
    dev->iobase = io;

    // Enable I/O space + bus mastering (same pattern as rtl8139).
    uint32_t cmd = pci_read(bus, slot, func, 0x04);
    pci_write(bus, slot, func, 0x04, cmd | 0x5);

    // Reset, then ACK + DRIVER.
    outb(io + VIRTIO_IO_STATUS, 0);
    outb(io + VIRTIO_IO_STATUS, VIRTIO_S_ACK);
    outb(io + VIRTIO_IO_STATUS, VIRTIO_S_ACK | VIRTIO_S_DRIVER);

    // Negotiate: FLUSH only when offered (we always accept a subset).
    uint32_t feat = inl(io + VIRTIO_IO_DEVICE_FEATS);
    uint32_t want = feat & VBLK_F_FLUSH;
    outl(io + VIRTIO_IO_GUEST_FEATS, want);
    outb(io + VIRTIO_IO_STATUS, VIRTIO_S_ACK | VIRTIO_S_DRIVER | VIRTIO_S_FEATURES_OK);
    if (!(inb(io + VIRTIO_IO_STATUS) & VIRTIO_S_FEATURES_OK)) return -1;
    dev->flush_ok = (want & VBLK_F_FLUSH) ? 1 : 0;

    // Queue 0: the layout MUST be derived from the device's QueueNum —
    // legacy PCI has no size register, so the device computes every ring
    // address from the size it reported. A smaller driver-side ring would
    // put avail/used where the device never looks.
    outw(io + VIRTIO_IO_QUEUE_SEL, 0);
    uint16_t qsz = inw(io + VIRTIO_IO_QUEUE_NUM);
    if (qsz < 8 || qsz > VBLK_QSIZE_MAX) return -1;
    int d = (int)(dev - vblk_devs);
    dev->qsize = qsz;
    dev->avail_off = (uint32_t)qsz * 16;
    dev->used_off = VBLK_ALIGN_UP(dev->avail_off + 6 + (uint32_t)qsz * 2, 4096);
    dev->req_off = dev->used_off + 6 + (uint32_t)qsz * 8;
    dev->st_off = dev->req_off + 16;
    if (dev->st_off + 1 > VBLK_QMEM) return -1;
    memset(vblk_qmem[d], 0, VBLK_QMEM);
    // avail.flags = NO_INTERRUPT (poll-only: the device never asserts INTx).
    uint16_t noint = VIRTQ_AVAIL_F_NO_INTERRUPT;
    memcpy(&vblk_qmem[d][dev->avail_off], &noint, 2);
    vblk_barrier();
    uint32_t pfn = (uint32_t)(uintptr_t)vblk_qmem[d] >> 12;
    if (pfn == 0) return -1;
    outl(io + VIRTIO_IO_QUEUE_PFN, pfn);
    dev->last_used = 0;

    // Capacity (sectors, 512-byte units per the legacy default BLK_SIZE).
    uint32_t cap_lo = inl(io + VIRTIO_IO_CONFIG);
    uint32_t cap_hi = inl(io + VIRTIO_IO_CONFIG + 4);
    dev->capacity = ((uint64_t)cap_hi << 32) | cap_lo;

    (void)inb(io + VIRTIO_IO_ISR);              // ack any stale interrupt
    outb(io + VIRTIO_IO_STATUS,
         VIRTIO_S_ACK | VIRTIO_S_DRIVER | VIRTIO_S_FEATURES_OK | VIRTIO_S_DRIVER_OK);
    (void)inb(io + VIRTIO_IO_ISR);
    return 0;
}

void virtio_blk_init(void) {
    for (int i = 0; i < VIRTIO_BLK_MAX; i++) vblk_devs[i].in_use = 0;
    int n = 0;
    for (int i = 0; i < pci_device_count && n < VIRTIO_BLK_MAX; i++) {
        pci_device_t* d = &pci_devices[i];
        if (d->vendor_id != VIRTIO_PCI_VENDOR) continue;
        if (d->device_id != VIRTIO_PCI_DEV_BLK_LEGACY) continue;  // modern (0x104x): skip
        if (vblk_hw_init(&vblk_devs[n], d->bus, d->slot, d->func) != 0) {
            write_serial_string("[VIRTIO] blk init failed\n");
            continue;
        }
        write_serial_string("[VIRTIO] blk ");
        write_serial_hex((uint32_t)n);
        write_serial_string(" io=");
        write_serial_hex((uint32_t)vblk_devs[n].iobase);
        write_serial_string(" cap=");
        write_serial_hex((uint32_t)(vblk_devs[n].capacity & 0xFFFFFFFFu));
        write_serial_string(" sectors -> drive ");
        write_serial_hex((uint32_t)(VIRTIO_BLK_BASE + n));
        write_serial_string("\n");
        vblk_devs[n].in_use = 1;
        n++;
    }
    if (n > 0) write_serial_string("[VIRTIO] ready\n");
    else write_serial_string("[VIRTIO] no legacy blk device (PCI)\n");
}

int virtio_blk_present(void) {
    for (int i = 0; i < VIRTIO_BLK_MAX; i++)
        if (vblk_devs[i].in_use) return 1;
    return 0;
}

// Fire one descriptor chain and poll the used ring. head is always 0
// (single-command-at-a-time). Returns the status byte or -1 on timeout.
static int vblk_poll(int d) {
    vblk_dev_t* dev = &vblk_devs[d];
    vblk_barrier();
    outw(dev->iobase + VIRTIO_IO_QUEUE_NOTIFY, 0);
    // The memcpy inside vblk_used_idx() is an opaque call, so the compiler
    // reloads the index every iteration — the poll cannot hoist into an
    // infinite loop even though the ring is plain (cache-coherent) .bss.
    int t = VBLK_POLL_TRIES;
    while (--t > 0) {
        if (vblk_used_idx(d) != dev->last_used) break;
    }
    if (t == 0) return -1;
    vblk_barrier();
    if (vblk_used_id(d, dev->last_used % dev->qsize) != 0) return -1;
    dev->last_used++;
    (void)inb(dev->iobase + VIRTIO_IO_ISR);     // keep the INTx line quiet
    return (int)*vblk_status(d);
}

// One chunk (<= VBLK_CHUNK sectors) of a read or write. is_write = OUT.
static int vblk_chunk(int d, int is_write, uint64_t sector, int nsec) {
    vblk_dev_t* dev = &vblk_devs[d];
    vblk_req_t* req = vblk_req(d);
    req->type = is_write ? VBLK_T_OUT : VBLK_T_IN;
    req->reserved = 0;
    req->sector = sector;
    *vblk_status(d) = 0xFF;

    uint32_t req_phys = (uint32_t)(uintptr_t)req;
    uint32_t dat_phys = (uint32_t)(uintptr_t)vblk_bounce;
    uint32_t st_phys = (uint32_t)(uintptr_t)vblk_status(d);

    vblk_desc(d, 0)->addr_lo = req_phys; vblk_desc(d, 0)->addr_hi = 0;
    vblk_desc(d, 0)->len = sizeof(vblk_req_t);
    vblk_desc(d, 0)->flags = VIRTQ_DESC_F_NEXT; vblk_desc(d, 0)->next = 1;
    vblk_desc(d, 1)->addr_lo = dat_phys; vblk_desc(d, 1)->addr_hi = 0;
    vblk_desc(d, 1)->len = (uint32_t)(nsec * 512);
    vblk_desc(d, 1)->flags = VIRTQ_DESC_F_NEXT | (is_write ? 0 : VIRTQ_DESC_F_WRITE);
    vblk_desc(d, 1)->next = 2;
    vblk_desc(d, 2)->addr_lo = st_phys; vblk_desc(d, 2)->addr_hi = 0;
    vblk_desc(d, 2)->len = 1;
    vblk_desc(d, 2)->flags = VIRTQ_DESC_F_WRITE; vblk_desc(d, 2)->next = 0;

    vblk_avail_set(d, vblk_avail_idx(d) % dev->qsize, 0);
    vblk_barrier();
    vblk_avail_bump(d);
    vblk_barrier();
    int st = vblk_poll(d);
    if (st != VBLK_S_OK) return -1;
    return 0;
}

static int vblk_flush(int d) {
    vblk_dev_t* dev = &vblk_devs[d];
    vblk_req_t* req = vblk_req(d);
    req->type = VBLK_T_FLUSH;
    req->reserved = 0;
    req->sector = 0;
    *vblk_status(d) = 0xFF;

    vblk_desc(d, 0)->addr_lo = (uint32_t)(uintptr_t)req;
    vblk_desc(d, 0)->addr_hi = 0;
    vblk_desc(d, 0)->len = sizeof(vblk_req_t);
    vblk_desc(d, 0)->flags = VIRTQ_DESC_F_NEXT; vblk_desc(d, 0)->next = 1;
    vblk_desc(d, 1)->addr_lo = (uint32_t)(uintptr_t)vblk_status(d);
    vblk_desc(d, 1)->addr_hi = 0;
    vblk_desc(d, 1)->len = 1;
    vblk_desc(d, 1)->flags = VIRTQ_DESC_F_WRITE; vblk_desc(d, 1)->next = 0;

    vblk_avail_set(d, vblk_avail_idx(d) % dev->qsize, 0);
    vblk_barrier();
    vblk_avail_bump(d);
    vblk_barrier();
    return (vblk_poll(d) == VBLK_S_OK) ? 0 : -1;
}

static int vblk_valid(int drive) {
    int d = drive - VIRTIO_BLK_BASE;
    if (d < 0 || d >= VIRTIO_BLK_MAX) return -1;
    if (!vblk_devs[d].in_use) return -1;
    return d;
}

int virtio_blk_read_sectors(int drive, uint32_t lba, int count, uint8_t* buf) {
    int d = vblk_valid(drive);
    if (d < 0 || !buf || count < 1) return -1;
    extern volatile int hdd_activity;
    vblk_eflags = spin_lock_irqsave(&vblk_lock);
    hdd_activity = 10;
    uint64_t sec = lba;
    int left = count;
    int rc = 0;
    while (left > 0) {
        int n = (left > VBLK_CHUNK) ? VBLK_CHUNK : left;
        if (vblk_chunk(d, 0, sec, n) != 0) { rc = -1; break; }
        memcpy(buf, vblk_bounce, (uint32_t)(n * 512));
        buf += n * 512;
        sec += (uint64_t)n;
        left -= n;
    }
    spin_unlock_irqrestore(&vblk_lock, vblk_eflags);
    return rc;
}

int virtio_blk_write_sectors(int drive, uint32_t lba, int count, const uint8_t* buf) {
    int d = vblk_valid(drive);
    if (d < 0 || !buf || count < 1) return -1;
    extern volatile int hdd_activity;
    vblk_eflags = spin_lock_irqsave(&vblk_lock);
    hdd_activity = 10;
    uint64_t sec = lba;
    int left = count;
    int rc = 0;
    while (left > 0) {
        int n = (left > VBLK_CHUNK) ? VBLK_CHUNK : left;
        memcpy(vblk_bounce, buf, (uint32_t)(n * 512));
        if (vblk_chunk(d, 1, sec, n) != 0) { rc = -1; break; }
        buf += n * 512;
        sec += (uint64_t)n;
        left -= n;
    }
    // Durability parity with the IDE CACHE FLUSH the ATA path issues
    // after every write (v38.26): only when negotiated, only on success.
    if (rc == 0 && vblk_devs[d].flush_ok && vblk_flush(d) != 0) rc = -1;
    spin_unlock_irqrestore(&vblk_lock, vblk_eflags);
    return rc;
}
