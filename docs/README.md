# Mectov OS — Documentation Index

Welcome to the official technical documentation for **Mectov OS**, an x86 32-bit monolithic operating system kernel built from scratch in C and Assembly.

---

## 📚 Technical Documentation Map

### 1. Architecture (`docs/architecture/`)
* **[Architecture Overview](file:///home/mectov/my-os/docs/architecture/overview.md)** — High-level layout of the kernel, boot sequence, and subsystem interaction.
* **[Multi-Core (SMP) & APIC](file:///home/mectov/my-os/docs/architecture/smp_and_apic.md)** — ACPI MADT parsing, APIC/IOAPIC setup, INIT-SIPI-SIPI AP startup, and IRQ overriding.
* **[Memory Management](file:///home/mectov/my-os/docs/architecture/memory.md)** — Physical page allocation (PMM), Virtual Memory (VMM/Paging), heap isolation, and process tear-down safety.
* **[Preemptive Scheduler](file:///home/mectov/my-os/docs/architecture/scheduler.md)** — Priority Round-Robin scheduler, context switching, interrupt gates, and deadlock prevention.
* **[Syscall Subsystem](file:///home/mectov/my-os/docs/architecture/syscalls.md)** — `int 0x80` Ring 3 interface, register passing, and modular syscall dispatching (`syscall_gui`, `syscall_vfs`, `syscall_net`, etc.).

### 2. Device Drivers (`docs/drivers/`)
* **[VGA / VBE Video Driver](file:///home/mectov/my-os/docs/drivers/vga_vbe.md)** — 1024x768 VESA VBE linear framebuffer, triple-buffer rendering, dirty region tracking, and hardware mouse cursor.
* **[RTL8139 Network Stack](file:///home/mectov/my-os/docs/drivers/network.md)** — PCI detection, RTL8139 packet polling, Ethernet/ARP/IPv4/ICMP/UDP/DNS stack, and Host Web Proxy integration.
* **[Input & Audio Drivers](file:///home/mectov/my-os/docs/drivers/input_and_sound.md)** — PS/2 Keyboard and Mouse handlers, PC Speaker sound generation, and SB16 DAC support.

### 3. User Interface & Window Manager (`docs/gui/`)
* **[Window Manager & Desktop Shell](file:///home/mectov/my-os/docs/gui/window_manager.md)** — Double-buffered window compositor, Z-order layering, Aero Snap window docking, desktop squircle icons, and taskbar.

---

## 🛠️ Quick Build & Run Instructions

```bash
# Compile the kernel and standalone MCT user apps
make clean && make

# Run in QEMU with VBE display, 256MB RAM, and 4 CPU Cores (SMP)
./run.sh

# Run headless (serial debug output to serial_debug.log)
./run_headless.sh
```
