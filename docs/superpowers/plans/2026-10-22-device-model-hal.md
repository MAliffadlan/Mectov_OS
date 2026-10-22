# Device Model & HAL Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a static device registry and initcall ordering so block and net drivers register themselves, `kernel.c` stops naming drivers, and VFS/FS talk to a block class instead of `ata_*`.

**Architecture:** A small `device_t` registry (fixed arrays, single-threaded bring-up) plus linker-section initcalls at numeric levels preserve the current boot order. Block drivers register `{drive range, ops}`; `block_read/write` route by drive. NICs keep the existing `net_backend` priority model and additionally register a NET device.

**Tech Stack:** i386 freestanding C99 (`gcc -m32`), NASM/ld, GNU make, QEMU TCG, serial-log assertions.

**Spec:** `docs/superpowers/specs/2026-10-22-device-model-hal-design.md`

## Global Constraints

- Freestanding i386, no libc/POSIX; compile flags come from the Makefile (`-m32 -std=gnu99 -ffreestanding -O2 -march=i686 -fno-pie -fno-pic`).
- Static registry only: no loadable modules, no PCI ID matching, no hotplug.
- Preserve boot order and behavior; new serial lines are additive.
- **Do not commit.** The user has not authorized commits. Each task ends with a `git status`/`git diff` checkpoint instead of `git commit`.
- Evidence before claims: every task is verified with a build and a boot/suite run.

## File Map

| File | Responsibility |
|---|---|
| `src/include/device.h` | `device_class_t`, `device_t`, registry API |
| `src/include/initcall.h` | `initcall_fn`, `INITCALL()` macro, `device_init_all()` |
| `src/sys/device.c` | registry storage, initcall walker, `[DEV]` summary |
| `src/include/block.h` | `block_ops_t`, `BLOCK_BATCH_MAX`, `block_*` API |
| `src/sys/block.c` | provider table + drive-range routing |
| `linker.ld` | `.initcall.*` sections + `__initcall_start/end` |
| `kernel.c` | one `device_init_all()` call replaces driver calls |
| `src/drivers/ata.c` | ATA-only PIO/DMA, provider registration, IRQ 46/47 |
| `src/drivers/ahci.c`, `xhci.c`, `virtio_blk.c` | register block providers |
| `src/drivers/rtl8139.c`, `e1000e.c`, `net.c` | initcalls + NET device registration |
| `src/drivers/virtio_gpu.c`, `sb16.c` | initcalls |
| `src/sys/vfs.c`, `ext2.c`, `fat32.c` | use `block_*` |
| `docs/architecture/overview.md` | document the model |

---

## Task 1: Device registry + initcall infrastructure

**Files:**
- Create: `src/include/device.h`, `src/include/initcall.h`, `src/sys/device.c`
- Modify: `linker.ld`, `kernel.c`

**Interfaces:**
- Produces: `device_register(device_t*)`, `device_find(cls, name)`, `device_at(cls, i)`, `device_count(cls)`, `device_init_all()`, `INITCALL(fn, level)`.
- `device_t` fields: `name`, `cls`, `instance`, `present`, `irq`, `drive_base`, `drive_count`, `ops`.

- [ ] **Step 1: Write the failing test (boot marker gate)**

Run:
```bash
make -j8 iso && python3 scripts/boot_test.py
grep -c "\[DEV\]" /tmp/mectov_ci_serial.log
```
Expected: boot PASS, grep prints `0` (no device model yet). This is RED.

- [ ] **Step 2: Create `src/include/device.h`**

```c
#ifndef DEVICE_H
#define DEVICE_H

#include "types.h"

#define MAX_DEVICES_PER_CLASS 16

typedef enum {
    DEV_CLASS_BLOCK = 0,
    DEV_CLASS_NET,
    DEV_CLASS_COUNT
} device_class_t;

typedef struct device {
    const char*    name;
    device_class_t cls;
    int            instance;
    int            present;
    int            irq;         /* -1 when poll-only */
    int            drive_base;  /* block only */
    int            drive_count; /* block only */
    const void*    ops;         /* class-specific ops */
} device_t;

void      device_register(device_t* dev);
device_t* device_find(device_class_t cls, const char* name);
device_t* device_at(device_class_t cls, int index);
int       device_count(device_class_t cls);
void      device_log_summary(void);

#endif
```

