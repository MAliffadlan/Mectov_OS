Mectov OS v13.0 (The Memory & Stability Update)
================================================

Mectov OS is a professional-grade, monolithic, bare-metal operating system kernel developed in C and x86 Assembly. Version 13.0 introduces stable Virtual Memory management and a redesigned interrupt handling framework to ensure high system reliability.

Technical Overview
------------------
Mectov OS operates directly on x86 hardware without any external dependencies. It implements a modular architecture with a focus on memory protection, persistent storage via ATA PIO, and a responsive interrupt-driven user environment.

Core Features
-------------

### 1. Memory Management (Paging)
- **Virtual Memory:** Full implementation of Paging with CR0 bit 31 control.
- **Identity Mapping:** 16MB Identity Mapping enabled to safely cover kernel code, stack, and VGA memory.
- **Physical Memory Manager (PMM):** Bitmap-based tracking for the total 32MB managed RAM.
- **Dynamic Allocation:** Kernel-level `kmalloc` and `kfree` for efficient heap management.

### 2. System Stability and Interrupts
- **Pointer-Based Dispatcher:** ISR framework redesigned to use pointer-based context passing, resolving stack/struct mismatch issues.
- **IDT Configuration:** Dedicated handlers for IRQ0 (PIT @ 50Hz) and IRQ1 (Keyboard).
- **Deadlock Prevention:** Removed hardware-wait loops (VGA retrace) from interrupt context to ensure continuous timer reliability.

### 3. Boot Experience and Audio
- **Startup Splash:** High-contrast Cyan/Green ASCII branding during initialization.
- **Audio Jingle:** Rising startup melody (A4-C5-E5) synthesized via the PC Speaker (PIT Channel 2).

### 4. Persistence and VFS
- **VFS Structure:** Custom 1536-byte file entry layout (3 sectors) for metadata and data.
- **ATA PIO Driver:** Native driver for persistent read/write operations on IDE hard disks.

### 5. Security
- **Access Control:** Mandatory secure login on boot with masked password input.
- **Session Protection:** Hardware-level locking via the `kunci` command.

User Interface (Mectovsh)
-------------------------
- **Tab Auto-complete:** Intelligent command completion for all internal binaries.
- **Command History:** Buffer-based history accessible via Arrow keys.
- **Non-blocking Input:** Custom keyboard buffer implementation to ensure responsiveness during heavy I/O.

System Commands
---------------
- `mfetch`      : Technical system summary and memory usage.
- `mem`         : Real-time RAM statistics from the MMM.
- `waktu`       : Hardware RTC time (WIB/Jakarta).
- `ls`, `buat`, `tulis`, `baca`, `hapus`: Persistent file manipulation.
- `edit [file]` : Terminal-based text editor (Mectov Nano).
- `ular`        : High-performance Snake game.
- `matikan`     : ACPI soft-off shutdown.
- `mulaiulang`  : Triple-fault triggered CPU reset.

Build and Deployment
--------------------
Requirements: i386-elf-gcc, nasm, make, qemu.

1. Build the kernel: `make` or `./build.sh`
2. Launch in emulation: `./run.sh`

*Default System Password: mectov123*

Created by Alif Fadlan.
Licensed under the MIT License.
