// ============================================================
// VirtIO-GPU driver — modern (virtio 1.x) PCI transport, poll-only (v38.115)
// ============================================================
// Why this driver speaks MODERN virtio while virtio_blk.c speaks legacy PIO:
// measured on QEMU 8.2.2, virtio-gpu-pci has no legacy interface to speak to.
// In the same `info pci`, a virtio-blk-pci with `disable-modern=on` shows up
// as 1AF4:1001 with "BAR0: I/O at ..." — the legacy layout this kernel already
// drives — while virtio-gpu-pci (flag or no flag) stays 1AF4:1050 with BAR1
// (MSI-X) and BAR4 (the region block) and no I/O BAR at all. So the handshake
// here is the modern one: walk the device's vendor capabilities for the
// common-config / notify / ISR / device-config regions, negotiate the 64-bit
// feature word (accepting VIRTIO_F_VERSION_1, without which a modern device
// marks itself FAILED), publish the rings' physical addresses through the
// common config and notify with a memory write at
// notify_base + queue_notify_off * notify_off_multiplier.
//
// SeaBIOS places this device's 64-bit prefetchable BAR4 at 0xFE000000 — below
// 4 GB — and the capability walk below refuses any region it cannot address
// whole, rather than truncating an address and poking the wrong memory.
//
// Everything else follows virtio_blk.c: static .bss rings (the kernel's
// identity map makes guest-virtual == physical), one command at a time, poll
// the used ring, and keep the device from raising INTx with the available
// ring's NO_INTERRUPT flag — this kernel wires no IRQ for it and wants none.
//
// The driver's own self-test runs at init and is what makes the milestone
// checkable without a window: it builds a 128x128 test resource in guest
// memory, hands it to the device (create 2D -> attach backing -> transfer to
// host), puts it on scanout 0 and flushes it. The suite then asks QEMU's
// monitor for a screendump of THIS device's console and compares every pixel
// against the same pattern the driver wrote — guest memory -> host resource ->
// scanout -> pixels, end to end. With VIRGL negotiated it additionally probes
// the 3D readback path (transfer to host, poison the guest buffer, transfer
// back) and reports what the host answered; that is the path the next
// milestone's rendering will use, and a refusal here is information, not a
// failure.

#include "../include/virtio_gpu.h"
#include "../include/virtio.h"      // VIRTIO_S_* status bits, VIRTQ_DESC_F_*
#include "../include/pci.h"
#include "../include/serial.h"
#include "../include/utils.h"       // memset/memcpy

#define VGPU_QSIZE_MAX   64         // control queue; we are strictly serial
#define VGPU_QMEM        16384      // desc[] + avail + 4K-aligned used + slack
#define VGPU_CMD_MAX     256        // largest command (ctx_create): 96 bytes
#define VGPU_RESP_MAX    512        // GET_DISPLAY_INFO answers 408 bytes
#define VGPU_POLL_TRIES  4000000

#define VGPU_TEST_ID     1
#define VGPU_3D_ID       2
#define VGPU_CTX_ID      1
#define VGPU_TEST_W      128
#define VGPU_TEST_H      128
#define VGPU_TEST_PITCH  (VGPU_TEST_W * 4)
#define VGPU_TEST_BYTES  (VGPU_TEST_W * VGPU_TEST_H * 4)

typedef struct {
    int      in_use;
    uint8_t  bus, slot, func;
    volatile uint8_t* common;
    volatile uint8_t* notify;
    volatile uint8_t* isr;
    volatile uint8_t* devcfg;
    uint32_t notify_mul;
    uint16_t qsize;
    uint16_t notify_off;
    uint32_t avail_off, used_off;
    uint16_t last_used;             // poll cursor into the used ring
} vgpu_dev_t;

// 16-byte split-ring descriptor (same wire format for legacy and modern).
typedef struct {
    uint32_t addr_lo, addr_hi, len;
    uint16_t flags, next;
} vgpu_desc_t;

static vgpu_dev_t vgpu;
static virtio_gpu_info_t vgpu_pub;

static uint8_t vgpu_qmem[VGPU_QMEM]        __attribute__((aligned(4096)));
static uint8_t vgpu_cmd_buf[VGPU_CMD_MAX]  __attribute__((aligned(16)));
static uint8_t vgpu_resp_buf[VGPU_RESP_MAX] __attribute__((aligned(16)));
// The resource's backing store, and a second buffer for the 3D round trip.
static uint8_t vgpu_test_buf[VGPU_TEST_BYTES] __attribute__((aligned(4096)));
static uint8_t vgpu_read_buf[VGPU_TEST_BYTES] __attribute__((aligned(4096)));

// ---- small helpers ---------------------------------------------------------
static void vgpu_barrier(void) { __asm__ __volatile__("" ::: "memory"); }

static void vgpu_log(const char* s) { write_serial_string(s); }
static void vgpu_log_hex(uint32_t v) { write_serial_hex(v); }

// Decimal, because "scanout0=1280x800" reads like the q3 logs while
// "0x500x0x320" does not. No libc here, and 32-bit division (not 64) is what
// this kernel can do natively.
static void vgpu_log_dec(uint32_t v) {
    char b[12];
    int i = 11;
    b[i] = 0;
    if (v == 0) { vgpu_log("0"); return; }
    while (v > 0 && i > 0) { b[--i] = (char)('0' + (v % 10)); v /= 10; }
    vgpu_log(&b[i]);
}

