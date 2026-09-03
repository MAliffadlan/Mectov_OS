// ============================================================
// xHCI host controller + USB 3.0 mass-storage driver (v38.56)
// ============================================================
// USB 3.0 support on the existing drive/VFS stack. The xHCI host
// controller (PCI 0C/03/30, 64-bit BAR0 in the identity-mapped MMIO
// window) is brought up QEMU-real, following the AHCI model (v38.50):
//   * static .bss DMA structures (identity-mapped, aligned) — DCBAA,
//     command ring, event ring + ERST, per-device contexts + endpoint
//     rings, one 64K-aligned bounce buffer;
//   * everything under xhci_lock with IRQs off, completion by POLLING
//     the event-ring cycle bit (the kernel has no MSI support and the
//     IOAPIC masks unknown GSI lines — AHCI polls for the same reason);
//   * mass-storage devices register as drives 8+ on the ATA sector
//     API, so ext2/FAT32/pcache and `mount` work unchanged.
//
// Protocol layers, bottom-up:
//   xHC init      halt+reset, DCBAA, command ring (CRCR), event ring +
//                 ERST (interrupter 0), MaxSlotsEn, RUN
//   commands      Enable Slot, Address Device, Configure Endpoint
//   transfers    control (Setup/Data/Status TRBs on EP0) and Normal
//                 TRBs on the bulk rings, doorbell, event-ring poll
//   enumeration  port reset -> device/config descriptors -> MSC
//                 interface (class 8 / subclass 6 / BOT) -> bulk EPs
//   mass-storage  BOT: CBW -> data -> CSW around SCSI INQUIRY /
//                 TEST UNIT READY / READ CAPACITY(10) / READ(10) /
//                 WRITE(10)
//
// Every register/TRB/context bit position below is verified against the
// actual hardware this driver runs on: QEMU 8.2.2's hw/usb/hcd-xhci.c
// (which implements the xHCI 1.x spec):
//   * TRB control: cycle=0, TC(link)=1, ISP=2, chain=4, IOC=5, IDT=6,
//     type=10..15, DIR=16 (data/normal TRBs), TRT=16..17 (setup TRB,
//     not parsed by QEMU — direction comes from bmRequestType),
//     slot id=24..31 (commands), EP id=16..20 (events)
//   * TRB status: transfer length = bits 0..16, interrupter target =
//     bits 22..31 (MUST stay 0 or events land on the wrong interrupter);
//     event TRBs carry the completion code in bits 24..31
//   * input context: dword 0 = DROP flags, dword 1 = ADD flags,
//     slot context at +32, endpoint context i (DCI) at +32+32*i
//   * slot context: route string = dword 0 nibbles, root port =
//     dword 1 bits 16..23, context entries = dword 0 bits 27..31
//   * endpoint context: EP type = dword 1 bits 3..5 (2=bulk out,
//     6=bulk in, 4=control), max packet = dword 1 bits 16..31,
//     transfer-ring dequeue = dwords 2/3 with the DCS cycle bit 0
//   * QEMU's model reports HCSPARAMS2 = 0xF -> MaxScratchpadBufs = 0,
//     assigns the USB device address = slot id itself during Address
//     Device, and needs the output-context pointer in DCBAA[slot]
//     valid BEFORE the Address Device command (it reads it once there
//     and caches it for the slot's lifetime).

#include "../include/xhci.h"
#include "../include/ata.h"       // ata_batch_limit
#include "../include/pci.h"
#include "../include/mem.h"       // page_map for the BAR
#include "../include/serial.h"
#include "../include/utils.h"    // memcpy/memset
#include "../include/spinlock.h"

static spinlock_t xhci_lock = SPINLOCK_INIT;
static uint32_t xhci_eflags;
// Event-ring trace (type/slot/epid/cc per consumed event) — on during
// bring-up + enumeration only, off before the data path starts. Every
// CBW/data/CSW phase consumes an event; printing them for real I/O would
// drown the serial log and stall sector transfers behind polled writes.
static int xhci_evt_trace = 1;

// ---- capability registers (at BAR0) ----
#define XHCI_CAP_CAPLENGTH 0x00  // byte 0: op-base; bytes 2-3: HCIVERSION
#define XHCI_CAP_HCSPARAMS1 0x04 // slots | intrs<<8 | ports<<24
#define XHCI_CAP_HCSPARAMS2 0x08 // MaxPSASize=0xF on QEMU, scratchpad bits 0
#define XHCI_CAP_DBOFF      0x14 // doorbell array offset (QEMU: 0x2000)
#define XHCI_CAP_RTSOFF     0x18 // runtime base offset (QEMU: 0x1000)

// ---- operational registers (base = CAPLENGTH) ----
#define XHCI_OP_USBCMD 0x00
#define XHCI_OP_USBSTS 0x04
#define XHCI_OP_CRCR   0x18      // 64-bit: low then high
#define XHCI_OP_DCBAAP 0x30      // 64-bit: low then high
#define XHCI_OP_CONFIG 0x38      // MaxSlotsEn = bits 7:0

#define USBCMD_RUN   (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBSTS_HCH   (1u << 0)
#define USBSTS_CNR   (1u << 11)

// Ports live at op + 0x400 + 0x10*(port-1), 1-based port number.
#define XHCI_PORT_BASE   0x400
#define XHCI_PORT_STRIDE 0x10

// PORTSC bits (verified against QEMU's xhci_port_update/xhci_port_write)
#define PORTSC_CCS  (1u << 0)    // current connect status
#define PORTSC_PED  (1u << 1)    // port enabled (SS devices attach enabled)
#define PORTSC_PR   (1u << 4)    // port reset (w1tostart, self-clearing)
#define PORTSC_PP   (1u << 9)    // port power (read/write — preserve it!)
#define PORTSC_SPEED_MASK (0xFu << 10)
#define PORTSC_CSC  (1u << 17)   // change bits below are WRITE-1-TO-CLEAR
#define PORTSC_PEC  (1u << 18)
#define PORTSC_WRC  (1u << 19)
#define PORTSC_OCC  (1u << 20)
#define PORTSC_PRC  (1u << 21)   // port reset change
#define PORTSC_PLC  (1u << 22)
#define PORTSC_CEC  (1u << 23)
#define PORTSC_ALL_CHANGES (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | \
                            PORTSC_OCC | PORTSC_PRC | PORTSC_PLC | PORTSC_CEC)

#define SPEED_HIGH   3
#define SPEED_SUPER  4
#define EP0_MPS_SUPER 512
#define EP0_MPS_HIGH  64
#define BULK_MPS_SUPER 1024
#define BULK_MPS_HIGH  512

// ---- runtime / interrupter registers (base = RTSOFF) ----
// interrupter 0 registers sit at RTSOFF + 0x20.
#define XHCI_IR0      0x20
#define XHCI_IR_IMAN  0x00      // bit 1: IE (we keep 0 — poll model)
#define XHCI_IR_ERSTSZ 0x08
#define XHCI_IR_ERSTBA 0x10     // 64-bit: low then high
#define XHCI_IR_ERDP  0x18      // 64-bit: low then high; bit 3 = EHB
#define ERDP_EHB (1u << 3)

// ---- TRB (16 bytes) ----
// [0..7]   parameter (64-bit, type-specific)
// [8..11]  status:  bits 0..16 transfer length, bits 22..31 interrupter
//                  target (keep 0), event TRBs: completion code bits 24..31
// [12..15] control: cycle(0) TC(1) ISP(2) chain(4) IOC(5) IDT(6)
//                   type(10..15) DIR(16) TRT(16..17) epid(16..20)
//                   slot id(24..31)
typedef struct {
    uint32_t param;       // parameter low
    uint32_t param_hi;    // parameter high
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) trb_t;