- [ ] **Step 3: Create `src/include/initcall.h`**

```c
#ifndef INITCALL_H
#define INITCALL_H

typedef void (*initcall_fn)(void);

#define INITCALL(fn, level) \
    static initcall_fn __attribute__((used, section(".initcall." #level))) \
        _initcall_##fn = fn

void device_init_all(void);

#endif
```

- [ ] **Step 4: Create `src/sys/device.c`**

```c
#include "../include/device.h"
#include "../include/initcall.h"
#include "../include/serial.h"

extern initcall_fn __initcall_start[];
extern initcall_fn __initcall_end[];

static device_t* devices[DEV_CLASS_COUNT][MAX_DEVICES_PER_CLASS];
static int device_counts[DEV_CLASS_COUNT];

static void dev_write_uint(uint32_t v) {
    char buf[10];
    int n = 0;
    if (v == 0) { write_serial('0'); return; }
    while (v > 0) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) write_serial(buf[--n]);
}

static int dev_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void device_register(device_t* dev) {
    if (dev == NULL || dev->cls >= DEV_CLASS_COUNT) return;
    int n = device_counts[dev->cls];
    if (n >= MAX_DEVICES_PER_CLASS) {
        write_serial_string("[DEV] registry full\n");
        return;
    }
    dev->instance = n;
    devices[dev->cls][n] = dev;
    device_counts[dev->cls] = n + 1;
}

device_t* device_find(device_class_t cls, const char* name) {
    if (cls >= DEV_CLASS_COUNT) return NULL;
    for (int i = 0; i < device_counts[cls]; i++)
        if (dev_streq(devices[cls][i]->name, name)) return devices[cls][i];
    return NULL;
}

device_t* device_at(device_class_t cls, int index) {
    if (cls >= DEV_CLASS_COUNT || index < 0 || index >= device_counts[cls])
        return NULL;
    return devices[cls][index];
}

int device_count(device_class_t cls) {
    return (cls < DEV_CLASS_COUNT) ? device_counts[cls] : 0;
}

void device_log_summary(void) {
    for (int c = 0; c < DEV_CLASS_COUNT; c++) {
        for (int i = 0; i < device_counts[c]; i++) {
            device_t* d = devices[c][i];
            write_serial_string("[DEV] ");
            write_serial_string(c == DEV_CLASS_BLOCK ? "block " : "net ");
            write_serial_string(d->name);
            if (c == DEV_CLASS_BLOCK) {
                write_serial_string(" drives=");
                dev_write_uint((uint32_t)d->drive_base);
                write_serial('-');
                dev_write_uint((uint32_t)(d->drive_base + d->drive_count - 1));
            }
            write_serial_string(" present=");
            write_serial('0' + (d->present ? 1 : 0));
            write_serial_string("\n");
        }
    }
}

void device_init_all(void) {
    int count = 0;
    for (initcall_fn* it = __initcall_start; it < __initcall_end; it++) {
        (*it)();
        count++;
    }
    write_serial_string("[DEV] initcalls=");
    dev_write_uint((uint32_t)count);
    write_serial_string("\n");
    device_log_summary();
}
```

- [ ] **Step 5: Add the initcall section to `linker.ld`**

Replace the `.data` block with:
```ld
    .data BLOCK(4K) : ALIGN(4K) {
        *(.data)
        . = ALIGN(4);
        __initcall_start = .;
        KEEP(*(.initcall.10))
        KEEP(*(.initcall.11))
        KEEP(*(.initcall.12))
        KEEP(*(.initcall.13))
        KEEP(*(.initcall.14))
        KEEP(*(.initcall.15))
        KEEP(*(.initcall.16))
        KEEP(*(.initcall.17))
        KEEP(*(.initcall.20))
        KEEP(*(.initcall.*))
        __initcall_end = .;
    }
```