// Word accessors that cannot be optimized into a wider or cached access: the
// rings are shared with the device, so every read of a device-written field
// goes through memcpy (opaque to the compiler, like virtio_blk.c's ring
// accessors) and every write is separated from the notification by a barrier.
static uint16_t vgpu_rd16(const void* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void vgpu_wr16(void* p, uint16_t v) { memcpy(p, &v, 2); }
static void vgpu_wr32(void* p, uint32_t v) { memcpy(p, &v, 4); }

static uint16_t vgpu_avail_idx(void) { return vgpu_rd16(&vgpu_qmem[vgpu.avail_off + 2]); }
static uint16_t vgpu_used_idx(void)  { return vgpu_rd16(&vgpu_qmem[vgpu.used_off + 2]); }
static uint32_t vgpu_used_id(void) {
    uint32_t v;
    memcpy(&v, &vgpu_qmem[vgpu.used_off + 4 + (uint32_t)(vgpu.last_used % vgpu.qsize) * 8], 4);
    return v;
}

// ---- common-config region --------------------------------------------------
static uint8_t  vgpu_cc8(uint32_t off) { return *(volatile uint8_t*)(vgpu.common + off); }
static void     vgpu_cc_w8(uint32_t off, uint8_t v) { *(volatile uint8_t*)(vgpu.common + off) = v; }
static uint16_t vgpu_cc16(uint32_t off) { return *(volatile uint16_t*)(vgpu.common + off); }
static void     vgpu_cc_w16(uint32_t off, uint16_t v) { *(volatile uint16_t*)(vgpu.common + off) = v; }
static uint32_t vgpu_cc32(uint32_t off) { return *(volatile uint32_t*)(vgpu.common + off); }
static void     vgpu_cc_w32(uint32_t off, uint32_t v) { *(volatile uint32_t*)(vgpu.common + off) = v; }

// ---- PCI config access (pci_read is dword-aligned only) --------------------
static uint8_t vgpu_pcfg8(uint8_t off) {
    uint32_t v = pci_read(vgpu.bus, vgpu.slot, vgpu.func, (uint8_t)(off & 0xFC));
    return (uint8_t)(v >> ((off & 3) * 8));
}
static uint16_t vgpu_pcfg16(uint8_t off) {
    uint32_t v = pci_read(vgpu.bus, vgpu.slot, vgpu.func, (uint8_t)(off & 0xFC));
    return (uint16_t)(v >> ((off & 3) * 8));
}
static uint32_t vgpu_pcfg32u(uint8_t off) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= ((uint32_t)vgpu_pcfg8((uint8_t)(off + i))) << (8 * i);
    return v;
}

// Assigned address of a BAR (SeaBIOS has programmed them by the time drivers
// run). Handles the 64-bit memory form by folding in the next BAR's dword.
static uint64_t vgpu_bar_addr(uint8_t bar) {
    uint8_t off = (uint8_t)(0x10 + bar * 4);
    uint32_t lo = pci_read(vgpu.bus, vgpu.slot, vgpu.func, off);
    if (lo & 1) return (uint64_t)(lo & ~3u);            // I/O BAR
    uint64_t base = (uint64_t)(lo & ~0xFu);
    if (((lo >> 1) & 3) == 2 && bar < 5) {              // 64-bit memory BAR
        uint32_t hi = pci_read(vgpu.bus, vgpu.slot, vgpu.func, (uint8_t)(off + 4));
        base |= ((uint64_t)hi) << 32;
    }
    return base;
}

// Walk the capability list for the four virtio regions. Nothing about the
// region layout is assumed: QEMU puts common/notify/ISR/device-config wherever
// its caps say (measured: all four inside BAR4, at +0x1000, +0x3000, +0x2000,
// +0x1000 offsets), and a driver that hardcoded BAR numbers would poke the
// MSI-X table on a device that arranges things differently.
static int vgpu_scan_caps(void) {
    if (!(vgpu_pcfg16(0x06) & (1u << 4))) return -1;    // no capability list
    uint8_t ptr = (uint8_t)(vgpu_pcfg8(0x34) & 0xFC);
    int guard = 0;
    while (ptr != 0 && guard++ < 48) {
        uint8_t id   = vgpu_pcfg8(ptr);
        uint8_t next = (uint8_t)(vgpu_pcfg8((uint8_t)(ptr + 1)) & 0xFC);
        uint8_t len  = vgpu_pcfg8((uint8_t)(ptr + 2));
        if (id == 0x09 && len >= 16) {                  // virtio vendor cap
            // struct virtio_pci_cap (uapi linux/virtio_pci.h): cap_vndr/next/len
            // at +0..+2, cfg_type at +3, bar at +4, then `id` and TWO padding
            // bytes, so offset lives at +8 — not right after `bar` — and length
            // at +12. A 20-byte capability is either a notify cap (whose extra
            // dword is notify_off_multiplier, at +16) or a cap64 (offset_hi at
            // +16, length_hi at +20). Reading these four bytes from the wrong
            // place is exactly how this driver first failed: it computed a
            // region address out of the padding and refused to map anything.
            uint8_t  type = vgpu_pcfg8((uint8_t)(ptr + 3));
            uint8_t  bar  = vgpu_pcfg8((uint8_t)(ptr + 4));
            uint64_t coff = (uint64_t)vgpu_pcfg32u((uint8_t)(ptr + 8));
            uint64_t clen = (uint64_t)vgpu_pcfg32u((uint8_t)(ptr + 12));
            if (len >= 20 && type != VIRTIO_PCI_CAP_NOTIFY_CFG) {
                coff |= ((uint64_t)vgpu_pcfg32u((uint8_t)(ptr + 16))) << 32;
                clen |= ((uint64_t)vgpu_pcfg32u((uint8_t)(ptr + 20))) << 32;
            }
            // Capabilities this driver does not use (QEMU also publishes the
            // PCI config-access cap, type 5, whose bar 0 / offset 0 is legal)
            // are skipped WITHOUT validating them as a region.
            if (type < VIRTIO_PCI_CAP_COMMON_CFG || type > VIRTIO_PCI_CAP_DEVICE_CFG) {
                ptr = next;
                continue;
            }
            uint64_t base = (bar < 6) ? vgpu_bar_addr(bar) : 0;
            uint64_t addr = base + coff;
            if (base == 0 || clen == 0 || addr >= 0x100000000ull ||
                addr + clen > 0x100000000ull) {
                vgpu_log("[VIRTIO-GPU] region unreachable for a 32-bit kernel (type=");
                vgpu_log_dec(type);
                vgpu_log(" bar=");
                vgpu_log_dec(bar);
                vgpu_log(")\n");
                return -1;
            }
            volatile uint8_t* p = (volatile uint8_t*)(uint32_t)addr;
            switch (type) {
                case VIRTIO_PCI_CAP_COMMON_CFG:
                    vgpu.common = p; vgpu_pub.region_common = (uint32_t)addr; break;
                case VIRTIO_PCI_CAP_NOTIFY_CFG:
                    vgpu.notify = p; vgpu_pub.region_notify = (uint32_t)addr;
                    if (len >= 20) vgpu.notify_mul = vgpu_pcfg32u((uint8_t)(ptr + 16));
                    break;
                case VIRTIO_PCI_CAP_ISR_CFG:
                    vgpu.isr = p; vgpu_pub.region_isr = (uint32_t)addr; break;
                case VIRTIO_PCI_CAP_DEVICE_CFG:
                    vgpu.devcfg = p; vgpu_pub.region_devcfg = (uint32_t)addr; break;
                default: break;
            }
        }
        ptr = next;
    }
    if (vgpu.common == 0 || vgpu.notify == 0 || vgpu.isr == 0 || vgpu.devcfg == 0) return -1;
    if (vgpu.notify_mul == 0) return -1;    // QEMU: 4; without it we cannot aim
    vgpu_pub.notify_mul = vgpu.notify_mul;  // gpustat reads the published copy
    return 0;
}

