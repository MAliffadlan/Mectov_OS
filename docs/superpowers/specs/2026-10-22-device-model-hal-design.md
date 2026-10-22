# Device model & HAL — static registry for block and net devices (2026-10-22)

## Context

The kernel is healthy but structurally rigid in the places that matter for adding
hardware:

- `kernel.c` hard-codes the bring-up order of nine drivers (`ata_dma_init`,
  `ahci_init`, `xhci_init`, `virtio_blk_init`, `virtio_gpu_init`, `sb16_init`,
  `init_rtl8139`, `e1000e_init`) and their IRQ wiring (`kernel.c:525-583`).
- Only the NIC layer has an interface: `net_backend_t` + `net_register_backend()`
  (`src/include/net.h:22`, `src/drivers/net.c:61`). It is the right precedent.
- Block devices have no interface. `ata.c` silently dispatches to AHCI, USB and
  virtio by drive-number range (`src/drivers/ata.c:98-100,152-154,291-293,584-586`),
  and VFS/FS call `ata_*` directly throughout `vfs.c`, `ext2.c` and `fat32.c`.
- There is no device registry, no probe ordering contract, and no way to discover
  what was brought up beyond reading the serial log.

Audit `docs/audit/architecture-review-2026-10-04.md` closed F0–F11; this work is
structural, not a bug fix. The 64-bit line was deliberately dropped
(`docs/architecture/why-32bit.md`), so "modern" here means boundaries and
discovery, not a wider word size.

## Goal

Make block and net devices plug into the kernel through one static device model:
a new driver registers itself and its devices; `kernel.c` no longer names drivers;
consumers talk to a class interface; and adding a driver means adding one file
plus a registration call, not editing the boot sequence.

## Requirements

1. **Generic registry.** `device_register()` records `{class, name, instance,
   present, ops, irq, drive range}`. `device_find()` / `device_count()` expose it.
2. **Initcall ordering.** Drivers declare their init via a linker-section macro;
   `device_init_all()` runs them in a deterministic level order after `pci_scan()`.
3. **Block class.** `block_ops_t` (`read`, `write`, `batch_limit`, `present`) plus
   `block_read()` / `block_write()` / `block_batch_limit()` route by drive range.
   Providers: ATA (0–3), AHCI (4–7), USB (8–11), virtio-blk (12–15).
4. **Net class.** `net_register_backend()` also registers a device; `net_init()` is
   a late initcall. NIC priority (`e1000e` > `rtl8139`) is unchanged.
5. **No driver names in `kernel.c`.** The nine driver init calls and the ATA/net IRQ
   registrations move into the drivers or their initcalls.
6. **No behavior change.** Boot, filesystems, networking and all existing suites
   keep their current semantics and logs (new `[DEV]` lines are additive).
7. **Observability.** Boot logs `[DEV] initcalls=<n>` plus one `[DEV] block ...` /
   `[DEV] net ...` summary line per registered device, so discovery is evidence,
   not inference.

## Design

### Core data model — `src/include/device.h`

```c
typedef enum {
    DEV_CLASS_BLOCK = 0,
    DEV_CLASS_NET,
    DEV_CLASS_COUNT
} device_class_t;

typedef struct device {
    const char*    name;        /* "ata", "ahci", "usb", "virtio-blk", "rtl8139" */
    device_class_t cls;
    int            instance;    /* per-class ordinal */
    int            present;
    int            irq;         /* -1 when poll-only */
    int            drive_base;  /* block only */
    int            drive_count; /* block only */
    const void*    ops;         /* block_ops_t* for DEV_CLASS_BLOCK */
} device_t;

void      device_register(device_t* dev);
device_t* device_find(device_class_t cls, const char* name);
device_t* device_at(device_class_t cls, int index);
int       device_count(device_class_t cls);
```

