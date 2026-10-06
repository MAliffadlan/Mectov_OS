# Mectov OS — Documentation Index

Welcome to the official technical documentation for **Mectov OS**, a monolithic operating system kernel built from scratch in C and Assembly.

One line lives in this repository: the **x86 32-bit kernel** (`src/` + `kernel.c`),
built by bare `make`, run with `./run.sh`. Every document below describes it.

There was a 64-bit kernel (`kernel64.c` + `k64/`, M1–M14) that was briefly the
default build. It was removed once it was clear the momentum had moved the other
way: the release line had gone 36 commits into `src/` against 4 into `k64/`, and
everything the desktop needs — networking, audio, USB, SATA, a writable
filesystem, 54 apps, Quake III Arena — existed only in 32-bit. See
[why 32-bit only](architecture/why-32bit.md) for the reasoning and for the
measurements that port produced, which are still a live to-do list.

---

## 📚 Technical Documentation Map

### 1. Architecture (`docs/architecture/`)
* **[Architecture Overview](architecture/overview.md)** — High-level layout of the kernel, boot sequence, and subsystem interaction.
* **[Multi-Core (SMP) & APIC](architecture/smp_and_apic.md)** — ACPI MADT parsing, APIC/IOAPIC setup, INIT-SIPI-SIPI AP startup, and IRQ overriding.
* **[Memory Management](architecture/memory.md)** — Physical page allocation (PMM), Virtual Memory (VMM/Paging), heap isolation, and process tear-down safety.
* **[Preemptive Scheduler](architecture/scheduler.md)** — Priority Round-Robin scheduler, context switching, interrupt gates, and deadlock prevention.
* **[Syscall Subsystem](architecture/syscalls.md)** — `int 0x80` Ring 3 interface, register passing, and modular syscall dispatching (`syscall_gui`, `syscall_vfs`, `syscall_net`, etc.).
* **[Why 32-bit only](architecture/why-32bit.md)** — why the 64-bit kernel was removed, and the 301-site pointer-truncation measurement it left behind as a 32-bit to-do list.

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
make clean_all && make         # -> myos.bin
make iso                       # -> mectov.iso  (./run.sh does this for you)
./run.sh                      # builds the ISO, boots QEMU (KVM), 4 cores
make check                     # 66-suite gate battery (~25 min)
make check-quick               # 12-suite fast subset
```