// Diagnostic dump for a failed bring-up: every virtio capability as the device
// reports it, plus the six BAR values, so a host that arranges its regions
// differently can be diagnosed from the log instead of from a rebuild.
static void vgpu_dump_caps(void) {
    if (vgpu_pcfg16(0x06) & (1u << 4)) {
        uint8_t ptr = (uint8_t)(vgpu_pcfg8(0x34) & 0xFC);
        int guard = 0;
        while (ptr != 0 && guard++ < 48) {
            if (vgpu_pcfg8(ptr) == 0x09) {
                vgpu_log("[VIRTIO-GPU] cap type=");
                vgpu_log_dec(vgpu_pcfg8((uint8_t)(ptr + 3)));
                vgpu_log(" bar=");
                vgpu_log_dec(vgpu_pcfg8((uint8_t)(ptr + 4)));
                vgpu_log(" off=");
                vgpu_log_hex(vgpu_pcfg32u((uint8_t)(ptr + 8)));
                vgpu_log(" len=");
                vgpu_log_hex(vgpu_pcfg32u((uint8_t)(ptr + 12)));
                vgpu_log(" caplen=");
                vgpu_log_dec(vgpu_pcfg8((uint8_t)(ptr + 2)));
                vgpu_log("\n");
            }
            ptr = (uint8_t)(vgpu_pcfg8((uint8_t)(ptr + 1)) & 0xFC);
        }
    } else {
        vgpu_log("[VIRTIO-GPU] no capability list\n");
    }
    for (uint8_t b = 0; b < 6; b++) {
        vgpu_log("[VIRTIO-GPU] bar");
        vgpu_log_dec(b);
        vgpu_log("=");
        vgpu_log_hex(pci_read(vgpu.bus, vgpu.slot, vgpu.func, (uint8_t)(0x10 + b * 4)));
        vgpu_log("\n");
    }
}

// ---- feature negotiation (64-bit) -----------------------------------------
// Only two device bits and the mandatory VERSION_1 are ever accepted: taking a
// feature we do not implement (ACCESS_PLATFORM, RING_EVENT_IDX, the blob/
// context-init extensions) is how a driver ends up owning a protocol it never
// wrote.
static int vgpu_negotiate(void) {
    vgpu_cc_w32(VGPU_CC_DEV_FEAT_SEL, 0);
    uint32_t lo = vgpu_cc32(VGPU_CC_DEV_FEAT);
    vgpu_cc_w32(VGPU_CC_DEV_FEAT_SEL, 1);
    uint32_t hi = vgpu_cc32(VGPU_CC_DEV_FEAT);
    vgpu_pub.dev_feat_lo = lo;
    vgpu_pub.dev_feat_hi = hi;
    if (!(hi & 1u)) return -1;              // bit 32: a modern device must offer it
    uint32_t glo = 0, ghi = 1u;             // accept VIRTIO_F_VERSION_1
    if (lo & (1u << VIRTIO_GPU_F_VIRGL)) { glo |= (1u << VIRTIO_GPU_F_VIRGL); vgpu_pub.virgl = 1; }
    if (lo & (1u << VIRTIO_GPU_F_EDID))  { glo |= (1u << VIRTIO_GPU_F_EDID);  vgpu_pub.edid  = 1; }
    vgpu_cc_w32(VGPU_CC_DRV_FEAT_SEL, 0); vgpu_cc_w32(VGPU_CC_DRV_FEAT, glo);
    vgpu_cc_w32(VGPU_CC_DRV_FEAT_SEL, 1); vgpu_cc_w32(VGPU_CC_DRV_FEAT, ghi);
    return 0;
}

