#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include "types.h"

// ============================================================
// VirtIO-GPU — protocol definitions + driver interface (v38.115)
// ============================================================
// This device is NOT driven the way virtio_blk.c drives its own: measured on
// QEMU 8.2.2, `virtio-gpu-pci` (1AF4:1050) has no legacy interface at all.
// `info pci` shows no I/O BAR0 for it even with `disable-modern=on` (where a
// virtio-blk device in the same run DOES appear as 1AF4:1001 with BAR0 I/O),
// and its two implemented BARs are BAR1 (MSI-X) and BAR4 (the region block) —
// the modern virtio-pci layout. So this driver speaks modern virtio 1.x:
// walk the device's vendor-specific PCI capabilities for the common-config,
// notify, ISR and device-config regions, negotiate the 64-bit feature word
// (a modern device requires VIRTIO_F_VERSION_1), publish the ring addresses
// into the common config and notify with a memory write.
//
// Measured on this host's boot path: SeaBIOS places the 64-bit prefetchable
// BAR4 at 0xFE000000 — below 4 GB — so a 32-bit kernel can address every
// region it needs (the driver re-checks that and refuses anything above 4 GB
// rather than silently truncating).
//
// The wire structures below are transcribed from the kernel's
// include/uapi/linux/virtio_gpu.h (BSD-licensed by its authors), which mirrors
// the virtio 1.2 specification §5.7 field for field.

// ---- PCI identity ----
#define VIRTIO_PCI_DEV_GPU_MODERN 0x1050    // display controller, modern layout

// ---- feature bits (device-offered, negotiation is 64-bit) ----
#define VIRTIO_GPU_F_VIRGL    0             // 3D commands / virgin capsets
#define VIRTIO_GPU_F_EDID     1             // GET_EDID
#define VIRTIO_F_VERSION_1    32            // mandatory for a modern device

// ---- vendor-specific PCI capability types (config space cap id 0x09) ----
#define VIRTIO_PCI_CAP_COMMON_CFG  1        // device_feature/queue_* registers
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2        // queue notification area
#define VIRTIO_PCI_CAP_ISR_CFG     3        // interrupt status byte
#define VIRTIO_PCI_CAP_DEVICE_CFG  4        // virtio_gpu_config (5 u32)

// ---- common-config register offsets (virtio 1.2 §4.1.4.3) ----
#define VGPU_CC_DEV_FEAT_SEL     0x00
#define VGPU_CC_DEV_FEAT         0x04
#define VGPU_CC_DRV_FEAT_SEL     0x08
#define VGPU_CC_DRV_FEAT         0x0C
#define VGPU_CC_MSIX_CONFIG      0x10       // u16
#define VGPU_CC_NUM_QUEUES       0x12       // u16
#define VGPU_CC_DEV_STATUS       0x14       // u8
#define VGPU_CC_CFG_GENERATION   0x15       // u8
#define VGPU_CC_QUEUE_SEL        0x16       // u16
#define VGPU_CC_QUEUE_SIZE       0x18       // u16 (max in, chosen size out)
#define VGPU_CC_QUEUE_MSIX_VEC   0x1A       // u16
#define VGPU_CC_QUEUE_ENABLE     0x1C       // u16
#define VGPU_CC_QUEUE_NOTIFY_OFF 0x1E       // u16, valid after enable
#define VGPU_CC_QUEUE_DESC       0x20       // u64
#define VGPU_CC_QUEUE_AVAIL      0x28       // u64
#define VGPU_CC_QUEUE_USED       0x30       // u64

#define VGPU_MSI_NO_VECTOR       0xFFFF     // "do not use MSI-X for this queue"

// ---- control command types ----
#define VGPU_CMD_GET_DISPLAY_INFO       0x0100
#define VGPU_CMD_RESOURCE_CREATE_2D     0x0101
#define VGPU_CMD_RESOURCE_UNREF         0x0102
#define VGPU_CMD_SET_SCANOUT            0x0103
#define VGPU_CMD_RESOURCE_FLUSH         0x0104
#define VGPU_CMD_TRANSFER_TO_HOST_2D    0x0105
#define VGPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VGPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define VGPU_CMD_GET_CAPSET_INFO        0x0108
#define VGPU_CMD_GET_CAPSET             0x0109
#define VGPU_CMD_RESOURCE_CREATE_3D     0x0204
#define VGPU_CMD_TRANSFER_TO_HOST_3D    0x0205
#define VGPU_CMD_TRANSFER_FROM_HOST_3D  0x0206
#define VGPU_CMD_CTX_CREATE             0x0200
#define VGPU_CMD_CTX_DESTROY            0x0201
#define VGPU_CMD_CTX_ATTACH_RESOURCE    0x0202

#define VGPU_RESP_OK_NODATA             0x1100
#define VGPU_RESP_OK_DISPLAY_INFO       0x1101
#define VGPU_RESP_OK_CAPSET_INFO        0x1102
#define VGPU_RESP_OK_CAPSET             0x1103
#define VGPU_RESP_ERR_UNSPEC            0x1200
#define VGPU_RESP_ERR_OUT_OF_MEMORY     0x1201
#define VGPU_RESP_ERR_INVALID_SCANOUT_ID 0x1202
#define VGPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VGPU_RESP_ERR_INVALID_CONTEXT_ID 0x1204
#define VGPU_RESP_ERR_INVALID_PARAMETER 0x1205

// ---- 2D formats (only what a scanout/test pattern needs) ----
#define VGPU_FORMAT_B8G8R8A8_UNORM  1       // byte order B,G,R,A  -> u32 0xAARRGGBB
#define VGPU_FORMAT_B8G8R8X8_UNORM  2       // byte order B,G,R,X  -> u32 0x00RRGGBB