#define TRB_CYCLE    (1u << 0)
#define TRB_TC       (1u << 1)   // link TRB: toggle cycle on wrap
#define TRB_ISP      (1u << 2)
#define TRB_CHAIN    (1u << 4)
#define TRB_IOC      (1u << 5)
#define TRB_IDT      (1u << 6)
#define TRB_TYPE_SHIFT 10
#define TRB_TYPE(t)  (((uint32_t)(t) << TRB_TYPE_SHIFT) & 0x0000FC00u)
#define TRB_DIR_IN   (1u << 16)   // data-stage + normal TRBs (QEMU checks!)
#define TRB_TRT_OUT  (2u << 16)   // setup TRB transfer type (not parsed by QEMU)
#define TRB_TRT_IN   (3u << 16)
#define TRB_SLOT_ID(id) (((uint32_t)(id) & 0xFFu) << 24)

#define TRB_LEN(l)     ((uint32_t)(l) & 0x1FFFFu)
// Event TRB completion code: status bits 24..31.
#define TRB_CC(t)      (((t).status >> 24) & 0xFFu)
#define TRB_EPID(t)    ((((t).control) >> 16) & 0x1Fu)
#define TRB_SLOT(t)    ((((t).control) >> 24) & 0xFFu)

// TRB types (xHCI spec table 6-4 / QEMU TR_* numbering)
#define TRB_NORMAL        1
#define TRB_SETUP_STAGE   2
#define TRB_DATA_STAGE    3
#define TRB_STATUS_STAGE  4
#define TRB_LINK          6
#define TRB_ENABLE_SLOT   9
#define TRB_ADDR_DEV     11
#define TRB_CFG_EP       12
#define TRB_NO_OP        23
#define TRB_TRANSFER_EVENT 32
#define TRB_CMD_COMPLETION 33
#define TRB_PORT_STATUS    34

// Address Device command TRB: bit 9 = BSR (block set address request).
// BSR=0 makes the xHC assign the USB address (QEMU: address = slot id).
#define TRB_BSR (1u << 9)

// completion codes
#define CC_SUCCESS        1
#define CC_TRB_ERROR      5
#define CC_STALL_ERROR    6
#define CC_SHORT_PACKET  13
#define CC_NO_SLOTS      11

// ---- contexts ----
// Input context: dword 0 = DROP flags (D0..D31), dword 1 = ADD flags
// (A0..A31); slot context at +32; endpoint context i at +32+32*i.
// Endpoint context: EP type = dword 1 bits 3..5; max packet = dword 1
// bits 16..31; transfer-ring dequeue pointer = dwords 2/3 with the DCS
// cycle-state in dword 2 bit 0.
#define ICTX_DROP 0
#define ICTX_ADD  1
#define SLOT_OFF      32          // slot context offset in the input ctx
#define EPCTX_OFF(dci) (32 + 32 * (dci))   // DCI 1 = EP0
#define ADD_FLAG(bit) (1u << (bit))

#define EP_TYPE_CTRL 4
#define EP_TYPE_BULK_OUT 2
#define EP_TYPE_BULK_IN  6

// slot context dword 0: route string = bits 19:0 (nibbles = hub port numbers
// along the path from the root; a device DIRECTLY on a root port has an
// EMPTY route string = 0 — the root port itself lives in dword 1!),
// context entries = bits 31:27.
#define SLOT_CTX_ENTRIES(n) (((uint32_t)(n) & 0x1Fu) << 27)
// slot context dword 1: root hub port number = bits 23:16.
#define SLOT_ROOT_PORT(p) (((uint32_t)(p) & 0xFFu) << 16)

// endpoint context helpers: dword 1 packs EP type + max packet size.
#define EPCTX1(type, mps) (((uint32_t)(type) & 0x7u) << 3 | \
                           ((uint32_t)(mps) & 0xFFFFu) << 16)
// dword 2/3: ring base | DCS (cycle state bit 0 — 1 for a fresh ring).
#define EPCTX_DEQ(ring_addr) ((uint32_t)(uintptr_t)(ring_addr) | 1u)

// ---- sizes ----
#define XHCI_RING_TRBS 256          // TRBs per ring (4KB per ring)
#define XHCI_MAX_PORTS 16
#define XHCI_MAX_SLOTS 16
#define ICTX_DWORDS   (8 + 8 + 8 * 8)   // ICC + slot + EP ctxs up to DCI 8
#define DCTX_DWORDS   (8 + 8 * 8)       // slot + EP ctxs up to DCI 8
#define USB_DEV_DESC_LEN  18
#define USB_CONFIG_MAX    512
#define BOT_CBW_LEN 31
#define BOT_CSW_LEN 13
#define CBW_SIG 0x43425355u          // 'USBC' little-endian
#define CSW_SIG 0x53425355u          // 'USBS' little-endian

// A transfer ring. The Link TRB at the last slot turns the array into a
// cycle; the enqueue pointer stops one short of it.
typedef struct {
    trb_t trbs[XHCI_RING_TRBS] __attribute__((aligned(64)));
    int     enqueue;    // next TRB index to fill
    uint32_t cycle;      // cycle bit currently written into fresh TRBs
} xhci_ring_t;

// One USB device (a mass-storage BOT unit).
typedef struct {
    int     in_use;
    int     slot_id;
    int     port;               // root port, 1-based
    int     speed;
    int     ep0_mps;
    int     bulk_out_dci;       // device context index (2*ep + 0)
    int     bulk_in_dci;       // (2*ep + 1)
    int     bulk_out_ep;        // endpoint NUMBER from the descriptor
    int     bulk_in_ep;
    int     bulk_out_mps;
    int     bulk_in_mps;
    uint32_t max_lba;           // READ CAPACITY(10): last valid block
    int     sector_size;

    uint32_t dctx[DCTX_DWORDS] __attribute__((aligned(64)));
    uint32_t ictx[ICTX_DWORDS] __attribute__((aligned(64)));
    xhci_ring_t ep0_ring;
    xhci_ring_t bulk_out_ring;
    xhci_ring_t bulk_in_ring;
} usb_dev_t;

// ---- static DMA structures (kernel VA == physical address) ----
static uint64_t xhci_dcbaa[XHCI_MAX_SLOTS + 1] __attribute__((aligned(64)));
static trb_t xhci_cmd_ring[XHCI_RING_TRBS] __attribute__((aligned(64)));
static trb_t xhci_event_ring[XHCI_RING_TRBS] __attribute__((aligned(64)));
// ERST: one segment descriptor — 64-bit base then {size, reserved}.
static uint32_t xhci_erst[4] __attribute__((aligned(64)));
static usb_dev_t xhci_devs[USB_MAX_DRIVES];
// Bounce buffer for the sector API: 64K-aligned so one Normal TRB of a
// full 128-sector batch never crosses a 64K boundary.
static uint8_t xhci_bounce[65536] __attribute__((aligned(65536)));
// BOT + descriptor buffers (DMA targets; static and aligned).
static uint8_t xhci_cbw[BOT_CBW_LEN] __attribute__((aligned(64)));
static uint8_t xhci_csw[BOT_CSW_LEN] __attribute__((aligned(64)));
static uint8_t xhci_devdesc[USB_DEV_DESC_LEN] __attribute__((aligned(64)));
static uint8_t xhci_cfgdesc[USB_CONFIG_MAX] __attribute__((aligned(64)));

static volatile uint8_t* xhci_base = 0;  // BAR0 kernel VA (identity)
static int xhci_op_off = 0;             // operational base offset
static int xhci_db_off = 0;
static int xhci_rt_off = 0;
static int xhci_max_ports = 0;
static int xhci_ndrives = 0;

// command ring producer state (the consumer is the controller)
static int cmd_enqueue;
static uint32_t cmd_cycle;
// event ring consumer state (the producer is the controller)
static int evt_dequeue;
static uint32_t evt_cycle;