// ---- queue 0 (the control queue) ------------------------------------------
static int vgpu_queue_init(void) {
    vgpu_cc_w16(VGPU_CC_QUEUE_SEL, 0);
    uint16_t max = vgpu_cc16(VGPU_CC_QUEUE_SIZE);
    if (max < 8) return -1;
    uint16_t qsz = (max > VGPU_QSIZE_MAX) ? VGPU_QSIZE_MAX : max;
    vgpu_cc_w16(VGPU_CC_QUEUE_SIZE, qsz);               // tell the device our size
    vgpu_cc_w16(VGPU_CC_QUEUE_MSIX_VEC, VGPU_MSI_NO_VECTOR);

    vgpu.qsize = qsz;
    vgpu.avail_off = (uint32_t)qsz * 16;
    vgpu.used_off = (vgpu.avail_off + 6 + (uint32_t)qsz * 2 + 4095u) & ~4095u;
    if (vgpu.used_off + 6 + (uint32_t)qsz * 8 > VGPU_QMEM) return -1;
    memset(vgpu_qmem, 0, VGPU_QMEM);
    // Poll-only: tell the device not to raise INTx when it consumes a buffer.
    uint16_t noint = VIRTQ_AVAIL_F_NO_INTERRUPT;
    vgpu_wr16(&vgpu_qmem[vgpu.avail_off], noint);
    vgpu_barrier();

    uint32_t desc = (uint32_t)(uintptr_t)vgpu_qmem;
    uint64_t avail = (uint64_t)desc + vgpu.avail_off;
    uint64_t used  = (uint64_t)desc + vgpu.used_off;
    vgpu_cc_w32(VGPU_CC_QUEUE_DESC, desc);
    vgpu_cc_w32(VGPU_CC_QUEUE_DESC + 4, 0);
    vgpu_cc_w32(VGPU_CC_QUEUE_AVAIL, (uint32_t)avail);
    vgpu_cc_w32(VGPU_CC_QUEUE_AVAIL + 4, (uint32_t)(avail >> 32));
    vgpu_cc_w32(VGPU_CC_QUEUE_USED, (uint32_t)used);
    vgpu_cc_w32(VGPU_CC_QUEUE_USED + 4, (uint32_t)(used >> 32));
    vgpu_cc_w16(VGPU_CC_QUEUE_ENABLE, 1);
    vgpu.notify_off = vgpu_cc16(VGPU_CC_QUEUE_NOTIFY_OFF);   // valid after enable
    vgpu_pub.qsize = qsz;
    vgpu_pub.notify_off = vgpu.notify_off;
    vgpu_pub.num_queues = vgpu_cc16(VGPU_CC_NUM_QUEUES);
    return 0;
}

static int vgpu_hw_init(void) {
    // Memory decoding + bus master before the first region access.
    uint32_t cmd = pci_read(vgpu.bus, vgpu.slot, vgpu.func, 0x04);
    pci_write(vgpu.bus, vgpu.slot, vgpu.func, 0x04, cmd | 0x6);
    if (vgpu_scan_caps() != 0) return -1;

    vgpu_cc_w8(VGPU_CC_DEV_STATUS, 0);                  // reset
    vgpu_cc_w8(VGPU_CC_DEV_STATUS, VIRTIO_S_ACK);
    vgpu_cc_w8(VGPU_CC_DEV_STATUS, VIRTIO_S_ACK | VIRTIO_S_DRIVER);
    if (vgpu_negotiate() != 0) return -1;
    vgpu_cc_w8(VGPU_CC_DEV_STATUS, VIRTIO_S_ACK | VIRTIO_S_DRIVER | VIRTIO_S_FEATURES_OK);
    if (!(vgpu_cc8(VGPU_CC_DEV_STATUS) & VIRTIO_S_FEATURES_OK)) return -1;
    if (vgpu_queue_init() != 0) return -1;
    vgpu_cc_w8(VGPU_CC_DEV_STATUS,
               VIRTIO_S_ACK | VIRTIO_S_DRIVER | VIRTIO_S_FEATURES_OK | VIRTIO_S_DRIVER_OK);
    (void)*(volatile uint8_t*)vgpu.isr;                 // ack any stale status
    return 0;
}

static void vgpu_notify_queue0(void) {
    // The address identifies the queue; the value written is the queue index
    // (or queue_notify_data when VIRTIO_F_NOTIF_CONFIG_DATA is negotiated,
    // which this driver does not take). Queue 0 makes both readings agree.
    *(volatile uint32_t*)(vgpu.notify + (uint32_t)vgpu.notify_off * vgpu.notify_mul) = 0;
}

// One command descriptor + one response descriptor, head 0, then poll. Returns
// 0 on success, -1 on timeout, -(0x1200..0x1205) when the device refused.
static int vgpu_submit(uint32_t cmd_len, uint32_t resp_len) {
    vgpu_desc_t* d = (vgpu_desc_t*)vgpu_qmem;
    memset(vgpu_resp_buf, 0, resp_len);
    vgpu_barrier();

    d[0].addr_lo = (uint32_t)(uintptr_t)vgpu_cmd_buf;
    d[0].addr_hi = 0;
    d[0].len = cmd_len;
    d[0].flags = VIRTQ_DESC_F_NEXT;
    d[0].next = 1;
    d[1].addr_lo = (uint32_t)(uintptr_t)vgpu_resp_buf;
    d[1].addr_hi = 0;
    d[1].len = resp_len;
    d[1].flags = VIRTQ_DESC_F_WRITE;
    d[1].next = 0;

    uint16_t idx = vgpu_avail_idx();
    vgpu_wr16(&vgpu_qmem[vgpu.avail_off + 4 + (uint32_t)(idx % vgpu.qsize) * 2], 0);
    vgpu_barrier();
    vgpu_wr16(&vgpu_qmem[vgpu.avail_off + 2], (uint16_t)(idx + 1));
    vgpu_barrier();
    vgpu_notify_queue0();

    int t = VGPU_POLL_TRIES;
    while (--t > 0) {
        if (vgpu_used_idx() != vgpu.last_used) break;
    }
    if (t == 0) return -1;
    vgpu_barrier();
    if (vgpu_used_id() != 0) return -1;                 // not our head
    vgpu.last_used++;
    (void)*(volatile uint8_t*)vgpu.isr;                 // keep INTx quiet

    uint32_t rtype = ((vgpu_ctrl_hdr_t*)vgpu_resp_buf)->type;
    if (rtype >= VGPU_RESP_ERR_UNSPEC && rtype <= VGPU_RESP_ERR_INVALID_PARAMETER)
        return -(int)rtype;
    if (rtype < VGPU_RESP_OK_NODATA || rtype > 0x1106) return -2;
    return 0;
}