- [ ] **Step 6: Call the walker from `kernel.c` (keep existing driver calls for now)**

Immediately after the net IRQ registration (`register_interrupt_handler(43, net_irq_handler);`), insert:
```c
    write_serial_string("[K] devices\n");
    extern void device_init_all(void);
    device_init_all();
```

- [ ] **Step 7: Build and run the green test**

Run:
```bash
make -j8 iso && python3 scripts/boot_test.py
grep "\[DEV\]" /tmp/mectov_ci_serial.log
```
Expected:
```
[DEV] initcalls=0
```
and boot PASS. No devices yet; this proves the linker section and walker.

- [ ] **Step 8: Checkpoint**

Run `git status --short && git diff --stat`. Do not commit.

---

## Task 2: Migrate NICs and `net_init` to initcalls

**Files:**
- Modify: `src/drivers/rtl8139.c`, `src/drivers/e1000e.c`, `src/drivers/net.c`, `kernel.c`
- Test: boot marker + `icmp` suite

**Interfaces:**
- Consumes: `INITCALL`, `device_register`, `device_t`.
- Produces: NET devices named `rtl8139` / `e1000e`; `net_init` at level 20; IRQ 43 owned by `net_init`.

- [ ] **Step 1: Add the initcall to `rtl8139.c`**

Add `#include "../include/initcall.h"` to the includes, then at file scope after `init_rtl8139()`:
```c
INITCALL(init_rtl8139, 16);
```

- [ ] **Step 2: Add the initcall to `e1000e.c`**

Add `#include "../include/initcall.h"`, then after `e1000e_init()`:
```c
INITCALL(e1000e_init, 17);
```

- [ ] **Step 3: Register a NET device in `net_register_backend()`**

Add includes to `net.c`:
```c
#include "../include/device.h"
#include "../include/initcall.h"
```
At the top of `net_register_backend()`, before the priority logic:
```c
    static device_t net_devices[4];
    static int net_device_count;
    if (be->present && net_device_count < 4) {
        device_t* d = &net_devices[net_device_count++];
        d->name = be->name;
        d->cls = DEV_CLASS_NET;
        d->instance = 0;
        d->present = be->present;
        d->irq = -1;
        d->drive_base = 0;
        d->drive_count = 0;
        d->ops = be;
        device_register(d);
    }
```

- [ ] **Step 4: Move IRQ 43 into `net_init()` and add its initcall**

At the top of `net_init()`:
```c
    register_interrupt_handler(43, net_irq_handler);
```
At file scope after `net_init()`:
```c
INITCALL(net_init, 20);
```

- [ ] **Step 5: Remove the NIC/net calls from `kernel.c`**

Delete these blocks (keep `device_init_all()`):
```c
    write_serial_string("[K] rtl\n");
    extern void init_rtl8139();
    init_rtl8139();
    ...
    write_serial_string("[K] e1000e\n");
    extern void e1000e_init(void);
    e1000e_init();
    write_serial_string("[K] net\n");
    extern void net_init();
    net_init();
    ...
    extern void register_interrupt_handler(uint8_t n, isr_t handler);
    extern void net_irq_handler(registers_t*);
    register_interrupt_handler(43, net_irq_handler);
```

- [ ] **Step 6: Run the green test**

```bash
make -j8 iso && python3 scripts/boot_test.py
grep "\[DEV\]" /tmp/mectov_ci_serial.log
make check CHECK_ARGS="--only=icmp"
```
Expected: boot PASS; `[DEV] net rtl8139 present=1` (and `e1000e present=1` if QEMU exposes it); `icmp` PASS.

- [ ] **Step 7: Checkpoint**

`git status --short && git diff --stat`. Do not commit.

---

## Task 3: Block class + ATA/AHCI/USB/virtio providers

**Files:**
- Create: `src/include/block.h`, `src/sys/block.c`
- Modify: `src/drivers/ata.c`, `src/drivers/ahci.c`, `src/drivers/xhci.c`,
  `src/drivers/virtio_blk.c`, `src/include/ata.h`, `kernel.c`