The registry is a fixed array (`MAX_DEVICES` 16 per class) — consistent with the
kernel's static-capacity style (`MAX_TASKS`, `MAX_NODES`) and enough for the
current hardware matrix. Registration happens during single-threaded bring-up.

### Initcalls — `src/include/initcall.h` + `linker.ld`

```c
typedef void (*initcall_fn)(void);
#define INITCALL(fn, level) \
    static initcall_fn __attribute__((used, section(".initcall." #level))) \
        _initcall_##fn = fn;
```

`linker.ld` gains a `KEEP(*(.initcall.*))` block in `.data` plus
`__initcall_start` / `__initcall_end`. `device_init_all()` walks the section and
calls each entry in address order. Levels are numeric so the current order is
preserved exactly:

| level | init | reason |
|---|---|---|
| 10 | `ata_dma_init` | BMIDE detect; registers ATA block provider + INT 46/47 |
| 11 | `ahci_init` | needs `pci_scan`, before USB drive numbering |
| 12 | `xhci_init` | needs `pci_scan`, after AHCI (documented dependency) |
| 13 | `virtio_blk_init` | needs `pci_scan` |
| 14 | `virtio_gpu_init` | optional display device |
| 15 | `sb16_init` | audio device |
| 16 | `init_rtl8139` | first NIC, lowest priority |
| 17 | `e1000e_init` | second NIC, higher priority |
| 20 | `net_init` | protocol layer + INT 43 wiring, after all NICs |

`kernel.c` keeps core init (serial, GDT, paging, ACPI/APIC, SMP, syscalls, timer,
keyboard, `pci_scan`) and replaces the driver block with one
`device_init_all();` call at the same point. IRQ registration moves with the
owner: `register_interrupt_handler(46/47, ...)` into `ata_dma_init()`; `(43,
net_irq_handler)` into `net_init()`.

### Block class — `src/include/block.h` + `src/sys/block.c`

```c
#define BLOCK_BATCH_MAX 128

typedef struct block_ops {
    int (*read)(int drive, uint32_t lba, int count, uint8_t* buf);
    int (*write)(int drive, uint32_t lba, int count, const uint8_t* buf);
} block_ops_t;

int block_read(int drive, uint32_t lba, int count, uint8_t* buf);
int block_write(int drive, uint32_t lba, int count, const uint8_t* buf);
int block_present(int drive);
static inline int block_batch_limit(uint32_t lba, int count) {
    if (count > BLOCK_BATCH_MAX) count = BLOCK_BATCH_MAX;
    int boundary = BLOCK_BATCH_MAX - (int)(lba & (BLOCK_BATCH_MAX - 1));
    if (count > boundary) count = boundary;
    return (count < 1) ? 1 : count;
}
```

The batch math lives in `block.h`; `ata.h` includes `block.h`, defines
`ATA_BATCH_MAX` as `BLOCK_BATCH_MAX`, and keeps `ata_batch_limit()` as an alias
that calls `block_batch_limit()` so driver-internal call sites do not change.
`vfs.c`'s `ATA_BATCH_MAX` allocation becomes `BLOCK_BATCH_MAX`.

`block.c` looks up the provider whose `[drive_base, drive_base+drive_count)` range
contains `drive` and dispatches. It returns `-1` for an unregistered range.
Providers register unconditionally during their init; the `present` flag reports
whether hardware was found, so the registry shape is identical on every boot:

| provider | range | ops |
|---|---|---|
| ATA (IDE PIO/DMA) | 0–3 | `ata_read_sectors_drive` / `ata_write_sectors_drive` |
| AHCI | 4–7 | `ahci_read_sectors` / `ahci_write_sectors` |
| USB (xHCI BOT) | 8–11 | `usb_read_sectors` / `usb_write_sectors` |
| virtio-blk | 12–15 | `virtio_blk_read_sectors` / `virtio_blk_write_sectors` |