static vgpu_ctrl_hdr_t* vgpu_begin(uint32_t type) {
    vgpu_ctrl_hdr_t* c = (vgpu_ctrl_hdr_t*)vgpu_cmd_buf;
    c->type = type;
    c->flags = 0;
    c->fence_id = 0;
    c->ctx_id = 0;
    c->ring_idx = 0;
    c->padding[0] = 0;
    c->padding[1] = 0;
    c->padding[2] = 0;
    return c;
}

// ---- 2D commands -----------------------------------------------------------
static int vgpu_create_2d(uint32_t id, uint32_t fmt, uint32_t w, uint32_t h) {
    vgpu_cmd_resource_create_2d_t* c =
        (vgpu_cmd_resource_create_2d_t*)vgpu_begin(VGPU_CMD_RESOURCE_CREATE_2D);
    c->resource_id = id; c->format = fmt; c->width = w; c->height = h;
    return vgpu_submit(sizeof(*c), sizeof(vgpu_ctrl_hdr_t));
}

static int vgpu_attach_backing(uint32_t id, const void* buf, uint32_t len) {
    uint8_t tmp[sizeof(vgpu_cmd_attach_backing_t) + sizeof(vgpu_mem_entry_t)];
    vgpu_cmd_attach_backing_t* c = (vgpu_cmd_attach_backing_t*)tmp;
    vgpu_mem_entry_t* e = (vgpu_mem_entry_t*)(tmp + sizeof(*c));
    vgpu_begin(VGPU_CMD_RESOURCE_ATTACH_BACKING);
    memcpy(tmp, vgpu_cmd_buf, sizeof(*c));
    c->resource_id = id;
    c->nr_entries = 1;
    e->addr = (uint64_t)(uintptr_t)buf;
    e->length = len;
    e->padding = 0;
    memcpy(vgpu_cmd_buf, tmp, sizeof(tmp));
    return vgpu_submit((uint32_t)sizeof(tmp), sizeof(vgpu_ctrl_hdr_t));
}

static int vgpu_transfer_to_host_2d(uint32_t id, uint32_t w, uint32_t h) {
    vgpu_cmd_transfer_to_host_2d_t* c =
        (vgpu_cmd_transfer_to_host_2d_t*)vgpu_begin(VGPU_CMD_TRANSFER_TO_HOST_2D);
    c->r.x = 0; c->r.y = 0; c->r.width = w; c->r.height = h;
    c->offset = 0;
    c->resource_id = id;
    c->padding = 0;
    return vgpu_submit(sizeof(*c), sizeof(vgpu_ctrl_hdr_t));
}

static int vgpu_set_scanout(uint32_t scanout, uint32_t id, uint32_t w, uint32_t h) {
    vgpu_cmd_set_scanout_t* c = (vgpu_cmd_set_scanout_t*)vgpu_begin(VGPU_CMD_SET_SCANOUT);
    c->r.x = 0; c->r.y = 0; c->r.width = w; c->r.height = h;
    c->scanout_id = scanout;
    c->resource_id = id;
    return vgpu_submit(sizeof(*c), sizeof(vgpu_ctrl_hdr_t));
}

static int vgpu_flush(uint32_t id, uint32_t w, uint32_t h) {
    vgpu_cmd_resource_flush_t* c =
        (vgpu_cmd_resource_flush_t*)vgpu_begin(VGPU_CMD_RESOURCE_FLUSH);
    c->r.x = 0; c->r.y = 0; c->r.width = w; c->r.height = h;
    c->resource_id = id;
    c->padding = 0;
    return vgpu_submit(sizeof(*c), sizeof(vgpu_ctrl_hdr_t));
}

// ---- 3D (virgl) commands ---------------------------------------------------
static int vgpu_ctx_create(uint32_t ctx_id, uint32_t capset_id) {
    vgpu_cmd_ctx_create_t* c = (vgpu_cmd_ctx_create_t*)vgpu_begin(VGPU_CMD_CTX_CREATE);
    c->hdr.ctx_id = ctx_id;
    const char* name = "mectov";
    int n = 0;
    while (name[n] != 0) { c->debug_name[n] = name[n]; n++; }
    c->debug_name[n] = 0;
    c->nlen = (uint32_t)(n + 1);
    c->context_init = capset_id;        // VIRGL2 when the device offers it
    return vgpu_submit(sizeof(*c), sizeof(vgpu_ctrl_hdr_t));
}

static int vgpu_ctx_attach(uint32_t ctx_id, uint32_t res_id) {
    vgpu_cmd_ctx_resource_t* c =
        (vgpu_cmd_ctx_resource_t*)vgpu_begin(VGPU_CMD_CTX_ATTACH_RESOURCE);
    c->hdr.ctx_id = ctx_id;
    c->resource_id = res_id;
    c->padding = 0;
    return vgpu_submit(sizeof(*c), sizeof(vgpu_ctrl_hdr_t));
}

