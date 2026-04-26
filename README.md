Mectov OS v12.0 (The Foundation Update)
========================================

Mectov OS is a monolithic, bare-metal operating system kernel developed from scratch using C and x86 Assembly. Version 12.0 represents a significant architectural leap, moving from a polling-based system to a modern interrupt-driven framework.

Project Overview
----------------
Mectov OS operates directly on PC hardware (VGA, ATA, PIT, PS/2) without any external libraries (Freestanding C). It features a modular architecture, advanced memory management, and a robust interrupt handling system, providing a solid foundation for future multitasking capabilities.

Key Features
------------

### 🏗️ Core Architecture
- **Interrupt-Driven System (IDT/ISR):** Implements a proper Interrupt Descriptor Table. Hardware events like keyboard input and timer ticks are handled via ISRs, eliminating inefficient CPU polling.
- **Mectov Memory Manager (MMM):** 
    - Physical Memory Management (PMM) using Bitmap tracking.
    - Dynamic allocation via `kmalloc` and `kfree`.
    - Paging support for memory protection and future process isolation.
- **Modular Design:** Source code is surgically separated into `drivers/`, `sys/`, `apps/`, and `include/` for professional maintainability.

### 🔐 Security & Persistence
- **Secure Boot Login:** Initial access protection with masked password input.
- **Persistent Storage:** Native ATA/IDE PIO driver for virtual hard disk support, ensuring files are saved across reboots.
- **Screen Lock:** Immediate session locking via the `kunci` command.

### 📟 User Interface (Mectov TUI)
- **Live System HUD:** Real-time clock (WIB) and system uptime displayed in the dashboard, driven by the hardware timer.
- **Command Shell (Mectovsh):**
    - Tab Auto-complete for all system commands.
    - Command History accessed via the Up Arrow key.
    - Buffered Keyboard Input to prevent keystroke loss during heavy tasks.
- **Visuals:** Floating Window Manager illusion with drop shadows and smooth marquee running text.

### 🎮 Built-in Applications
- **Mectovular (Snake Game):** High-performance Snake game with WASD and Arrow key support.
- **Mectov Nano:** A lightweight terminal-based text editor for on-disk file manipulation.
- **MectovScript v2.0:** A batch scripting engine for automating system tasks with tone synthesis support.

System Commands
---------------
- `mfetch`      : Detailed system info, CPU brand string (CPUID), and RAM usage.
- `mem`         : Display real-time RAM statistics from MMM.
- `waktu`       : Current WIB (Jakarta) time from hardware RTC.
- `ls`, `buat`, `tulis`, `baca`, `hapus`: Persistent file management.
- `edit [file]` : Open the text editor.
- `ular`        : Launch the Snake game.
- `man [cmd]`   : Access the internal manual book for help.
- `matikan`     : ACPI Shutdown.
- `mulaiulang`  : Hardware CPU Reset.

Installation and Execution
--------------------------
### Requirements
- GCC (i386 multilib)
- NASM
- GNU Make
- QEMU

### Building
```bash
./build.sh
```

### Running
```bash
./run.sh
```
*Note: Default system password is `mectov123`.*

Created and maintained by Alif Fadlan.
Licensed under the MIT License.