// ---- register access ----
static inline uint32_t xreg(int off) {
    return *(volatile uint32_t*)(xhci_base + off);
}
static inline void xreg_w(int off, uint32_t v) {
    *(volatile uint32_t*)(xhci_base + off) = v;
}
static inline uint32_t op_reg(int reg)            { return xreg(xhci_op_off + reg); }
static inline void op_reg_w(int reg, uint32_t v)  { xreg_w(xhci_op_off + reg, v); }
static inline uint32_t rt_reg(int reg)            { return xreg(xhci_rt_off + reg); }
static inline void rt_reg_w(int reg, uint32_t v)  { xreg_w(xhci_rt_off + reg, v); }
static inline volatile uint32_t* portsc(int port) {
    // port is 1-based: port 1 sits at op + 0x400.
    return (volatile uint32_t*)(xhci_base + xhci_op_off + XHCI_PORT_BASE
                                + (port - 1) * XHCI_PORT_STRIDE);
}
static inline void ring_doorbell(int slot, int target) {
    *(volatile uint32_t*)(xhci_base + xhci_db_off + slot * 4)
        = (uint32_t)target;
}

// ---- ring producer helpers ----
// Reserve the next TRB slot, writing the Link TRB (with TC set so the
// controller toggles its cycle expectation) when the walk wraps. The
// caller fills the returned TRB and calls ring_commit().
static trb_t* ring_next(xhci_ring_t* r) {
    if (r->enqueue >= XHCI_RING_TRBS - 1) {
        trb_t* l = &r->trbs[XHCI_RING_TRBS - 1];
        memset(l, 0, sizeof(trb_t));
        l->param    = (uint32_t)(uintptr_t)&r->trbs[0];
        l->param_hi = 0;
        // The Link TRB carries the CURRENT lap's cycle (so the controller
        // consumes it as part of this batch) and TC toggles its
        // expectation for the next lap.
        l->control  = (r->cycle & TRB_CYCLE) | TRB_TC | TRB_TYPE(TRB_LINK);
        r->cycle   ^= 1;
        r->enqueue  = 0;
    }
    return &r->trbs[r->enqueue];
}
static void ring_commit(xhci_ring_t* r) { r->enqueue++; }

static void ring_reset(xhci_ring_t* r) {
    memset(r, 0, sizeof(*r));
    r->cycle = 1;      // DCS we hand the controller in the EP context
}

// ---- event-ring consumer ----
// Poll for an event TRB matching (type, slot, dci). want_slot/want_dci
// < 0 match anything. The 2M-iteration budget mirrors the AHCI polls —
// under TCG a controller wedge takes seconds to time out, and a healthy
// transfer completes in a handful of reads.
typedef struct {
    trb_t    trb;
    uint32_t cc;
    int      slot;
    int      epid;
} xhci_evt_t;

static int event_wait(xhci_evt_t* out, int want_type, int want_slot,
                      int want_epid, int timeout) {
    while (--timeout > 0) {
        trb_t* e = &xhci_event_ring[evt_dequeue];
        if ((e->control & TRB_CYCLE) != evt_cycle) continue;   // no event
        // consume it
        out->trb  = *e;
        out->cc   = TRB_CC(*e);
        out->slot = (int)TRB_SLOT(*e);
        out->epid = (int)TRB_EPID(*e);
        evt_dequeue++;
        if (evt_dequeue >= XHCI_RING_TRBS) {
            evt_dequeue = 0;          // event ring wraps WITHOUT a link TRB:
            evt_cycle ^= 1;          // the cycle bit alone marks the lap
        }
        // Advance ERDP so the controller knows the slot is free (QEMU
        // uses it for its event-ring-full check).
        uint32_t dp = (uint32_t)(uintptr_t)&xhci_event_ring[evt_dequeue];
        rt_reg_w(XHCI_IR0 + XHCI_IR_ERDP, dp | ERDP_EHB);

        int type = (e->control >> TRB_TYPE_SHIFT) & 0x3F;
        // Bring-up trace: every consumed event (boot-time only; see
        // xhci_evt_trace — cleared once enumeration is done).
        if (xhci_evt_trace) {
            write_serial_string("[XHCI] evt type=");
            write_serial_hex((uint32_t)type);
            write_serial_string(" slot=");
            write_serial_hex((uint32_t)out->slot);
            write_serial_string(" epid=");
            write_serial_hex((uint32_t)out->epid);
            write_serial_string(" cc=");
            write_serial_hex(out->cc);
            write_serial_string("\n");
        }
        if (type == want_type &&
            (want_slot < 0 || out->slot == want_slot) &&
            (want_epid < 0 || out->epid == want_epid)) {
            return 0;
        }
        // Not the event we wanted (e.g. a PORT_STATUS_CHANGE from another
        // port while a command is in flight) — keep polling.
    }
    return -1;
}

// Issue a command on the command ring and wait for its completion.
// Returns 0 + fills `out` with the completion event (its slot field
// carries the freshly allocated slot id for Enable Slot), -1 on error.
static int xhci_command(xhci_evt_t* out, int type, int slot, int bsr,
                        uint32_t ictx_addr) {
    trb_t* t = &xhci_cmd_ring[cmd_enqueue];
    if (cmd_enqueue >= XHCI_RING_TRBS - 1) {
        trb_t* l = &xhci_cmd_ring[XHCI_RING_TRBS - 1];
        memset(l, 0, sizeof(trb_t));
        l->param   = (uint32_t)(uintptr_t)&xhci_cmd_ring[0];
        l->control = (cmd_cycle & TRB_CYCLE) | TRB_TC | TRB_TYPE(TRB_LINK);
        cmd_cycle ^= 1;
        cmd_enqueue = 0;
        t = &xhci_cmd_ring[0];
    }
    memset(t, 0, sizeof(trb_t));
    t->param    = ictx_addr;      // input context pointer (addr/cfg cmds)
    t->status   = 0;              // interrupter target must stay 0
    t->control  = cmd_cycle | TRB_TYPE(type) | TRB_SLOT_ID(slot);
    if (bsr) t->control |= TRB_BSR;
    cmd_enqueue++;
    ring_doorbell(0, 0);

    // Commands are strictly one-at-a-time; any command completion is ours.
    if (event_wait(out, TRB_CMD_COMPLETION, -1, -1, 2000000) < 0) {
        write_serial_string("[XHCI] command timeout type=");
        write_serial_hex((uint32_t)type);
        write_serial_string("\n");
        return -1;
    }
    if (out->cc != CC_SUCCESS) {
        write_serial_string("[XHCI] command cc=");
        write_serial_hex(out->cc);
        write_serial_string(" type=");
        write_serial_hex((uint32_t)type);
        write_serial_string("\n");
        return -1;
    }
    return 0;
}