static int vgpu_create_3d(uint32_t res_id, uint32_t ctx_id, uint32_t fmt,
                          uint32_t w, uint32_t h) {
    vgpu_cmd_resource_create_3d_t* c =
        (vgpu_cmd_resource_create_3d_t*)vgpu_begin(VGPU_CMD_RESOURCE_CREATE_3D);
    c->hdr.ctx_id = ctx_id;
    c->resource_id = res_id;
    c->target = VGPU_TARGET_2D;
    c->format = fmt;
    c->bind = VGPU_BIND_RENDER_TARGET | VGPU_BIND_SAMPLER_VIEW |
              VGPU_BIND_TRANSFER_TO_HOST | VGPU_BIND_TRANSFER_FROM_HOST;
    c->width = w; c->height = h;
    c->depth = 1;
    c->array_size = 1;
    c->last_level = 0;
    c->nr_samples = 0;
    c->flags = 0;
    c->padding = 0;
    return vgpu_submit(sizeof(*c), sizeof(vgpu_ctrl_hdr_t));
}

static int vgpu_transfer_host_3d(uint32_t type, uint32_t res_id, uint32_t ctx_id,
                                 uint32_t w, uint32_t h) {
    vgpu_cmd_transfer_host_3d_t* c =
        (vgpu_cmd_transfer_host_3d_t*)vgpu_begin(type);
    c->hdr.ctx_id = ctx_id;
    c->box.x = 0; c->box.y = 0; c->box.z = 0;
    c->box.w = w; c->box.h = h; c->box.d = 1;
    c->offset = 0;
    c->resource_id = res_id;
    c->level = 0;
    c->stride = VGPU_TEST_PITCH;
    c->layer_stride = VGPU_TEST_PITCH * h;
    return vgpu_submit(sizeof(*c), sizeof(vgpu_ctrl_hdr_t));
}

// ---- the pattern the suite checks pixels against ---------------------------
// 16x16 blocks: a magenta marker at the origin (so a screenshot can be located
// unambiguously), then a red/blue checkerboard. Written as 0x00RRGGBB words
// into a B8G8R8X8 resource, which is the byte order this kernel's own
// framebuffer code uses.
static uint32_t vgpu_pattern_pixel(int x, int y) {
    if (x < 16 && y < 16) return 0x00FF00FFu;                       // magenta
    return ((((x >> 4) + (y >> 4)) & 1) != 0) ? 0x00FF0000u         // red
                                             : 0x000000FFu;         // blue
}

static void vgpu_fill_pattern(uint8_t* buf) {
    for (int y = 0; y < VGPU_TEST_H; y++) {
        for (int x = 0; x < VGPU_TEST_W; x++) {
            vgpu_wr32(&buf[(y * VGPU_TEST_W + x) * 4], vgpu_pattern_pixel(x, y));
        }
    }
}

