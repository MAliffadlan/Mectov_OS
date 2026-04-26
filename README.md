Mectov OS v11.0 (The Modular & Memory Update)
=================================================

Mectov OS is a monolithic, bare-metal operating system kernel developed from scratch using C and x86 Assembly. Version 11.0 marks a major milestone with a fully modular architecture and the introduction of the Mectov Memory Manager (MMM).

Modular Architecture
--------------------
The project has been refactored into a structured hierarchy for professional-grade development:
- `src/drivers/`: Low-level hardware drivers (VGA, Keyboard, ATA, Speaker).
- `src/sys/`: Core system services (Memory Management, File System, Security).
- `src/apps/`: Integrated applications (Nano Editor, Snake Game).
- `src/include/`: Unified header files for inter-module communication.

Core Features
-------------
- **Mectov Memory Manager (MMM):** 
    - Physical Memory Management (PMM) using Bitmap tracking.
    - Simple Dynamic Allocation (`kmalloc`) for kernel services.
    - Paging support as a foundation for advanced multitasking.
- **Security & Integrity:**
    - Secure Boot Login with masked password authentication.
    - Persistent Screen Lock (`kunci`) and session protection.
- **Hardware Interaction:**
    - CPUID Identity Detection: Native identification of the host processor.
    - Persistent Storage: ATA/IDE PIO driver for virtual hard disk support.
    - Real-Time Clock: Localized WIB (UTC+7) hardware clock reading.
- **User Interface (Mectov TUI):**
    - Floating Window Manager with Live System HUD (Real-time Clock & Uptime).
    - Stable Hardware Cursor and Smooth Marquee running text.
- **Command Shell (Mectovsh):**
    - Tab Auto-complete and Command History (Up Arrow).
    - Integrated Manual System (`man` command).

System Command Reference
------------------------
### Administration & Power
- `mfetch`      : Detailed system info, CPU brand, and memory usage.
- `mem`         : Display RAM statistics (Total, Used, Free).
- `waktu`       : Display current WIB time (Jakarta).
- `kunci`       : Lock the system session.
- `matikan`     : Shutdown via ACPI.
- `mulaiulang`  : Hardware CPU reset.

### File & Scripting
- `ls`, `buat`, `tulis`, `baca`, `hapus`: Persistent file management.
- `edit [name]` : Open Mectov Nano text editor.
- `jalankan`    : Execute MectovScript (.ms) batch files.
- `ular`        : Launch the Mectov Snake game (WASD/Arrow keys).

Installation and Execution
--------------------------
### Building
Requirements: `gcc`, `nasm`, `make`, `qemu`, `gcc-multilib`.
    ./build.sh

### Running
    ./run.sh

*Note: Default system password is `mectov123`.*

Created and maintained by Alif Fadlan.
Licensed under the MIT License.