// ---- control transfers (endpoint 0) ----
// Setup (8 bytes inline via IDT), optional data stage, status stage.
// IOC sits on the status TRB only, so the TD produces exactly one event.
// `data` must be a DMA-safe kernel buffer (static/aligned like ours).
static int usb_control(usb_dev_t* d, const uint8_t* setup, uint8_t* data,
                       uint32_t len) {
    xhci_ring_t* r = &d->ep0_ring;
    int is_in = (setup[0] & 0x80) != 0;
    write_serial_string("[XHCI] ctl req=");
    write_serial_hex((uint32_t)setup[1]);
    write_serial_string(" val=");
    write_serial_hex(((uint32_t)setup[2] << 8) | setup[3]);
    write_serial_string(" len=");
    write_serial_hex(len);
    write_serial_string("\n");

    // 1. Setup stage: parameter = the 8 setup bytes, bmRequestType in the
    //    LOW byte (QEMU reads it from there). Length must be exactly 8
    //    and IDT must be set or QEMU rejects the TRB.
    {
        trb_t* t = ring_next(r);
        memset(t, 0, sizeof(trb_t));
        uint64_t p = 0;
        for (int i = 7; i >= 0; i--) p = (p << 8) | setup[i];
        t->param    = (uint32_t)(p & 0xFFFFFFFFull);
        t->param_hi = (uint32_t)(p >> 32);
        t->status   = TRB_LEN(8);
        t->control  = r->cycle | TRB_TYPE(TRB_SETUP_STAGE) | TRB_IDT
                    | TRB_SLOT_ID(d->slot_id);
        if (len > 0) {
            // TRT is not parsed by QEMU (direction comes from
            // bmRequestType) but keep it spec-correct for real HW.
            t->control |= is_in ? TRB_TRT_IN : TRB_TRT_OUT;
        }
        ring_commit(r);
    }
    // 2. Data stage. QEMU checks the DIR bit on data-stage TRBs — it must
    //    agree with bmRequestType.
    if (len > 0) {
        trb_t* t = ring_next(r);
        memset(t, 0, sizeof(trb_t));
        t->param    = (uint32_t)(uintptr_t)data;
        t->param_hi = 0;
        t->status   = TRB_LEN(len);
        t->control  = r->cycle | TRB_TYPE(TRB_DATA_STAGE)
                    | (is_in ? TRB_DIR_IN : 0)
                    | TRB_SLOT_ID(d->slot_id);
        ring_commit(r);
    }
    // 3. Status stage: always the OPPOSITE direction of the data stage
    //    (zero-length), IN when there was no data stage at all.
    {
        trb_t* t = ring_next(r);
        memset(t, 0, sizeof(trb_t));
        t->param    = 0;
        t->param_hi = 0;
        t->status   = TRB_LEN(0);
        t->control  = r->cycle | TRB_TYPE(TRB_STATUS_STAGE) | TRB_IOC
                    | ((len == 0 || !is_in) ? TRB_DIR_IN : 0)
                    | TRB_SLOT_ID(d->slot_id);
        ring_commit(r);
    }
    ring_doorbell(d->slot_id, 1);      // EP0

    xhci_evt_t ev;
    if (event_wait(&ev, TRB_TRANSFER_EVENT, d->slot_id, 1, 2000000) < 0) {
        write_serial_string("[XHCI] control transfer timeout\n");
        return -1;
    }
    if (ev.cc != CC_SUCCESS && ev.cc != CC_SHORT_PACKET) {
        write_serial_string("[XHCI] control cc=");
        write_serial_hex(ev.cc);
        write_serial_string("\n");
        return -1;
    }
    return 0;
}

// ---- standard USB requests ----
// GET_DESCRIPTOR wValue = (type << 8) | index — the TYPE is the HIGH byte
// (USB spec 9.4.3). 0x0100 = device, 0x0200 = configuration.
static int usb_get_device_desc(usb_dev_t* d) {
    uint8_t setup[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00,
                         USB_DEV_DESC_LEN, 0x00 };
    return usb_control(d, setup, xhci_devdesc, USB_DEV_DESC_LEN);
}

static int usb_get_config_desc(usb_dev_t* d, uint32_t total_len) {
    if (total_len > USB_CONFIG_MAX) total_len = USB_CONFIG_MAX;
    uint8_t setup[8] = { 0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00 };
    setup[6] = (uint8_t)(total_len & 0xFF);
    setup[7] = (uint8_t)(total_len >> 8);
    return usb_control(d, setup, xhci_cfgdesc, total_len);
}

static int usb_set_configuration(usb_dev_t* d, uint8_t cfg) {
    uint8_t setup[8] = { 0x00, 0x09, cfg, 0x00, 0x00, 0x00, 0x00, 0x00 };
    return usb_control(d, setup, 0, 0);
}

// ---- BOT (Bulk-Only Transport) ----
// CBW -> data -> CSW, one TRB per phase, IOC on each (each phase is its
// own TD on its own ring, so each produces one event we must await).
// Returns 0 when the CSW is valid and error-free, -1 otherwise.
static int usb_bot(usb_dev_t* d, uint32_t tag, uint8_t* data,
                   uint32_t data_len) {
    xhci_evt_t ev;

    // 1. CBW -> bulk OUT. DIR must be clear (QEMU checks Normal TRBs too).
    {
        xhci_ring_t* r = &d->bulk_out_ring;
        trb_t* t = ring_next(r);
        memset(t, 0, sizeof(trb_t));
        t->param  = (uint32_t)(uintptr_t)xhci_cbw;
        t->status = TRB_LEN(BOT_CBW_LEN);
        t->control = r->cycle | TRB_TYPE(TRB_NORMAL) | TRB_IOC
                   | TRB_SLOT_ID(d->slot_id);
        ring_commit(r);
        ring_doorbell(d->slot_id, d->bulk_out_dci);
        if (event_wait(&ev, TRB_TRANSFER_EVENT, d->slot_id,
                       d->bulk_out_dci, 2000000) < 0) {
            write_serial_string("[XHCI] CBW TIMEOUT\n");
            return -1;
        }
        if (ev.cc != CC_SUCCESS && ev.cc != CC_SHORT_PACKET) {
            write_serial_string("[XHCI] CBW failed cc=");
            write_serial_hex(ev.cc);
            write_serial_string("\n");
            return -1;
        }
    }
    // 2. Data phase (direction comes from the CBW's bmCBWFlags, byte 12).
    int data_in = (xhci_cbw[12] & 0x80) != 0;
    if (data_len > 0) {
        xhci_ring_t* r = data_in ? &d->bulk_in_ring : &d->bulk_out_ring;
        int epid = data_in ? d->bulk_in_dci : d->bulk_out_dci;
        trb_t* t = ring_next(r);
        memset(t, 0, sizeof(trb_t));
        t->param  = (uint32_t)(uintptr_t)data;
        t->status = TRB_LEN(data_len);
        t->control = r->cycle | TRB_TYPE(TRB_NORMAL) | TRB_IOC
                   | (data_in ? TRB_DIR_IN : 0)
                   | TRB_SLOT_ID(d->slot_id);
        ring_commit(r);
        ring_doorbell(d->slot_id, epid);
        if (event_wait(&ev, TRB_TRANSFER_EVENT, d->slot_id, epid,
                       2000000) < 0) {
            write_serial_string("[XHCI] BOT data phase TIMEOUT (epid=");
            write_serial_hex((uint32_t)epid);
            write_serial_string(")\n");
            return -1;
        }
        if (ev.cc != CC_SUCCESS && ev.cc != CC_SHORT_PACKET) {
            write_serial_string("[XHCI] BOT data phase failed cc=");
            write_serial_hex(ev.cc);
            write_serial_string("\n");
            return -1;
        }
    }
    // 3. CSW <- bulk IN (DIR set).
    {
        xhci_ring_t* r = &d->bulk_in_ring;
        trb_t* t = ring_next(r);
        memset(t, 0, sizeof(trb_t));
        t->param  = (uint32_t)(uintptr_t)xhci_csw;
        t->status = TRB_LEN(BOT_CSW_LEN);
        t->control = r->cycle | TRB_TYPE(TRB_NORMAL) | TRB_IOC
                   | TRB_DIR_IN | TRB_SLOT_ID(d->slot_id);
        ring_commit(r);
        ring_doorbell(d->slot_id, d->bulk_in_dci);
        if (event_wait(&ev, TRB_TRANSFER_EVENT, d->slot_id,
                       d->bulk_in_dci, 2000000) < 0) {
            write_serial_string("[XHCI] CSW TIMEOUT\n");
            return -1;
        }
        if (ev.cc != CC_SUCCESS) {
            write_serial_string("[XHCI] CSW failed cc=");
            write_serial_hex(ev.cc);
            write_serial_string("\n");
            return -1;
        }
    }
    // Validate the CSW: signature 'USBS', matching tag, status 0.
    uint32_t sig = (uint32_t)xhci_csw[0] | ((uint32_t)xhci_csw[1] << 8)
                 | ((uint32_t)xhci_csw[2] << 16) | ((uint32_t)xhci_csw[3] << 24);
    uint32_t ctag = (uint32_t)xhci_csw[4] | ((uint32_t)xhci_csw[5] << 8)
                  | ((uint32_t)xhci_csw[6] << 16) | ((uint32_t)xhci_csw[7] << 24);
    uint32_t residue = (uint32_t)xhci_csw[8] | ((uint32_t)xhci_csw[9] << 8)
                     | ((uint32_t)xhci_csw[10] << 16) | ((uint32_t)xhci_csw[11] << 24);
    if (sig != CSW_SIG || ctag != tag || xhci_csw[12] != 0) {
        write_serial_string("[XHCI] bad CSW sig=");
        write_serial_hex(sig);
        write_serial_string(" tag=");
        write_serial_hex(ctag);
        write_serial_string(" (want ");
        write_serial_hex(tag);
        write_serial_string(") residue=");
        write_serial_hex(residue);
        write_serial_string(" st=");
        write_serial_hex((uint32_t)xhci_csw[12]);
        write_serial_string("\n");
        return -1;
    }
    return 0;
}