// ---- virgl (3D) resource description fields ----
#define VGPU_TARGET_2D              2       // pipe_texture_target PIPE_TEXTURE_2D
#define VGPU_VIRGL_CAPSET           1
#define VGPU_VIRGL2_CAPSET          2
#define VGPU_BIND_RENDER_TARGET     (1u << 1)
#define VGPU_BIND_SAMPLER_VIEW      (1u << 3)
#define VGPU_BIND_TRANSFER_TO_HOST  (1u << 18)
#define VGPU_BIND_TRANSFER_FROM_HOST (1u << 19)

// ---- control header (all commands begin with this; 24 bytes) ----
typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;         // VIRTIO_GPU_FLAG_FENCE = 1 (unused: poll-only)
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx;
    uint8_t  padding[3];
} vgpu_ctrl_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t x, y, width, height;
} vgpu_rect_t;

typedef struct __attribute__((packed)) {
    uint32_t type, flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx, padding[3];
} vgpu_hdr_wire_t;

typedef struct __attribute__((packed)) {
    vgpu_rect_t r;
    uint32_t enabled;
    uint32_t flags;
} vgpu_display_one_t;

#define VGPU_MAX_SCANOUTS 16

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    vgpu_display_one_t pmodes[VGPU_MAX_SCANOUTS];   // 408 bytes total
} vgpu_resp_display_info_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} vgpu_cmd_resource_create_2d_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;          // guest-physical
    uint32_t length;
    uint32_t padding;
} vgpu_mem_entry_t;         // 16 bytes

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;    // vgpu_mem_entry_t[nr_entries] follow
} vgpu_cmd_attach_backing_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    vgpu_rect_t r;
    uint64_t offset;        // byte offset into the resource's backing
    uint32_t resource_id;
    uint32_t padding;
} vgpu_cmd_transfer_to_host_2d_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    vgpu_rect_t r;
    uint32_t scanout_id;
    uint32_t resource_id;
} vgpu_cmd_set_scanout_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    vgpu_rect_t r;
    uint32_t resource_id;
    uint32_t padding;
} vgpu_cmd_resource_flush_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} vgpu_cmd_resource_unref_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    uint32_t capset_index;
    uint32_t padding;
} vgpu_cmd_get_capset_info_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    uint32_t capset_id;
    uint32_t capset_max_version;
    uint32_t capset_max_size;
    uint32_t padding;
} vgpu_resp_capset_info_t;

// ---- 3D (virgl) commands ----
typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    uint32_t nlen;              // length of debug_name including NUL
    uint32_t context_init;      // capset id for the context (VIRGL/VIRGL2)
    char     debug_name[64];
} vgpu_cmd_ctx_create_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} vgpu_cmd_ctx_resource_t;      // CTX_ATTACH_RESOURCE / CTX_DETACH_RESOURCE

typedef struct __attribute__((packed)) {
    uint32_t x, y, z;
    uint32_t w, h, d;
} vgpu_box_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width, height, depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;
} vgpu_cmd_resource_create_3d_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    vgpu_box_t box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;            // row pitch in bytes
    uint32_t layer_stride;
} vgpu_cmd_transfer_host_3d_t;

// ---- device-specific config space (BAR region from DEVICE_CFG) ----
typedef struct {
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t num_capsets;
    uint32_t blob_alignment;
} vgpu_config_t;

// ---- driver state published for `gpustat` and the test suite ----
typedef struct {
    int      present;           // device found + queue usable
    uint8_t  bus, slot, func;
    uint32_t region_common, region_notify, region_isr, region_devcfg;
    uint32_t notify_mul;
    uint32_t dev_feat_lo, dev_feat_hi;   // what the device offered
    int      virgl, edid;                // features we accepted
    uint16_t num_queues, qsize, notify_off;
    uint32_t num_scanouts, num_capsets;
    uint32_t scanout_w, scanout_h;       // scanout 0 as the device reports it
    int      scanout_enabled;
    int      capset_ok;
    uint32_t capset_id, capset_version, capset_size;
    int      selftest_ok;                // 2D resource round trip + scanout
    uint32_t selftest_ck;                // FNV-1a of the pattern we handed over
    uint32_t selftest_res, selftest_w, selftest_h;
    int      probe3d;                   // 0 = skipped, 1 = ok, 2 = refused, 3 = mismatch
    uint32_t probe3d_err;               // device error code when refused
    uint32_t probe3d_ck;                // FNV-1a after the host round trip
} virtio_gpu_info_t;

void virtio_gpu_init(void);
int virtio_gpu_present(void);
const virtio_gpu_info_t* virtio_gpu_info(void);
// One `[GPU] gpustat:` line on the serial port summarising the same state
// `gpustat` prints on screen, so the headless suite can assert the command.
void virtio_gpu_log_status(void);

// Compile-time check that the wire layout is what the device expects: a
// padding mismatch would show up as ERR_INVALID_PARAMETER on the device side,
// which is a much worse place to discover it than a build error.
typedef char vgpu_hdr_size_check[(sizeof(vgpu_ctrl_hdr_t) == 24) ? 1 : -1];
typedef char vgpu_dispinfo_size_check[(sizeof(vgpu_resp_display_info_t) == 408) ? 1 : -1];
typedef char vgpu_mementry_size_check[(sizeof(vgpu_mem_entry_t) == 16) ? 1 : -1];

#endif // VIRTIO_GPU_H