**Interfaces:**
- Consumes: `device_register`, `device_t`, `INITCALL`.
- Produces: `block_read(drive,lba,count,buf)`, `block_write(...)`,
  `block_present(drive)`, `block_batch_limit(lba,count)`, `BLOCK_BATCH_MAX`,
  and four registered block devices.

- [ ] **Step 1: Write the failing test**

```bash
make -j8 iso && python3 scripts/boot_test.py
grep "\[DEV\] block" /tmp/mectov_ci_serial.log
```
Expected: no `[DEV] block` lines (RED).

- [ ] **Step 2: Create `src/include/block.h`**

```c
#ifndef BLOCK_H
#define BLOCK_H

#include "types.h"
#include "device.h"

#define BLOCK_BATCH_MAX 128

typedef struct block_ops {
    int (*read)(int drive, uint32_t lba, int count, uint8_t* buf);
    int (*write)(int drive, uint32_t lba, int count, const uint8_t* buf);
} block_ops_t;

void block_register(device_t* dev);
int  block_read(int drive, uint32_t lba, int count, uint8_t* buf);
int  block_write(int drive, uint32_t lba, int count, const uint8_t* buf);
int  block_present(int drive);

static inline int block_batch_limit(uint32_t lba, int count) {
    if (count > BLOCK_BATCH_MAX) count = BLOCK_BATCH_MAX;
    int boundary = BLOCK_BATCH_MAX - (int)(lba & (BLOCK_BATCH_MAX - 1));
    if (count > boundary) count = boundary;
    return (count < 1) ? 1 : count;
}

#endif
```

- [ ] **Step 3: Create `src/sys/block.c`**

```c
#include "../include/block.h"
#include "../include/device.h"

#define BLOCK_MAX_PROVIDERS 8
static device_t* providers[BLOCK_MAX_PROVIDERS];
static int provider_count;

void block_register(device_t* dev) {
    if (dev == NULL || dev->cls != DEV_CLASS_BLOCK || dev->ops == NULL) return;
    if (provider_count >= BLOCK_MAX_PROVIDERS) return;
    providers[provider_count++] = dev;
    device_register(dev);
}

static device_t* block_lookup(int drive) {
    for (int i = 0; i < provider_count; i++) {
        device_t* d = providers[i];
        if (drive >= d->drive_base && drive < d->drive_base + d->drive_count)
            return d;
    }
    return NULL;
}

int block_read(int drive, uint32_t lba, int count, uint8_t* buf) {
    device_t* d = block_lookup(drive);
    if (d == NULL || d->ops == NULL) return -1;
    const block_ops_t* ops = (const block_ops_t*)d->ops;
    return ops->read ? ops->read(drive, lba, count, buf) : -1;
}

int block_write(int drive, uint32_t lba, int count, const uint8_t* buf) {
    device_t* d = block_lookup(drive);
    if (d == NULL || d->ops == NULL) return -1;
    const block_ops_t* ops = (const block_ops_t*)d->ops;
    return ops->write ? ops->write(drive, lba, count, buf) : -1;
}

int block_present(int drive) {
    device_t* d = block_lookup(drive);
    return d ? d->present : 0;
}
```

- [ ] **Step 4: Make `ata.h` delegate the batch math to `block.h`**

Add `#include "block.h"` to `ata.h`, add `#define ATA_DRIVE_COUNT 4`, then
replace the body of `ata_batch_limit`:
```c
#define ATA_BATCH_MAX BLOCK_BATCH_MAX
static inline int ata_batch_limit(unsigned int lba, int count) {
    return block_batch_limit(lba, count);
}
```

- [ ] **Step 5: Keep the existing `ata.c` dispatch for now**

Do **not** delete the `VIRTIO_BLK_BASE` / `USB_DRIVE_BASE` / `AHCI_DRIVE_BASE`
dispatch lines yet. Until Task 5 moves every FS caller to `block_*`, the
remaining `ata_*` callers still depend on that dispatch. Task 5 removes it
after the migration.

- [ ] **Step 6: Register the ATA provider and own IRQ 46/47**