// Build a CBW (31 bytes) around a SCSI CDB and run the BOT transaction.
// CBW layout (BOT spec 5.1): sig 0-3, tag 4-7, data length 8-11,
// bmCBWFlags 12, bCBWLUN 13, bCBWCBLength 14, CBWCB 15-30 (16 bytes).
// dCBWDataTransferLength = the number of data bytes the command moves.
// is_in: 1 = data from device to host (IN), 0 = host to device (OUT).
static int usb_scsi(usb_dev_t* d, const uint8_t* cdb, int cdb_len,
                    uint8_t* data, uint32_t data_len, int is_in) {
    static uint32_t bot_tag = 0x4D4354u;   // 'MCT'
    memset(xhci_cbw, 0, BOT_CBW_LEN);
    xhci_cbw[0] = (uint8_t)(CBW_SIG & 0xFF);         // 'U'
    xhci_cbw[1] = (uint8_t)((CBW_SIG >> 8) & 0xFF);  // 'S'
    xhci_cbw[2] = (uint8_t)((CBW_SIG >> 16) & 0xFF); // 'B'
    xhci_cbw[3] = (uint8_t)((CBW_SIG >> 24) & 0xFF);// 'C'
    uint32_t tag = ++bot_tag;
    xhci_cbw[4] = (uint8_t)(tag & 0xFF);
    xhci_cbw[5] = (uint8_t)((tag >> 8) & 0xFF);
    xhci_cbw[6] = (uint8_t)((tag >> 16) & 0xFF);
    xhci_cbw[7] = (uint8_t)((tag >> 24) & 0xFF);
    xhci_cbw[8]  = (uint8_t)(data_len & 0xFF);        // dCBWDataTransferLength
    xhci_cbw[9]  = (uint8_t)((data_len >> 8) & 0xFF);
    xhci_cbw[10] = (uint8_t)((data_len >> 16) & 0xFF);
    xhci_cbw[11] = (uint8_t)((data_len >> 24) & 0xFF);
    xhci_cbw[12] = (data_len > 0 && is_in) ? 0x80 : 0x00;  // bmCBWFlags
    xhci_cbw[13] = 0;                                  // bCBWLUN: LUN 0
    xhci_cbw[14] = (uint8_t)cdb_len;                   // bCBWCBLength
    for (int i = 0; i < cdb_len && i < 16; i++)
        xhci_cbw[15 + i] = cdb[i];
    return usb_bot(d, tag, data, data_len);
}

// ---- SCSI commands ----
// INQUIRY needs 36 bytes, READ CAPACITY 8, REQUEST SENSE 18 — keep one
// buffer large enough for the largest (INQUIRY) to avoid overflow into
// the .bss neighbours (evt_dequeue/cycle lived 12/8 bytes after the old
// 8-byte array and got clobbered with "QEMU    ").
static uint8_t xhci_cap8[64] __attribute__((aligned(64)));
static uint8_t xhci_sense[18] __attribute__((aligned(64)));

static int usb_scsi_test_unit_ready(usb_dev_t* d) {
    uint8_t cdb[6] = { 0x00, 0, 0, 0, 0, 0 };
    return usb_scsi(d, cdb, 6, 0, 0, 0);
}

// REQUEST SENSE: the standard follow-up when a command fails — the sense
// key / ASC / ASCQ say exactly why. Also used as a bring-up diagnostic.
static int usb_scsi_request_sense(usb_dev_t* d) {
    uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    memset(xhci_sense, 0, sizeof(xhci_sense));
    if (usb_scsi(d, cdb, 6, xhci_sense, 18, 1) < 0) {
        write_serial_string("[XHCI] REQUEST SENSE itself failed\n");
        return -1;
    }
    if (xhci_sense[0] == 0) {
        write_serial_string("[XHCI] sense data EMPTY (0 bytes)\n");
        return -1;
    }
    write_serial_string("[XHCI] sense key=");
    write_serial_hex((uint32_t)(xhci_sense[2] & 0x0F));
    write_serial_string(" asc=");
    write_serial_hex((uint32_t)xhci_sense[12]);
    write_serial_hex((uint32_t)xhci_sense[13]);
    write_serial_string("\n");
    return 0;
}

static int usb_scsi_inquiry(usb_dev_t* d) {
    uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    // Peripheral device type must be 0 (direct-access block device);
    // QEMU's usb-storage answers INQUIRY with type 0.
    if (usb_scsi(d, cdb, 6, xhci_cap8, 36, 1) < 0) return -1;
    if ((xhci_cap8[0] & 0x1F) != 0) {
        write_serial_string("[XHCI] not a block device (type=");
        write_serial_hex((uint32_t)(xhci_cap8[0] & 0x1F));
        write_serial_string(")\n");
        return -1;
    }
    return 0;
}

static int usb_scsi_read_capacity(usb_dev_t* d) {
    uint8_t cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (usb_scsi(d, cdb, 10, xhci_cap8, 8, 1) < 0) return -1;
    d->max_lba = ((uint32_t)xhci_cap8[0] << 24) | ((uint32_t)xhci_cap8[1] << 16)
               | ((uint32_t)xhci_cap8[2] << 8) | (uint32_t)xhci_cap8[3];
    d->sector_size = ((uint32_t)xhci_cap8[4] << 24) | ((uint32_t)xhci_cap8[5] << 16)
                   | ((uint32_t)xhci_cap8[6] << 8) | (uint32_t)xhci_cap8[7];
    return 0;
}

// One READ(10)/WRITE(10) of up to 128 sectors through the bounce buffer.
static int usb_scsi_rw10(usb_dev_t* d, int is_write, uint32_t lba,
                         int count, uint8_t* bounce) {
    uint8_t cdb[10] = { 0 };
    cdb[0] = is_write ? 0x2A : 0x28;
    cdb[2] = (uint8_t)(lba >> 24); cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);  cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(count >> 8); cdb[8] = (uint8_t)count;
    return usb_scsi(d, cdb, 10, bounce, (uint32_t)count * 512, !is_write);
}

