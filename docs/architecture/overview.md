# Mectov OS — Architecture Overview

Mectov OS is an x86 (i386) 32-bit monolithic operating system kernel written from scratch in C and x86 Assembly. It operates without external runtime dependencies (no libc, no POSIX) directly on bare-metal or virtualized hardware.

---

## 🏛️ System Architecture Diagram

```
+-----------------------------------------------------------------------------+
|                       GRUB Multiboot v1 (VBE Mode)                          |
+-----------------------------------------------------------------------------+
|  boot.asm  -->  kernel.c (kernel_main)                                      |
+-----------------------------------------------------------------------------+
|  GDT (Ring 0/3)   |  IDT & IRQ Stub   |  TSS per core  |  Syscall (int 0x80) |
+-----------------------------------------------------------------------------+
|  ACPI MADT        |  Local APIC       |  I/O APIC      |  SMP Trampoline    |
+-----------------------------------------------------------------------------+
|  Physical Mem (PMM)|  Virtual Mem (VMM)|  Heap Sandbox |  Ext2 Filesystem   |
+-----------------------------------------------------------------------------+
|  AHCI SATA (4+)   |  xHCI USB 3.0 (8+)|  FAT32 / pcache|  VFS Mount Table   |
+-----------------------------------------------------------------------------+
|  VBE Framebuffer  |  Double Buffer    |  Window Mgr    |  Desktop & Taskbar |
+-----------------------------------------------------------------------------+
|  NIC backends     |  Ethernet/IP/UDP  |  DNS/TCP/Proxy |  Ring 3 MCT Apps   |
+-----------------------------------------------------------------------------+
|  PAGE_DEV Scanout |  FB_MAP/RELEASE   |  PAGE_NX W^X   |  PAE 3-level Paging|
```

---

## 🚀 Bootstrapping Sequence

1. **GRUB Boot Protocol (`boot.asm`)**:
   - Loaded by GRUB Multiboot compliant bootloader.
   - Configures Multiboot header requesting VBE graphics (1024x768x32).
   - Sets up initial kernel stack and calls `kernel_main(multiboot_info_t *mbi)`.

2. **Core Hardware Initialization (`kernel.c`)**:
   - **Serial Log**: Initializes UART 16550 COM1 serial port for immediate debug logs.
   - **GDT Initialization**: Constructs global descriptor table with Ring 0 code/data, Ring 3 code/data, and TSS descriptors.
   - **Memory & Paging**: Parses BIOS memory map, initializes physical memory manager (PMM), page tables (VMM), identity-maps kernel and VBE framebuffer.
   - **ACPI & APIC**: Locates RSDP/RSDT/MADT tables, maps Local APIC and I/O APIC regions, disables legacy 8259 PIC, and routes IRQs to APIC interrupts.
   - **SMP Initialization**: Executes INIT-SIPI-SIPI sequence to wake Application Processors (APs), loading per-core GDT/IDT/TSS.
   - **Interrupts & Timers**: Configures IDT gates, initializes PIT hardware timer (1000 Hz) and PS/2 Keyboard/Mouse drivers.
   - **Networking & VFS**: Initializes ATA PIO driver, mounts Ext2/VFS virtual file system, probes the PCI NIC backends (rtl8139, e1000e, virtio-net — see [network.md](../drivers/network.md)), and sends ARP query to Gateway.

3. **GUI Shell & Userland Startup**:
   - Enables interrupts (`sti`).
   - Allocates VBE double-buffer and renders desktop wallpaper and squircle icons.
   - Renders Login Screen window (`gui_login()`).
   - Listens for user input, dispatches Ring 3 `.mct` binaries (Explorer, Terminal, Doom, Browser, Notepad, Calculator, etc.).

---

## 📁 Source Code Organization

* `kernel.c` — Main kernel entry point and initialization loop.
* `boot.asm` — Multiboot assembly entry and stack allocation.
* `src/sys/` — System management (GDT, IDT, Task Scheduler, VMM, PMM, ACPI, APIC, SMP, Syscalls, VFS, Ext2, FAT32, FB_MAP, device/block core).
* `src/drivers/` — Hardware device drivers (VGA/VBE, Keyboard, Mouse, PIT Timer, NIC backends: RTL8139 / e1000e / virtio-net, ATA, AHCI, xHCI, SB16, Serial).
* `src/gui/` — Graphical user interface (Window Manager, Desktop, Taskbar, Login Screen).
* `src/apps/` — Built-in Ring 0 & Ring 3 wrapper applications.
* `apps/` — Userland Ring 3 executable sources (`.c`) compiled to `.mct` binaries.

---

## 🔌 Device model

Drivers register themselves through `device_register()` from a linker-section
initcall (`INITCALL(fn, level)`), so `kernel.c` contains one
`device_init_all()` call instead of a driver list. Levels preserve the old
bring-up order (10 ATA DMA, 11 AHCI, 12 xHCI, 13 virtio-blk, 14 virtio-gpu,
15 SB16, 16 rtl8139, 17 e1000e, 18 virtio-net, 20 net). Boot prints `[DEV] initcalls=<n>` and
one `[DEV] block|net <name> ...` line per registered device.

Block devices register a drive range and `block_ops_t`; VFS/ext2/FAT32 call
`block_read()` / `block_write()` and the core routes by drive:
0–3 ATA, 4–7 AHCI, 8–11 USB, 12–15 virtio-blk. NICs keep the existing
priority-based `net_backend` and additionally appear as NET devices.