At the end of `ata.c`:
```c
static const block_ops_t ata_block_ops = {
    .read = ata_read_sectors_drive,
    .write = ata_write_sectors_drive,
};

static device_t ata_block_device = {
    .name = "ata",
    .cls = DEV_CLASS_BLOCK,
    .present = 1,
    .irq = -1,
    .drive_base = 0,
    .drive_count = ATA_DRIVE_COUNT,
    .ops = &ata_block_ops,
};

static void ata_boot_init(void) {
    ata_dma_init();
    register_interrupt_handler(46, ata_dma_irq_primary);
    register_interrupt_handler(47, ata_dma_irq_secondary);
    block_register(&ata_block_device);
}
INITCALL(ata_boot_init, 10);
```

- [ ] **Step 7: Register AHCI (level 11), USB (12), virtio-blk (13)**

In `ahci.c`:
```c
static const block_ops_t ahci_block_ops = {
    .read = ahci_read_sectors,
    .write = ahci_write_sectors,
};
static device_t ahci_block_device = {
    .name = "ahci", .cls = DEV_CLASS_BLOCK, .present = 0, .irq = -1,
    .drive_base = AHCI_DRIVE_BASE, .drive_count = AHCI_MAX_PORTS,
    .ops = &ahci_block_ops,
};
static void ahci_boot_init(void) {
    ahci_init();
    ahci_block_device.present = ahci_present();
    block_register(&ahci_block_device);
}
INITCALL(ahci_boot_init, 11);
```

In `xhci.c`:
```c
static const block_ops_t usb_block_ops = {
    .read = usb_read_sectors,
    .write = usb_write_sectors,
};
static device_t usb_block_device = {
    .name = "usb", .cls = DEV_CLASS_BLOCK, .present = 0, .irq = -1,
    .drive_base = USB_DRIVE_BASE, .drive_count = 4,
    .ops = &usb_block_ops,
};
static void xhci_boot_init(void) {
    xhci_init();
    usb_block_device.present = usb_present();
    block_register(&usb_block_device);
}
INITCALL(xhci_boot_init, 12);
```

In `virtio_blk.c`:
```c
static const block_ops_t virtio_block_ops = {
    .read = virtio_blk_read_sectors,
    .write = virtio_blk_write_sectors,
};
static device_t virtio_block_device = {
    .name = "virtio-blk", .cls = DEV_CLASS_BLOCK, .present = 0, .irq = -1,
    .drive_base = VIRTIO_BLK_BASE, .drive_count = VIRTIO_BLK_MAX,
    .ops = &virtio_block_ops,
};
static void virtio_blk_boot_init(void) {
    virtio_blk_init();
    virtio_block_device.present = virtio_blk_present();
    block_register(&virtio_block_device);
}
INITCALL(virtio_blk_boot_init, 13);
```

Each of these files needs `#include "../include/block.h"`,
`#include "../include/device.h"`, `#include "../include/initcall.h"`.

- [ ] **Step 8: Remove the four block driver calls from `kernel.c`**

Delete the `ata_dma_init`, `ahci_init`, `xhci_init` and `virtio_blk_init`
blocks (including IRQ 46/47 registration). Keep `device_init_all()`. Remove
the now-unused `extern void register_interrupt_handler(...)` declaration if
no other call remains in `kernel.c`.

- [ ] **Step 9: Run the green test**

```bash
make -j8 iso && python3 scripts/boot_test.py
grep "\[DEV\]" /tmp/mectov_ci_serial.log
make check CHECK_ARGS="--only=fputest"
```
Expected:
```
[DEV] initcalls=7
[DEV] block ata drives=0-3 present=1
[DEV] block ahci drives=4-7 present=0
[DEV] block usb drives=8-11 present=0
[DEV] block virtio-blk drives=12-15 present=0
[DEV] net rtl8139 present=1
```
(`present` for ahci/usb/virtio depends on the QEMU devices attached.)
`fputest` PASS.

- [ ] **Step 10: Checkpoint**

`git status --short && git diff --stat`. Do not commit.