// ---- device enumeration ----
// Enable a slot, address the device, read descriptors, find the MSC
// BOT interface, configure the endpoints. Returns 0 when `d` is a usable
// mass-storage device (bulk endpoints armed), -1 otherwise.
static int usb_enumerate(usb_dev_t* d, int port, int speed) {
    d->port = port;
    d->speed = speed;
    write_serial_string("[XHCI] enumerate port ");
    write_serial_hex((uint32_t)port);
    write_serial_string(" speed ");
    write_serial_hex((uint32_t)speed);
    write_serial_string("\n");
    d->ep0_mps = (speed == SPEED_SUPER) ? EP0_MPS_SUPER : EP0_MPS_HIGH;
    int bulk_mps = (speed == SPEED_SUPER) ? BULK_MPS_SUPER : BULK_MPS_HIGH;

    // 1. Enable Slot — the completion event carries the new slot id.
    xhci_evt_t ev;
    if (xhci_command(&ev, TRB_ENABLE_SLOT, 0, 0, 0) < 0) return -1;
    d->slot_id = ev.slot;
    if (d->slot_id < 1 || d->slot_id > XHCI_MAX_SLOTS) {
        write_serial_string("[XHCI] bad slot id ");
        write_serial_hex((uint32_t)d->slot_id);
        write_serial_string("\n");
        return -1;
    }
    // The controller reads the output-context pointer from DCBAA[slot]
    // ONCE, during Address Device — it must be valid before the command.
    xhci_dcbaa[d->slot_id] = (uint64_t)(uintptr_t)d->dctx;
    ring_reset(&d->ep0_ring);

    // 2. Address Device: input context = ICC (drop=0, add=A0|A1) + slot
    //    context (route = this root port, QEMU looks the device up by it)
    //    + EP0 context (control, MPS, empty ring with DCS=1).
    memset(d->ictx, 0, sizeof(d->ictx));
    d->ictx[ICTX_ADD] = ADD_FLAG(0) | ADD_FLAG(1);
    uint32_t* sctx = &d->ictx[SLOT_OFF / 4];
    // Root devices: route string stays EMPTY (dword 0 carries only the
    // context-entries count); QEMU's xhci_lookup_uport takes the root
    // port from dword 1 and walks the route nibbles for hub children
    // only — a nonzero route here made it look for "1.1" and fail.
    sctx[0] = 0;
    sctx[1] = SLOT_ROOT_PORT(port);
    uint32_t* e0 = &d->ictx[EPCTX_OFF(1) / 4];
    e0[1] = EPCTX1(EP_TYPE_CTRL, d->ep0_mps);
    e0[2] = EPCTX_DEQ(&d->ep0_ring.trbs[0]);
    e0[3] = 0;

    if (xhci_command(&ev, TRB_ADDR_DEV, d->slot_id, 0 /* BSR=0 */,
                     (uint32_t)(uintptr_t)d->ictx) < 0) return -1;

    // 3. Device descriptor (18 bytes) — vendor/product for the log.
    if (usb_get_device_desc(d) < 0) return -1;
    write_serial_string("[XHCI] dev ");
    write_serial_hex((uint32_t)d->slot_id);
    write_serial_string(" vid=");
    write_serial_hex((uint32_t)(xhci_devdesc[8] | (xhci_devdesc[9] << 8)));
    write_serial_string(" pid=");
    write_serial_hex((uint32_t)(xhci_devdesc[10] | (xhci_devdesc[11] << 8)));
    write_serial_string("\n");

    // 4. Configuration descriptor: read the 9-byte head for wTotalLength,
    //    then the full tree, and walk it for the MSC BOT interface
    //    (class 08 / subclass 06 / protocol 50) and its two bulk endpoints.
    uint8_t setup9[8] = { 0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 9, 0 };
    if (usb_control(d, setup9, xhci_cfgdesc, 9) < 0) return -1;
    uint32_t total = (uint32_t)xhci_cfgdesc[2] | ((uint32_t)xhci_cfgdesc[3] << 8);
    if (total < 9 || total > USB_CONFIG_MAX) {
        write_serial_string("[XHCI] bad config length ");
        write_serial_hex(total);
        write_serial_string("\n");
        return -1;
    }
    if (usb_get_config_desc(d, total) < 0) return -1;
    uint8_t cfg_value = xhci_cfgdesc[5];

    int if_class = -1, if_sub = -1, if_proto = -1;
    int msc_iface = -1;
    d->bulk_in_ep = d->bulk_out_ep = -1;
    int msc = 0;
    for (uint32_t i = 0; i + 1 < total; ) {
        uint8_t len = xhci_cfgdesc[i];
        uint8_t type = xhci_cfgdesc[i + 1];
        if (len < 2) break;
        if (type == 4 && len >= 9) {             // interface descriptor
            if_class = xhci_cfgdesc[i + 5];
            if_sub = xhci_cfgdesc[i + 6];
            if_proto = xhci_cfgdesc[i + 7];
            msc = (if_class == 0x08 && if_sub == 0x06 && if_proto == 0x50);
            if (msc && msc_iface < 0) msc_iface = (int)xhci_cfgdesc[i + 2];
        } else if (type == 5 && len >= 7 && msc) {   // endpoint descriptor
            uint8_t addr = xhci_cfgdesc[i + 2];
            uint8_t attrs = xhci_cfgdesc[i + 3];
            uint16_t mps = (uint16_t)(xhci_cfgdesc[i + 4]
                                      | (xhci_cfgdesc[i + 5] << 8));
            if ((attrs & 0x03) == 2) {                // bulk
                if (mps == 0) mps = (uint16_t)bulk_mps;
                if (addr & 0x80) {
                    d->bulk_in_ep = addr & 0x0F;
                    d->bulk_in_dci = 2 * (addr & 0x0F) + 1;
                    d->bulk_in_mps = mps;
                } else {
                    d->bulk_out_ep = addr & 0x0F;
                    d->bulk_out_dci = 2 * (addr & 0x0F);
                    d->bulk_out_mps = mps;
                }
            }
        }
        i += len;
    }
    if (msc_iface < 0 || d->bulk_in_ep < 0 || d->bulk_out_ep < 0) {
        write_serial_string("[XHCI] no MSC BOT interface\n");
        return -1;
    }
    // The context arrays only hold DCI <= 8 (bulk EP number <= 4 OUT /
    // <= 3 IN): a descriptor claiming higher bulk EPs would make Configure
    // Endpoint write `ictx` past its end (and the xHC DMA-reads the input
    // context past it too, leaking kernel memory). QEMU's usb-storage uses
    // EP1/EP2; anything bigger is rejected instead of corrupting the
    // adjacent DMA structures.
    if (d->bulk_out_dci > 8 || d->bulk_in_dci > 8) {
        write_serial_string("[XHCI] bulk EP number too high (DCI ");
        write_serial_hex((uint32_t)d->bulk_in_dci);
        write_serial_string("/");
        write_serial_hex((uint32_t)d->bulk_out_dci);
        write_serial_string(" > 8), device skipped\n");
        return -1;
    }

    // 5. Set Configuration, then Configure Endpoint with the two bulk
    //    endpoint contexts. Add flags: A0 (slot, required by QEMU) + the
    //    two bulk DCIs; the slot context's Context Entries = highest DCI.
    if (usb_set_configuration(d, cfg_value) < 0) return -1;

    ring_reset(&d->bulk_out_ring);
    ring_reset(&d->bulk_in_ring);
    // Context Entries = the highest DCI in use (the bulk OUT DCI can
    // exceed the IN one when the device uses different EP numbers).
    int max_dci = d->bulk_out_dci > d->bulk_in_dci ? d->bulk_out_dci
                                                   : d->bulk_in_dci;
    memset(d->ictx, 0, sizeof(d->ictx));
    d->ictx[ICTX_ADD] = ADD_FLAG(0) | ADD_FLAG(d->bulk_out_dci)
                      | ADD_FLAG(d->bulk_in_dci);
    sctx[0] = SLOT_CTX_ENTRIES(max_dci);
    sctx[1] = SLOT_ROOT_PORT(port);
    uint32_t* eo = &d->ictx[EPCTX_OFF(d->bulk_out_dci) / 4];
    eo[1] = EPCTX1(EP_TYPE_BULK_OUT, d->bulk_out_mps);
    eo[2] = EPCTX_DEQ(&d->bulk_out_ring.trbs[0]);
    uint32_t* ei = &d->ictx[EPCTX_OFF(d->bulk_in_dci) / 4];
    ei[1] = EPCTX1(EP_TYPE_BULK_IN, d->bulk_in_mps);
    ei[2] = EPCTX_DEQ(&d->bulk_in_ring.trbs[0]);

    if (xhci_command(&ev, TRB_CFG_EP, d->slot_id, 0,
                     (uint32_t)(uintptr_t)d->ictx) < 0) return -1;

    // 6. SCSI: the device must be ready, a direct-access block device,
    //    and report 512-byte sectors — then it becomes a drive. A failed
    //    command is followed by REQUEST SENSE (standard MSC recovery flow,
    //    and it names the reason on the serial log).
    int t;
    for (t = 0; t < 3; t++) {
        if (usb_scsi_test_unit_ready(d) == 0) break;
        usb_scsi_request_sense(d);
    }
    if (t == 3) {
        write_serial_string("[XHCI] TEST UNIT READY never passed\n");
        return -1;
    }
    if (usb_scsi_inquiry(d) < 0) return -1;
    if (usb_scsi_read_capacity(d) < 0) return -1;
    if (d->sector_size != 512) {
        write_serial_string("[XHCI] sector size != 512: ");
        write_serial_hex((uint32_t)d->sector_size);
        write_serial_string("\n");
        return -1;
    }
    return 0;
}

