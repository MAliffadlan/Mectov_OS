Mectov OS v14.0 (Kernel Upgrade & PCI Detection)
==================================================

Mectov OS is a bare-metal operating system kernel developed in C and x86 Assembly. Version 14.0 introduces a dynamic memory allocator with proper kfree, PCI Bus hardware detection, a File Explorer GUI, and a Power Options dialog.

Technical Overview
------------------
Mectov OS operates directly on x86 hardware without any external dependencies. It implements a modular architecture with memory protection, persistent storage via ATA PIO, PCI bus enumeration, and a responsive interrupt-driven GUI environment.

Core Features
-------------

### 1. Preemptive Multitasking & Scheduling
- Round-Robin Scheduler via IRQ0 (PIT hardware timer).
- Context Switching across independent tasks (Registers, EFLAGS, ESP).
- Lock-Free Concurrency: background tasks will not freeze the GUI or I/O.

### 2. Window Manager & GUI
- Double-Buffered Rendering with VESA LFB 32-bit backbuffer.
- Floating windows with z-ordering, drag-and-drop, and close buttons.
- Mouse click events forwarded to individual window content areas.
- Taskbar with real-time RAM bar, WIB Clock, Caps Lock indicator, and HDD activity LED.
- Start Menu with quick-launch for all installed applications including Power options.
- Marquee scrolling banner at 25 FPS.

### 3. Memory Management
- Virtual Memory with Paging (CR0 bit 31).
- Dynamic Identity Mapping up to 128MB based on detected RAM.
- Linked-List Heap Allocator with working kmalloc and kfree (First-Fit algorithm).
- 4-byte aligned allocations with block metadata tracking.

### 4. PCI Bus Scanner
- Full enumeration of 256 buses, 32 slots, 8 functions per slot.
- Vendor identification database: Intel, AMD, NVIDIA, QEMU/Bochs, Red Hat, VMware, VirtualBox, Realtek, Broadcom.
- Device class identification: VGA, IDE, SATA, NVMe, Ethernet, USB, Audio, Host Bridge, ISA Bridge, and more.
- Results accessible via GUI (PCI Device Manager app) and CLI (lspci command).

### 5. Persistence and VFS
- Auto-Save VFS: changes flushed to disk.img via ATA PIO on every file operation.
- GUI Editor integration with automatic disk persistence on save.
- File Explorer GUI for browsing and opening files with a single click.

### 6. Boot Experience and Security
- Startup audio melody synthesized via PC Speaker (PIT Channel 2).
- Mandatory login screen with password authentication before desktop access.

GUI Applications
----------------
- Terminal       : Full terminal emulator with command history.
- Nano Editor    : Windowed text editor for VFS files.
- File Explorer  : Browse and open all files stored on disk.
- SysInfo        : Real-time RAM, CPU, resolution, and uptime monitoring.
- PCI Manager    : Lists all detected PCI hardware with vendor and class info.
- Clock          : Digital clock displaying WIB (UTC+7) time.
- Snake          : Classic snake game in a window.
- Power Options  : GUI dialog for Shut Down, Restart, and Lock Screen.

System Commands (Terminal)
--------------------------
- `help`         : Show all available commands.
- `mfetch`       : System fetch display.
- `mem`          : RAM usage statistics.
- `waktu`        : Hardware RTC time (WIB).
- `ls`           : List all files.
- `buat [file]`  : Create a new file.
- `baca [file]`  : Read file contents.
- `edit [file]`  : Open file in GUI Nano Editor.
- `hapus [file]` : Delete a file.
- `lspci`        : List all detected PCI bus devices.
- `clear`        : Clear terminal buffer.
- `matikan`      : ACPI soft-off shutdown.
- `mulaiulang`   : Triple-fault CPU reset.
- `kunci`        : Lock screen.

Taskbar System Tray
-------------------
- CAPS indicator : Orange when Caps Lock is active, gray when off.
- HDD indicator  : Red flash on disk read/write activity.
- RAM bar        : Live usage percentage.
- Clock          : Hours, minutes, seconds in WIB.

Build and Deployment
--------------------
Requirements: `i386-elf-gcc`, `nasm`, `make`, `qemu`.

1. Build the kernel: `make` (run `make clean` first after major changes)
2. Launch in emulation: `./run.sh`

Default System Password: mectov123

Created by Alif Fadlan.
Licensed under the MIT License.
