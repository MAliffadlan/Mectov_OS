Mectov OS v13.5 (GUI & Multitasking Update)
================================================

Mectov OS is a professional-grade, bare-metal operating system kernel developed in C and x86 Assembly. Version 13.5 introduces a massive architectural leap with a custom Graphical User Interface (Mint XFCE Theme), Window Manager, and true Preemptive Multitasking.

Technical Overview
------------------
Mectov OS operates directly on x86 hardware without any external dependencies. It implements a modular architecture with a focus on memory protection, persistent storage via ATA PIO, and a highly responsive interrupt-driven GUI environment.

Core Features
-------------

### 1. Preemptive Multitasking & Scheduling
- **Round-Robin Scheduler:** True CPU time-slicing via IRQ0 (PIT hardware timer).
- **Context Switching:** Seamlessly swaps CPU state (Registers, EFLAGS, ESP) across independent tasks.
- **Lock-Free Concurrency:** Infinite loops in background tasks will no longer freeze the OS GUI or I/O.

### 2. Window Manager & GUI
- **Double-Buffered Rendering:** VESA LFB graphics with a dedicated 32-bit backbuffer to prevent screen tearing.
- **Window Management:** Supports floating windows, z-ordering (bring to front), and overlapping elements.
- **Taskbar & Start Menu:** Modern bottom taskbar with real-time RAM usage, WIB Clock, and an interactive Start Menu for quick app launching.
- **Marquee Overlay:** Smooth scrolling banner animation rendered globally at 25 FPS without input lag.

### 3. Memory Management (Paging)
- **Virtual Memory:** Full implementation of Paging with CR0 bit 31 control.
- **Identity Mapping:** 32MB Identity Mapping enabled to safely cover kernel code, stack, and VGA framebuffer.
- **Dynamic Allocation:** Kernel-level `kmalloc` and `kfree` for efficient heap management.

### 4. Persistence and VFS
- **Auto-Save Mechanism:** The Virtual File System (VFS) instantly flushes changes to `disk.img` via ATA PIO upon file creation, modification, or deletion.
- **GUI Editor Integration:** Built-in floating GUI text editor (`nano.c`) with automatic disk persistence on exit.

### 5. Boot Experience and Security
- **Startup Audio:** Rising startup melody (A4-C5-E5) synthesized via the PC Speaker (PIT Channel 2).
- **Access Control:** Mandatory secure login screen implemented natively in the VESA GUI before accessing the desktop.

GUI Applications
----------------
- **Mectov Terminal:** High-performance terminal emulator running within a window.
- **Nano Editor:** Interactive, windowed text editor for modifying VFS files.
- **SysInfo:** Real-time RAM, Kernel, and Uptime monitoring tool.
- **Clock:** Analog/Digital hybrid clock displaying WIB (UTC+7) time.
- **Snake:** Classic snake game ported to the new window system.

System Commands (Terminal)
--------------------------
- `waktu`       : Hardware RTC time (WIB/Jakarta).
- `ls`, `buat`, `baca`, `hapus`: Persistent file manipulation.
- `edit [file]` : Opens the file in the GUI Nano Editor.
- `jalankan`    : Executes script commands from a file.
- `clear`       : Clears the terminal GUI buffer.
- `matikan`     : ACPI soft-off shutdown.
- `mulaiulang`  : Triple-fault triggered CPU reset.

Build and Deployment
--------------------
Requirements: `i386-elf-gcc`, `nasm`, `make`, `qemu`.

1. Build the kernel: `make` (Always run `make clean` first after heavy updates)
2. Launch in emulation: `./run.sh`

*Default System Password: mectov123*

Created by Alif Fadlan.
Licensed under the MIT License.