// ---- controller bring-up + port scan ----
static int xhci_bringup(void) {
    // Reset: halt first (RUN=0), then HCRST. QEMU completes resets
    // synchronously (CNR never sets) but poll anyway for real hardware.
    op_reg_w(XHCI_OP_USBCMD, 0);
    int t = 200000;
    while (!(op_reg(XHCI_OP_USBSTS) & USBSTS_HCH) && --t > 0);
    if (t == 0) {
        write_serial_string("[XHCI] controller never halted\n");
        return -1;
    }
    op_reg_w(XHCI_OP_USBCMD, USBCMD_HCRST);
    t = 2000000;
    while ((op_reg(XHCI_OP_USBCMD) & USBCMD_HCRST) && --t > 0);
    if (t == 0) {
        write_serial_string("[XHCI] HCRST never completed\n");
        return -1;
    }
    while (op_reg(XHCI_OP_USBSTS) & USBSTS_CNR) {
        if (--t <= 0) break;               // QEMU never sets CNR; real HW may
    }

    // Command ring: 256 TRBs, Link TRB closes the cycle. CRCR takes the
    // base | cycle on the low dword (write low then high; QEMU builds the
    // ring on the high write, masking the low 6 bits for alignment).
    memset(xhci_cmd_ring, 0, sizeof(xhci_cmd_ring));
    cmd_enqueue = 0;
    cmd_cycle = 1;
    op_reg_w(XHCI_OP_CRCR, (uint32_t)(uintptr_t)&xhci_cmd_ring[0] | 1u);
    op_reg_w(XHCI_OP_CRCR + 4, 0);

    // Event ring + ERST (interrupter 0): one 256-TRB segment. The event
    // ring has no Link TRB — the cycle bit alone marks the lap.
    memset(xhci_event_ring, 0, sizeof(xhci_event_ring));
    evt_dequeue = 0;
    evt_cycle = 1;
    xhci_erst[0] = (uint32_t)(uintptr_t)&xhci_event_ring[0];
    xhci_erst[1] = 0;
    xhci_erst[2] = XHCI_RING_TRBS;
    xhci_erst[3] = 0;
    rt_reg_w(XHCI_IR0 + XHCI_IR_ERSTSZ, 1);
    rt_reg_w(XHCI_IR0 + XHCI_IR_ERSTBA, (uint32_t)(uintptr_t)&xhci_erst[0]);
    rt_reg_w(XHCI_IR0 + XHCI_IR_ERSTBA + 4, 0);
    rt_reg_w(XHCI_IR0 + XHCI_IR_ERDP,
             (uint32_t)(uintptr_t)&xhci_event_ring[0] | ERDP_EHB);
    rt_reg_w(XHCI_IR0 + XHCI_IR_ERDP + 4, 0);
    rt_reg_w(XHCI_IR0 + XHCI_IR_IMAN, 0);      // poll model: IE stays off

    // DCBAA (no scratchpads: QEMU's HCSPARAMS2 = 0xF has SPB bits 0) and
    // MaxSlotsEn, then RUN.
    memset((void*)xhci_dcbaa, 0, sizeof(xhci_dcbaa));
    op_reg_w(XHCI_OP_DCBAAP, (uint32_t)(uintptr_t)&xhci_dcbaa[0]);
    op_reg_w(XHCI_OP_DCBAAP + 4, 0);
    uint32_t maxslots = xreg(XHCI_CAP_HCSPARAMS1) & 0xFF;
    if (maxslots > XHCI_MAX_SLOTS) maxslots = XHCI_MAX_SLOTS;
    if (maxslots == 0) maxslots = 1;
    op_reg_w(XHCI_OP_CONFIG, maxslots);

    op_reg_w(XHCI_OP_USBCMD, USBCMD_RUN);
    t = 200000;
    while ((op_reg(XHCI_OP_USBSTS) & USBSTS_HCH) && --t > 0);
    if (t == 0) {
        write_serial_string("[XHCI] controller never started\n");
        return -1;
    }
    return 0;
}

// Reset one root port and wait for it to settle. Returns the PORTSC speed
// field on success, -1 when the port never re-enabled.
static int xhci_port_reset(int port) {
    volatile uint32_t* ps = portsc(port);
    // Clear latched change bits (write-1-to-clear) and assert PR,
    // preserving PP (port power is a plain RW bit — writing 0 kills it).
    uint32_t v = *ps;
    v &= ~(PORTSC_ALL_CHANGES | PORTSC_PR);
    v |= PORTSC_PR;
    *ps = v;

    int t = 4000000;                     // PR self-clears when reset ends
    while ((*ps & PORTSC_PR) && --t > 0);
    if (t == 0) {
        write_serial_string("[XHCI] port ");
        write_serial_hex((uint32_t)port);
        write_serial_string(" reset never finished\n");
        return -1;
    }
    // Reset done: clear the change latches again (PRC/CSC from the reset).
    v = *ps;
    v &= ~PORTSC_PR;
    v |= PORTSC_ALL_CHANGES;
    *ps = v;
    if (!(*ps & PORTSC_PED)) {
        write_serial_string("[XHCI] port ");
        write_serial_hex((uint32_t)port);
        write_serial_string(" not enabled after reset (PORTSC=");
        write_serial_hex(*ps);
        write_serial_string(")\n");
        return -1;
    }
    return (int)((*ps & PORTSC_SPEED_MASK) >> 10);
}