---

## Task 4: Migrate virtio-gpu and SB16 inits

**Files:** `src/drivers/virtio_gpu.c`, `src/drivers/sb16.c`, `kernel.c`

- [ ] **Step 1: Add initcalls**

`virtio_gpu.c` (needs `#include "../include/initcall.h"`):
```c
INITCALL(virtio_gpu_init, 14);
```

`sb16.c` (needs `#include "../include/initcall.h"`):
```c
static void sb16_boot_init(void) {
    sb16_init();
    sb16_selftest();
}
INITCALL(sb16_boot_init, 15);
```

- [ ] **Step 2: Remove the calls from `kernel.c`**

Delete the `virtio_gpu_init()` and `sb16_init()`/`sb16_selftest()` blocks.
`device_init_all()` is now the only driver bring-up call.

- [ ] **Step 3: Run the green test**

```bash
make -j8 iso && python3 scripts/boot_test.py
grep "\[DEV\] initcalls" /tmp/mectov_ci_serial.log
grep "\[SB16\]" /tmp/mectov_ci_serial.log | tail -3
```
Expected: `[DEV] initcalls=9`; SB16 selftest lines present (or the documented
"no SB16" line when the device is absent).

- [ ] **Step 4: Checkpoint**

`git status --short && git diff --stat`. Do not commit.

---

## Task 5: Migrate VFS/FS call sites to the block API

**Files:** `src/sys/vfs.c`, `src/sys/ext2.c`, `src/sys/fat32.c`

**Interfaces:**
- Consumes: `block_read`, `block_write`, `block_batch_limit`,
  `BLOCK_BATCH_MAX`.
- Produces: no direct `ata_*` I/O calls in the three FS files.

- [ ] **Step 1: Write the failing test (grep gate)**

```bash
grep -nE 'ata_(read|write|batch)|ATA_BATCH_MAX' \
  src/sys/vfs.c src/sys/ext2.c src/sys/fat32.c \
  | grep -vE ':[0-9]+:[[:space:]]*//'
```
Expected: many matches (RED).

- [ ] **Step 2: Switch includes**

In each of the three files, replace `#include "../include/ata.h"` with
`#include "../include/block.h"`.

- [ ] **Step 3: Apply the mechanical replacements**

| before | after |
|---|---|
| `ata_read_sector_drive(d, lba, buf)` | `block_read(d, lba, 1, buf)` |
| `ata_write_sector_drive(d, lba, buf)` | `block_write(d, lba, 1, buf)` |
| `ata_read_sectors_drive(d, lba, n, buf)` | `block_read(d, lba, n, buf)` |
| `ata_write_sectors_drive(d, lba, n, buf)` | `block_write(d, lba, n, buf)` |
| `ata_read_sector(lba, buf)` | `block_read(0, lba, 1, buf)` |
| `ata_write_sector(lba, buf)` | `block_write(0, lba, 1, buf)` |
| `ata_batch_limit(` | `block_batch_limit(` |
| `ATA_BATCH_MAX` | `BLOCK_BATCH_MAX` |

The multi-sector names are safe to rename with:
```bash
perl -pi -e 's/\bata_read_sectors_drive\(/block_read(/g;
             s/\bata_write_sectors_drive\(/block_write(/g;
             s/\bata_batch_limit\(/block_batch_limit(/g;
             s/\bATA_BATCH_MAX\b/BLOCK_BATCH_MAX/g' \
  src/sys/vfs.c src/sys/ext2.c src/sys/fat32.c
```
Then edit the remaining single-sector call sites by hand using the table above
(`ata_read_sector_drive`, `ata_write_sector_drive`, `ata_read_sector`,
`ata_write_sector`). Verify with the grep gate.

- [ ] **Step 4: Run the green test (grep gate + build)**

```bash
if grep -nE 'ata_(read|write|batch)|ATA_BATCH_MAX' \
     src/sys/vfs.c src/sys/ext2.c src/sys/fat32.c \
     | grep -vE ':[0-9]+:[[:space:]]*//' | grep -q .; then
  echo "FAIL: direct ATA I/O remains"; else echo "PASS"; fi
make -j8
```
Expected: `PASS`, build exit 0.

