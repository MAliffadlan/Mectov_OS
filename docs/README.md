# Mectov OS — Documentation Index

Welcome to the official technical documentation for **Mectov OS**, a monolithic operating system kernel built from scratch in C and Assembly.

Two lines live in this repository:

* **x86_64 (mainline)** — `kernel64.c` + `k64/`, built by bare `make`, run with `./run64.sh`. This is where development happens.
* **x86 32-bit (original line, reference)** — `src/` + `kernel.c`, still fully buildable as `make all32` and runnable with `./run.sh`. The documents below describe that line; where it solves a problem well, the 64-bit kernel reuses the idea (see [the 64-bit audit and roadmap](architecture/x86_64_port.md)).

---

## 📚 Technical Documentation Map

### 1. Architecture (`docs/architecture/`)
* **[Architecture Overview](architecture/overview.md)** — High-level layout of the kernel, boot sequence, and subsystem interaction.
* **[Multi-Core (SMP) & APIC](architecture/smp_and_apic.md)** — ACPI MADT parsing, APIC/IOAPIC setup, INIT-SIPI-SIPI AP startup, and IRQ overriding.
* **[Memory Management](architecture/memory.md)** — Physical page allocation (PMM), Virtual Memory (VMM/Paging), heap isolation, and process tear-down safety.
* **[Preemptive Scheduler](architecture/scheduler.md)** — Priority Round-Robin scheduler, context switching, interrupt gates, and deadlock prevention.
* **[Syscall Subsystem](architecture/syscalls.md)** — `int 0x80` Ring 3 interface, register passing, and modular syscall dispatching (`syscall_gui`, `syscall_vfs`, `syscall_net`, etc.).
* **[x86_64 Port](architecture/x86_64_port.md)** — 64-bit kernel (`kernel64.c`, `k64/`) milestone status, measured `-m64` reusability of `src/`, the dependency map of the 32-bit desktop, the M10→M16 delivery order, and per-milestone design notes (M10 heap, M11 block layer, M12 filesystems).

### 2. Device Drivers (`docs/drivers/`)
* **[VGA / VBE Video Driver](drivers/vga_vbe.md)** — 1024x768 VESA VBE linear framebuffer, triple-buffer rendering, dirty region tracking, and hardware mouse cursor.
* **[AHCI / SATA Driver](drivers/ahci.md)** — Intel AHCI HBA (BAR5), single PRD via 64K bounce, LBA48 DMA EXT, poll PxCI; drives 4-7 on ATA API.
* **[xHCI / USB 3.0 Driver](drivers/usb_xhci.md)** — QEMU qemu-xhci (BAR0), SuperSpeed/HS, BOT mass-storage drives 8+ on ATA API.
* **[Ring 3 Scanout Takeover](drivers/fbmap.md)** — `SYS_FB_MAP`/`RELEASE`, `PAGE_DEV` device memory, logind-style auth, desktop suppression.
* **[RTL8139 Network Stack](drivers/network.md)** — PCI detection, RTL8139 packet polling, Ethernet/ARP/IPv4/ICMP/UDP/DNS stack, and Host Web Proxy integration.
* **[Input & Audio Drivers](drivers/input_and_sound.md)** — PS/2 Keyboard and Mouse handlers, PC Speaker sound generation, and SB16 DAC support.

### 3. User Interface & Window Manager (`docs/gui/`)
* **[Window Manager & Desktop Shell](gui/window_manager.md)** — Double-buffered window compositor, Z-order layering, Aero Snap window docking, desktop squircle icons, and taskbar.

### 4. Developer Guides (`docs/`)
* **[Git/GitHub Authentication](git-auth.md)** — Set up SSH keys or Git Credential Manager for safe `git push` without exposing tokens in chat or config.

---

## 🛠️ Quick Build & Run Instructions

```bash
# 64-bit (default): kernel + ISO, then boot it in QEMU
make clean64 && make          # -> myos64.bin, mectov64.iso
./run64.sh                    # windowed QEMU; ./run64.sh --headless for the gate
make check64                  # 64-bit gate battery + the scripts/ tests

# 32-bit (original line)
make all32                    # -> myos.bin
./run.sh                      # builds the ISO itself, boots QEMU (KVM), 4 cores
```