void xhci_init(void) {
    for (int i = 0; i < USB_MAX_DRIVES; i++) {
        xhci_devs[i].in_use = 0;
        xhci_devs[i].slot_id = 0;
    }
    xhci_ndrives = 0;

    // Bring-up diagnostic: what the PCI scan actually collected.
    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t* p = &pci_devices[i];
        write_serial_string("[XHCI] pci ");
        write_serial_hex(((uint32_t)p->bus << 16)
                         | ((uint32_t)p->slot << 8) | (uint32_t)p->func);
        write_serial_string(" class=");
        write_serial_hex(((uint32_t)p->class_code << 16)
                         | ((uint32_t)p->subclass << 8) | (uint32_t)p->prog_if);
        write_serial_string(" vid:dev=");
        write_serial_hex(((uint32_t)p->vendor_id << 16) | (uint32_t)p->device_id);
        write_serial_string("\n");
    }

    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t* p = &pci_devices[i];
        // Serial bus (0x0C) / USB (0x03). Real xHCI reports prog_if 0x30;
        // QEMU's qemu-xhci only sets the class word (prog_if byte stays 0),
        // so the Red Hat QEMU / NEC vendor-device ids vouch for it too.
        if (p->class_code != 0x0C || p->subclass != 0x03) continue;
        int is_xhci = (p->prog_if == 0x30) ||
                      (p->vendor_id == 0x1B36 && p->device_id == 0x000D) ||
                      (p->vendor_id == 0x1033 && p->device_id == 0x0194);
        if (!is_xhci) {
            write_serial_string("[XHCI] USB controller prog_if ");
            write_serial_hex((uint32_t)p->prog_if);
            write_serial_string(" skipped (not xHCI)\n");
            continue;
        }

        // BAR0 is a 64-bit memory BAR: low dword at 0x10, high at 0x14.
        // Type decode (bits 3:1 of the low dword): (v & 6) == 0 -> 32-bit,
        // (v & 6) == 4 -> 64-bit (PCI_BASE_ADDRESS_MEM_TYPE_64 = 0x04, i.e.
        // bits 2:1 = 10 — NOT 01; ahci.c's "~0xF" base mask is unaffected).
        // QEMU's qemu-xhci BAR0 reads back 0xFEBF0004: 64-bit, non-prefetch.
        uint32_t bar0 = pci_read(p->bus, p->slot, p->func, 0x10);
        if (bar0 & 1) continue;                     // must be memory space
        uint32_t base = bar0 & ~0xF;
        uint32_t memtype = bar0 & 0x6;
        if (memtype == 0x6) continue;               // reserved encoding
        if (memtype == 0x4) {                       // 64-bit: check the upper half
            uint32_t hi = pci_read(p->bus, p->slot, p->func, 0x14);
            if (hi != 0) {
                write_serial_string("[XHCI] BAR0 above 4GB\n");
                return;
            }
        }
        if (base < 0xFE000000 || base >= 0xFF000000) {
            // Outside the static MMIO window (unexpected on QEMU i440fx):
            // page_map only covers the window, so this is a bail-out.
            write_serial_string("[XHCI] BAR0 outside the MMIO window: ");
            write_serial_hex(base);
            write_serial_string("\n");
            return;
        }
        // The window is mapped from boot; make the mapping explicit
        // (idempotent) and keep the window's strong-uncached PCD|PWT
        // flags, exactly like the AHCI driver does for BAR5.
        page_map(base, base, PAGE_PRESENT | PAGE_RW | 0x18);
        uint32_t pcicmd = pci_read(p->bus, p->slot, p->func, 0x04);
        pci_write(p->bus, p->slot, p->func, 0x04, pcicmd | 0x6);

        xhci_base    = (volatile uint8_t*)(uintptr_t)base;
        xhci_op_off  = (int)(xreg(XHCI_CAP_CAPLENGTH) & 0xFF);
        xhci_db_off  = (int)xreg(XHCI_CAP_DBOFF);
        xhci_rt_off  = (int)xreg(XHCI_CAP_RTSOFF);
        uint32_t h1  = xreg(XHCI_CAP_HCSPARAMS1);
        xhci_max_ports = (int)((h1 >> 24) & 0xFF);
        if (xhci_max_ports > XHCI_MAX_PORTS) xhci_max_ports = XHCI_MAX_PORTS;

        write_serial_string("[XHCI] controller @ ");
        write_serial_hex(base);
        write_serial_string(" op=0x");
        write_serial_hex((uint32_t)xhci_op_off);
        write_serial_string(" ports=");
        write_serial_hex((uint32_t)xhci_max_ports);
        write_serial_string("\n");

        if (xhci_bringup() < 0) return;

        // Port scan: reset every connected USB2/USB3 port and try to
        // enumerate it as mass storage. Full/low-speed devices would need
        // the two-step MPS probe (read 8 dev-desc bytes at address 0) —
        // logged and skipped for now; QEMU's storage sits on SS/HS ports.
        for (int port = 1; port <= xhci_max_ports && xhci_ndrives < USB_MAX_DRIVES;
             port++) {
            volatile uint32_t* ps = portsc(port);
            uint32_t psc = *ps;
            if (!(psc & PORTSC_CCS)) continue;
            int speed = (int)((psc & PORTSC_SPEED_MASK) >> 10);
            if (speed != SPEED_SUPER && speed != SPEED_HIGH) {
                write_serial_string("[XHCI] port ");
                write_serial_hex((uint32_t)port);
                write_serial_string(" speed ");
                write_serial_hex((uint32_t)speed);
                write_serial_string(" skipped (only HS/SS supported)\n");
                continue;
            }
            speed = xhci_port_reset(port);
            if (speed < 0) continue;
            if (speed != SPEED_SUPER && speed != SPEED_HIGH) continue;

            usb_dev_t* d = &xhci_devs[xhci_ndrives];
            if (usb_enumerate(d, port, speed) < 0) {
                d->in_use = 0;
                continue;
            }
            d->in_use = 1;
            write_serial_string("[XHCI] port ");
            write_serial_hex((uint32_t)port);
            write_serial_string(" USB");
            write_serial_string(speed == SPEED_SUPER ? "3" : "2");
            write_serial_string(" -> drive ");
            write_serial_hex((uint32_t)(USB_DRIVE_BASE + xhci_ndrives));
            write_serial_string(" (");
            write_serial_hex(d->max_lba + 1);
            write_serial_string(" blocks)\n");
            xhci_ndrives++;
        }
        xhci_evt_trace = 0;   // data path starts now — no per-event trace
        write_serial_string(xhci_ndrives ? "[XHCI] ready\n"
                                         : "[XHCI] no mass-storage devices\n");
        return;
    }
    write_serial_string("[XHCI] no xHCI controller (PCI)\n");
}

int usb_present(void) { return xhci_ndrives > 0; }

// ---- public sector API (drive = USB_DRIVE_BASE + device index) ----
// Mirrors the AHCI sector API: batch clamp via ata_batch_limit, transfer
// through the 64K-aligned bounce buffer, everything under xhci_lock with
// IRQs off (the completion polls the event ring; an IRQ-context caller
// would deadlock on the held spinlock, and nothing does that).

int usb_read_sectors(int drive, uint32_t lba, int count, uint8_t* buf) {
    int idx = drive - USB_DRIVE_BASE;
    if (!xhci_base || idx < 0 || idx >= USB_MAX_DRIVES) return -1;
    usb_dev_t* d = &xhci_devs[idx];
    if (!d->in_use || count < 1) return -1;
    if (count > 0 && lba + (uint32_t)count - 1 > d->max_lba) return -1;

    xhci_eflags = spin_lock_irqsave(&xhci_lock);
    hdd_activity = 10;
    int rc = 0;
    uint32_t done = 0;
    while (done < (uint32_t)count && rc == 0) {
        int batch = ata_batch_limit(lba + done, count - (int)done);
        if (usb_scsi_rw10(d, 0, lba + done, batch, xhci_bounce) < 0) {
            rc = -1;
            break;
        }
        memcpy(buf + done * 512, xhci_bounce, (uint32_t)batch * 512);
        done += (uint32_t)batch;
    }
    spin_unlock_irqrestore(&xhci_lock, xhci_eflags);
    return rc;
}

int usb_write_sectors(int drive, uint32_t lba, int count, const uint8_t* buf) {
    int idx = drive - USB_DRIVE_BASE;
    if (!xhci_base || idx < 0 || idx >= USB_MAX_DRIVES) return -1;
    usb_dev_t* d = &xhci_devs[idx];
    if (!d->in_use || count < 1) return -1;
    if (count > 0 && lba + (uint32_t)count - 1 > d->max_lba) return -1;

    xhci_eflags = spin_lock_irqsave(&xhci_lock);
    hdd_activity = 10;
    int rc = 0;
    uint32_t done = 0;
    while (done < (uint32_t)count && rc == 0) {
        int batch = ata_batch_limit(lba + done, count - (int)done);
        memcpy(xhci_bounce, buf + done * 512, (uint32_t)batch * 512);
        if (usb_scsi_rw10(d, 1, lba + done, batch, xhci_bounce) < 0) {
            rc = -1;
            break;
        }
        done += (uint32_t)batch;
    }
    spin_unlock_irqrestore(&xhci_lock, xhci_eflags);
    return rc;
}