static uint32_t vgpu_fnv1a(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

// ---- init-time queries -----------------------------------------------------
static void vgpu_query_config(void) {
    volatile vgpu_config_t* cfg = (volatile vgpu_config_t*)vgpu.devcfg;
    vgpu_pub.num_scanouts = cfg->num_scanouts;
    vgpu_pub.num_capsets = cfg->num_capsets;
}

static void vgpu_query_display(void) {
    vgpu_begin(VGPU_CMD_GET_DISPLAY_INFO);
    if (vgpu_submit(sizeof(vgpu_ctrl_hdr_t), sizeof(vgpu_resp_display_info_t)) != 0) return;
    vgpu_display_one_t* p = &((vgpu_resp_display_info_t*)vgpu_resp_buf)->pmodes[0];
    vgpu_pub.scanout_w = p->r.width;
    vgpu_pub.scanout_h = p->r.height;
    vgpu_pub.scanout_enabled = (int)p->enabled;
}

static void vgpu_query_capset(void) {
    if (vgpu_pub.num_capsets == 0) return;
    vgpu_cmd_get_capset_info_t* c =
        (vgpu_cmd_get_capset_info_t*)vgpu_begin(VGPU_CMD_GET_CAPSET_INFO);
    c->capset_index = 0;                    // VIRGL2 in QEMU; VIRGL when older
    c->padding = 0;
    if (vgpu_submit(sizeof(*c), sizeof(vgpu_resp_capset_info_t)) != 0) return;
    vgpu_resp_capset_info_t* r = (vgpu_resp_capset_info_t*)vgpu_resp_buf;
    vgpu_pub.capset_id = r->capset_id;
    vgpu_pub.capset_version = r->capset_max_version;
    vgpu_pub.capset_size = r->capset_max_size;
    vgpu_pub.capset_ok = (r->capset_id != 0) ? 1 : 0;
}

// ---- self-test: guest memory -> host resource -> scanout ------------------
// Returns 0 when every step was acknowledged; on failure the caller logs the
// step name and the device's error code.
static const char* vgpu_selftest_2d(void) {
    vgpu_fill_pattern(vgpu_test_buf);
    vgpu_pub.selftest_ck = vgpu_fnv1a(vgpu_test_buf, VGPU_TEST_BYTES);
    vgpu_pub.selftest_w = VGPU_TEST_W;
    vgpu_pub.selftest_h = VGPU_TEST_H;

    if (vgpu_create_2d(VGPU_TEST_ID, VGPU_FORMAT_B8G8R8X8_UNORM,
                       VGPU_TEST_W, VGPU_TEST_H) != 0) return "create-2d";
    vgpu_pub.selftest_res = VGPU_TEST_ID;
    if (vgpu_attach_backing(VGPU_TEST_ID, vgpu_test_buf, VGPU_TEST_BYTES) != 0)
        return "attach-backing";
    if (vgpu_transfer_to_host_2d(VGPU_TEST_ID, VGPU_TEST_W, VGPU_TEST_H) != 0)
        return "transfer-to-host";
    if (vgpu_set_scanout(0, VGPU_TEST_ID, VGPU_TEST_W, VGPU_TEST_H) != 0)
        return "set-scanout";
    if (vgpu_flush(VGPU_TEST_ID, VGPU_TEST_W, VGPU_TEST_H) != 0) return "flush";
    // The resource stays alive and scanned out on purpose: the milestone's
    // evidence is a screendump of THIS device's console, and the pixels have
    // to still be there when the host asks for them.
    vgpu_pub.selftest_ok = 1;
    return 0;
}

// ---- optional probe: 3D readback (the path the renderer will need) --------
// transfer-to-host copies guest backing -> host resource; we then poison the
// guest buffer and transfer-from-host copies it back. Equality proves the
// bytes really travelled through the host's resource, byte for byte, without
// needing a screenshot. A refusal is reported, never fatal: the milestone that
// must pass on this path is the renderer's, not this driver's.
static void vgpu_probe_3d(void) {
    if (!vgpu_pub.virgl) {
        vgpu_log("[VIRTIO-GPU] readback: 3d=skip no-virgl-feature\n");
        return;
    }
    const char* step = "ctx-create";
    uint32_t capset = (vgpu_pub.capset_id == VGPU_VIRGL2_CAPSET) ? VGPU_VIRGL2_CAPSET
                                                                 : VGPU_VIRGL_CAPSET;
    int rc = vgpu_ctx_create(VGPU_CTX_ID, capset);
    if (rc != 0) goto refused;

    // Resources are context-independent (Linux creates them with ctx_id 0);
    // if this host insists on a live context, ask again with ours.
    step = "res-create-3d";
    rc = vgpu_create_3d(VGPU_3D_ID, 0, VGPU_FORMAT_B8G8R8A8_UNORM, VGPU_TEST_W, VGPU_TEST_H);
    if (rc != 0) {
        step = "res-create-3d-ctx1";
        rc = vgpu_create_3d(VGPU_3D_ID, VGPU_CTX_ID, VGPU_FORMAT_B8G8R8A8_UNORM,
                            VGPU_TEST_W, VGPU_TEST_H);
    }
    if (rc != 0) goto refused;

    step = "attach-backing";
    rc = vgpu_attach_backing(VGPU_3D_ID, vgpu_read_buf, VGPU_TEST_BYTES);
    if (rc != 0) goto refused;

    memcpy(vgpu_read_buf, vgpu_test_buf, VGPU_TEST_BYTES);
    step = "transfer-to-host-3d";
    rc = vgpu_transfer_host_3d(VGPU_CMD_TRANSFER_TO_HOST_3D, VGPU_3D_ID, 0,
                               VGPU_TEST_W, VGPU_TEST_H);
    if (rc != 0) goto refused;

    memset(vgpu_read_buf, 0xCD, VGPU_TEST_BYTES);       // nothing may survive on its own
    step = "transfer-from-host-3d";
    rc = vgpu_transfer_host_3d(VGPU_CMD_TRANSFER_FROM_HOST_3D, VGPU_3D_ID, 0,
                               VGPU_TEST_W, VGPU_TEST_H);
    if (rc != 0) goto refused;

    vgpu_pub.probe3d_ck = vgpu_fnv1a(vgpu_read_buf, VGPU_TEST_BYTES);
    if (vgpu_pub.probe3d_ck == vgpu_pub.selftest_ck) {
        vgpu_pub.probe3d = 1;
        vgpu_log("[VIRTIO-GPU] readback: 3d=ok ck=");
        vgpu_log_hex(vgpu_pub.probe3d_ck);
        vgpu_log("\n");
    } else {
        vgpu_pub.probe3d = 3;                           // travelled, but changed
        vgpu_log("[VIRTIO-GPU] readback: 3d=mismatch got=");
        vgpu_log_hex(vgpu_pub.probe3d_ck);
        vgpu_log(" want=");
        vgpu_log_hex(vgpu_pub.selftest_ck);
        vgpu_log("\n");
    }

    // Attaching to the context is the last step because it is what the
    // renderer needs next; its result is reported but cannot undo the
    // readback proof above.
    step = "ctx-attach-resource";
    rc = vgpu_ctx_attach(VGPU_CTX_ID, VGPU_3D_ID);
    if (rc != 0) {
        vgpu_log("[VIRTIO-GPU] ctx attach: err code=");
        vgpu_log_hex((uint32_t)(-rc));
        vgpu_log("\n");
    }
    return;

refused:
    vgpu_pub.probe3d = 2;
    vgpu_pub.probe3d_err = (uint32_t)(-rc);
    vgpu_log("[VIRTIO-GPU] readback: 3d=err step=");
    vgpu_log(step);
    vgpu_log(" code=");
    vgpu_log_hex(vgpu_pub.probe3d_err);
    vgpu_log("\n");
}

// ---- public entry points ---------------------------------------------------
void virtio_gpu_init(void) {
    memset(&vgpu, 0, sizeof(vgpu));
    memset(&vgpu_pub, 0, sizeof(vgpu_pub));

    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t* p = &pci_devices[i];
        if (p->vendor_id != VIRTIO_PCI_VENDOR) continue;
        if (p->device_id != VIRTIO_PCI_DEV_GPU_MODERN) continue;
        vgpu.bus = p->bus; vgpu.slot = p->slot; vgpu.func = p->func;

        vgpu_log("[VIRTIO-GPU] pci 1af4:1050 bus=");
        vgpu_log_dec(p->bus);
        vgpu_log(" slot=");
        vgpu_log_dec(p->slot);
        vgpu_log(" fn=");
        vgpu_log_dec(p->func);
        vgpu_log("\n");

        if (vgpu_hw_init() != 0) {
            vgpu_dump_caps();
            vgpu_log("[VIRTIO-GPU] init failed\n");
            return;
        }
        vgpu.in_use = 1;
        vgpu_pub.present = 1;

        vgpu_log("[VIRTIO-GPU] regions common="); vgpu_log_hex(vgpu_pub.region_common);
        vgpu_log(" notify=");  vgpu_log_hex(vgpu_pub.region_notify);
        vgpu_log(" isr=");     vgpu_log_hex(vgpu_pub.region_isr);
        vgpu_log(" devcfg=");  vgpu_log_hex(vgpu_pub.region_devcfg);
        vgpu_log(" notify_mul="); vgpu_log_dec(vgpu.notify_mul);
        vgpu_log("\n");

        vgpu_log("[VIRTIO-GPU] features dev_lo="); vgpu_log_hex(vgpu_pub.dev_feat_lo);
        vgpu_log(" dev_hi=");  vgpu_log_hex(vgpu_pub.dev_feat_hi);
        vgpu_log(" virgl=");   vgpu_log_dec((uint32_t)vgpu_pub.virgl);
        vgpu_log(" edid=");    vgpu_log_dec((uint32_t)vgpu_pub.edid);
        vgpu_log(" qsize=");   vgpu_log_dec(vgpu_pub.qsize);
        vgpu_log(" notify_off="); vgpu_log_dec(vgpu_pub.notify_off);
        vgpu_log("\n");

        vgpu_query_config();
        vgpu_query_display();
        vgpu_log("[VIRTIO-GPU] display scanout0=");
        vgpu_log_dec(vgpu_pub.scanout_w);
        vgpu_log("x");
        vgpu_log_dec(vgpu_pub.scanout_h);
        vgpu_log(" enabled=");
        vgpu_log_dec((uint32_t)vgpu_pub.scanout_enabled);
        vgpu_log(" num_scanouts=");
        vgpu_log_dec(vgpu_pub.num_scanouts);
        vgpu_log(" num_capsets=");
        vgpu_log_dec(vgpu_pub.num_capsets);
        vgpu_log("\n");

        vgpu_query_capset();
        if (vgpu_pub.capset_ok) {
            vgpu_log("[VIRTIO-GPU] capset0 id=");
            vgpu_log_dec(vgpu_pub.capset_id);
            vgpu_log(" ver=");
            vgpu_log_dec(vgpu_pub.capset_version);
            vgpu_log(" size=");
            vgpu_log_hex(vgpu_pub.capset_size);
            vgpu_log("\n");
        } else {
            vgpu_log("[VIRTIO-GPU] capset0 none\n");
        }

        const char* bad = vgpu_selftest_2d();
        if (bad == 0) {
            vgpu_log("[VIRTIO-GPU] selftest: res=");
            vgpu_log_hex(vgpu_pub.selftest_res);
            vgpu_log(" dim=");
            vgpu_log_dec(vgpu_pub.selftest_w);
            vgpu_log("x");
            vgpu_log_dec(vgpu_pub.selftest_h);
            vgpu_log(" fmt=2 attach=ok transfer=ok scanout=ok flush=ok ck=");
            vgpu_log_hex(vgpu_pub.selftest_ck);
            vgpu_log("\n");
            vgpu_probe_3d();
        } else {
            vgpu_log("[VIRTIO-GPU] selftest: FAIL step=");
            vgpu_log(bad);
            vgpu_log("\n");
        }
        vgpu_log("[VIRTIO-GPU] ready\n");
        return;
    }
    vgpu_log("[VIRTIO-GPU] no modern gpu device (PCI)\n");
}