- [ ] **Step 5: Run the FS suites**

```bash
make check CHECK_ARGS="--only=boot"
make check CHECK_ARGS="--only=fputest"
make check CHECK_ARGS="--only=fat32"
make check CHECK_ARGS="--only=iocache"
```
Expected: all PASS.

- [ ] **Step 5: Remove the ATA dispatch and guard ATA-only drives**

Only now that every FS caller uses `block_*`, delete the
`VIRTIO_BLK_BASE` / `USB_DRIVE_BASE` / `AHCI_DRIVE_BASE` dispatch lines from
`ata_read_sector_drive_io`, `ata_read_sectors_drive_io`,
`ata_write_sector_drive_io`, `ata_write_sectors_drive_io`. At the top of the
four public functions add:
```c
    if (drive < 0 || drive >= ATA_DRIVE_COUNT) return -1;
```
Then remove the `ahci.h`, `xhci.h` and `virtio_blk.h` includes from `ata.c`
and add:
```c
#include "../include/block.h"
#include "../include/device.h"
#include "../include/initcall.h"
```

- [ ] **Step 6: Run the FS and multi-backend suites**

```bash
make -j8
make check CHECK_ARGS="--only=fputest"
make check CHECK_ARGS="--only=fat32"
make check CHECK_ARGS="--only=ahci"
make check CHECK_ARGS="--only=virtio"
```
Expected: `fputest` and `fat32` PASS. `ahci`/`virtio` PASS when the suite's
QEMU device is available; if the suite is opt-in/skipped, record the skip.

- [ ] **Step 7: Checkpoint**

`git status --short && git diff --stat`. Do not commit.

---

## Task 6: Docs + full verification

**Files:** `docs/architecture/overview.md`

- [ ] **Step 1: Document the model**

Add a section after “Bootstrapping Sequence”:

```markdown
## Device model

Drivers register themselves through `device_register()` from a linker-section
initcall (`INITCALL(fn, level)`), so `kernel.c` contains one
`device_init_all()` call instead of a driver list. Levels preserve the old
bring-up order (10 ATA DMA, 11 AHCI, 12 xHCI, 13 virtio-blk, 14 virtio-gpu,
15 SB16, 16 rtl8139, 17 e1000e, 20 net).

Block devices register a drive range and `block_ops_t`; VFS/ext2/FAT32 call
`block_read()` / `block_write()` and the core routes by drive:
0–3 ATA, 4–7 AHCI, 8–11 USB, 12–15 virtio-blk. NICs keep the existing
priority-based `net_backend` and additionally appear as NET devices.
```

- [ ] **Step 2: Run the full quick suite**

```bash
make check-quick
```
Expected: 16/16 PASS (the known `iocache` TCG flake now retries once and keeps
the 2.5x threshold).

- [ ] **Step 3: Final gates**

```bash
git status --short
git diff --stat
grep -rn "ata_read\|ata_write\|ata_batch" src/sys/vfs.c src/sys/ext2.c src/sys/fat32.c
```
Expected: only comment lines mention `ata_*`; diff is limited to the file map.

- [ ] **Step 4: Checkpoint**

Report the exact build/suite output. Do not commit.

---

## Self-Review Checklist

- [ ] Spec requirement 1 (registry) → Task 1.
- [ ] Spec requirement 2 (initcalls) → Task 1.
- [ ] Spec requirement 3 (block class + providers) → Task 3.
- [ ] Spec requirement 4 (net class) → Task 2.
- [ ] Spec requirement 5 (no driver names in kernel.c) → Tasks 2–4.
- [ ] Spec requirement 6 (no behavior change) → boot + suites in every task.
- [ ] Spec requirement 7 (observability) → `[DEV] initcalls` and summary.
- [ ] No placeholders, no “similar to Task N”.
- [ ] Type consistency: `block_ops_t`, `block_register`, `device_t`, `INITCALL`
  are used with the same signatures everywhere.
