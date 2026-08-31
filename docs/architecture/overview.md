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
|  RTL8139 NIC      |  Ethernet/IP/UDP  |  DNS/TCP/Proxy |  Ring 3 MCT Apps   |
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
   - **Networking & VFS**: Initializes ATA PIO driver, mounts Ext2/VFS virtual file system, detects PCI RTL8139 NIC, and sends ARP query to Gateway.

3. **GUI Shell & Userland Startup**:
   - Enables interrupts (`sti`).
   - Allocates VBE double-buffer and renders desktop wallpaper and squircle icons.
   - Renders Login Screen window (`gui_login()`).
   - Listens for user input, dispatches Ring 3 `.mct` binaries (Explorer, Terminal, Doom, Browser, Notepad, Calculator, etc.).

---

## 📁 Source Code Organization

* `kernel.c` — Main kernel entry point and initialization loop.
* `boot.asm` — Multiboot assembly entry and stack allocation.
* `src/sys/` — System management (GDT, IDT, Task Scheduler, VMM, PMM, ACPI, APIC, SMP, Syscalls, VFS, Ext2, FAT32, FB_MAP).
* `src/drivers/` — Hardware device drivers (VGA/VBE, Keyboard, Mouse, PIT Timer, RTL8139 NIC, ATA, AHCI, xHCI, SB16, Serial).
* `src/gui/` — Graphical user interface (Window Manager, Desktop, Taskbar, Login Screen).
* `src/apps/` — Built-in Ring 0 & Ring 3 wrapper applications.
* `apps/` — Userland Ring 3 executable sources (`.c`) compiled to `.mct` binaries.