The `drive >= X_DRIVE_BASE` branches in `ata.c` are deleted: routing is now the
block core's job, and `ata_*` becomes ATA-only. `vfs.c`, `ext2.c` and `fat32.c`
switch their direct `ata_read_*` / `ata_write_*` calls to `block_read` /
`block_write`, and their `ata_batch_limit` calls to `block_batch_limit`.

### Net class

`net_register_backend()` gains one `device_register()` call (class NET, name from
the backend, IRQ -1 until the driver wires one). No protocol-layer changes:
`net_backend` remains the dispatch pointer. `net_init()` keeps its current body;
it is only reached through level 20 now.

### Files touched

| file | change |
|---|---|
| `src/include/device.h`, `src/sys/device.c` | new registry + initcall walker |
| `src/include/initcall.h` | new macro/typedef |
| `src/include/block.h`, `src/sys/block.c` | new block class |
| `linker.ld` | `.initcall.*` section + bounds |
| `kernel.c` | driver block replaced by `device_init_all()` |
| `src/drivers/ata.c` | registration + IRQ wiring; delete cross-driver dispatch |
| `src/drivers/ahci.c`, `src/drivers/xhci.c`, `src/drivers/virtio_blk.c` | register providers, initcall macro |
| `src/drivers/rtl8139.c`, `src/drivers/e1000e.c` | initcall macro; register backend device |
| `src/drivers/net.c` | register NET device; initcall level 20; INT 43 wiring |
| `src/drivers/sb16.c`, `src/drivers/virtio_gpu.c` | initcall macro |
| `src/sys/vfs.c`, `src/sys/ext2.c`, `src/sys/fat32.c` | use `block_*` |
| `Makefile` | no edit expected — `SRCS` wildcards pick up `src/sys/*.c`; the build verifies it |
| `docs/architecture/overview.md` | document the device model |

## Migration order

1. **Core + net** (low risk): device registry, initcalls, linker section, kernel.c
   switch, net registration/init move. Build + boot + `icmp` suite.
2. **Block providers** (medium risk): block class, provider registration, delete
   `ata.c` dispatch branches. Boot + `fputest` + `iocache`.
3. **FS call sites** (medium risk): migrate `vfs.c`/`ext2.c`/`fat32.c`, then remove
   the `ata.c` dispatch branches and add the ATA-only drive guard. Boot + `fputest`
   + `fat32` + full `check-quick`.
4. **Docs** update, final grep gate (no `ata_` in FS files).

## Non-goals

- Loadable `.ko` modules or runtime driver loading.
- PCI ID matching / bus model / resource manager (approach 2 later; the registry
  fields are shaped to accept it).
- Input, display and audio classes (phase 2).
- Hotplug, multi-instance NICs, ABI/syscall changes.
- Rewriting `vfs.c`/`task.c` decomposition.

## Verification

1. `make clean && make -j8` → exit 0, 0 C warnings.
2. Boot to login; serial shows `[DEV] initcalls=9` and one line per provider,
   e.g. `[DEV] block ata drives=0-3 present=1`, plus the active
   `[DEV] net ...` line.
3. `make check CHECK_ARGS="--only=boot"`, `--only=fputest`, `--only=iocache`,
   `--only=fat32` → PASS.
4. `make check-quick` → 16/16 PASS.
5. `grep -n "ata_" src/sys/vfs.c src/sys/ext2.c src/sys/fat32.c` returns only
   comments (no calls).
6. `git diff --stat` shows no changes outside the files table above.

## Risks

| risk | mitigation |
|---|---|
| Init order regression (IRQ/timer before driver) | numeric levels preserve the current order; boot + suites per step |
| Block routing bug (wrong provider for drive) | range table is explicit and tested by AHCI/USB/virtio mounts only when present; default path is ATA |
| `.initcall` section stripped or mislinked | linker keeps `KEEP(*(.initcall.*))`; boot log proves each init ran |
| Net backend priority change | priority function untouched; `net_init` still runs after both NICs |