int virtio_gpu_present(void) { return vgpu_pub.present; }

const virtio_gpu_info_t* virtio_gpu_info(void) { return &vgpu_pub; }

// The serial mirror of the `gpustat` command: one line a headless suite can
// assert, decimal for the fields a human reads and hex for checksums.
// Emitted by the command itself, so the assertion also proves the command ran
// (and not merely that the driver initialised at boot).
void virtio_gpu_log_status(void) {
    vgpu_log("[GPU] gpustat: ");
    if (!vgpu_pub.present) {
        vgpu_log("absent\n");
        return;
    }
    vgpu_log("bus=");
    vgpu_log_dec(vgpu.bus);
    vgpu_log(" slot=");
    vgpu_log_dec(vgpu.slot);
    vgpu_log(" virgl=");
    vgpu_log_dec((uint32_t)vgpu_pub.virgl);
    vgpu_log(" edid=");
    vgpu_log_dec((uint32_t)vgpu_pub.edid);
    vgpu_log(" selftest=");
    vgpu_log(vgpu_pub.selftest_ok ? "ok" : "fail");
    vgpu_log(" ck=");
    vgpu_log_hex(vgpu_pub.selftest_ck);
    vgpu_log(" scanout=");
    vgpu_log_dec(vgpu_pub.scanout_w);
    vgpu_log("x");
    vgpu_log_dec(vgpu_pub.scanout_h);
    vgpu_log(" qsize=");
    vgpu_log_dec(vgpu_pub.qsize);
    vgpu_log(" capsets=");
    vgpu_log_dec(vgpu_pub.num_capsets);
    vgpu_log(" readback=");
    switch (vgpu_pub.probe3d) {
        case 1:  vgpu_log("ok");       break;
        case 2:  vgpu_log("refused");  break;
        case 3:  vgpu_log("mismatch"); break;
        default: vgpu_log("skip");     break;
    }
    vgpu_log("\n");
}
