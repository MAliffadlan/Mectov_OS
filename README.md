Mectov OS v10.5 (The Professional Bare-Metal Update)
===================================================

Mectov OS is a monolithic, bare-metal operating system kernel developed from scratch using C and x86 Assembly. It is designed for educational research into low-level systems, hardware drivers, and kernel architecture.

Project Overview
----------------
Unlike high-level applications, Mectov OS operates directly on PC hardware (VGA, ATA, PIT, PS/2). It implements its own Window Manager (TUI), Virtual File System, and Shell environment without any external libraries.

Core Features
-------------
- **Security & Integrity:**
    - Secure Boot Login with masked password authentication.
    - Persistent Screen Lock (`kunci`) integrated with the security module.
- **Hardware Interaction:**
    - CPUID Identity Detection: Automatically identifies the host processor.
    - Persistent Storage: Full ATA/IDE PIO driver for virtual hard disk support.
    - Real-Time Clock: Localized WIB (UTC+7) hardware clock reading.
    - Audio Synthesis: Native PC Speaker driver for tone and melody generation.
- **User Interface (Mectov TUI):**
    - Floating Window Manager illusion with drop shadows and desktop background.
    - Live System HUD: Real-time clock and uptime display in the dashboard.
    - Hardware Cursor: Stable blinking underscore cursor synchronization.
    - Marquee System: Smooth running text on the bottom system bar.
- **Command Shell (Mectovsh):**
    - Tab Auto-complete: Intelligent command suggestion and completion.
    - Command History: Access previously executed commands using the Up Arrow.
    - Manual System: Built-in documentation via the `man` command.
- **Embedded Applications:**
    - Mectov Nano: A lightweight terminal-based text editor.
    - Mectovular: A high-performance Snake game with WASD and Arrow key support.
    - Scripting Engine: MectovScript v2.0 for automated batch task execution.

System Command Reference
------------------------
### Administration & Power
- `mfetch`      : Comprehensive system information and hardware specs.
- `help`        : Formatted ASCII table of all available commands.
- `waktu`       : Display current WIB time (Jakarta).
- `warna`       : Cycle through terminal color themes.
- `matikan`     : Shutdown the system via ACPI.
- `mulaiulang`  : Perform a hardware CPU reset.
- `kunci`       : Lock the terminal session immediately.
- `clear`       : Reset the terminal workspace.

### File Management
- `ls`          : List persistent files on the ATA drive.
- `buat [name]` : Initialize a new empty file.
- `edit [name]` : Invoke the Mectov Nano editor.
- `baca [name]` : Output file contents to the terminal.
- `hapus [name]`: Permanently remove a file from disk.
- `tulis [name] [text]`: Direct CLI write to a specific file.

### Scripting & Entertainment
- `ular`        : Launch the Mectov Snake game.
- `man [cmd]`   : Access the manual entry for a command.
- `beep`        : Execute a system audio test.
- `echo [text]` : Print string to the workspace.
- `jalankan [file]`: Execute a MectovScript (.ms) file.

Installation and Execution
--------------------------
### Requirements
- GCC (with multilib support for i386)
- NASM (Netwide Assembler)
- QEMU (PC System Emulator)

### Building from Source
Use the provided build script:
    ./build.sh

Manual build process:
    nasm -f elf32 boot.asm -o boot.o
    gcc -m32 -c kernel.c -o kernel.o -std=gnu99 -ffreestanding -O2 -Wall -Wextra
    ld -m elf_i386 -T linker.ld boot.o kernel.o -o myos.bin

### Running the System
Use the provided run script:
    ./run.sh

Manual QEMU execution (with persistent storage and audio):
    qemu-system-i386 -kernel myos.bin -audiodev alsa,id=snd0 -machine pcspk-audiodev=snd0 -drive file=disk.img,format=raw,index=0,media=disk

*Note: The default system password is `mectov123`.*

Development
-----------
Mectov OS is an open-source project. All core logic resides within `kernel.c` and `boot.asm`.

Created and maintained by Alif Fadlan.
Licensed under the MIT License.