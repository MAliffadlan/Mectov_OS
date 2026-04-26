# Mectov OS v17.0 (The Mectov Kernel)

A bare-metal x86 operating system kernel written from scratch in C and Assembly. No external libraries, no libc, no POSIX -- every byte runs directly on hardware.

## About

Mectov OS is a hobby operating system designed as a learning project and technical showcase. It boots via GRUB Multiboot, sets up protected mode with paging, and provides a fully graphical desktop environment with floating windows, custom static wallpapers, persistent draggable icons, hardware detection, and real internet connectivity.

Created by M Alif Fadlan.

---

## Architecture

```
+---------------------------------------------------------+
|                    GRUB Multiboot                       |
+---------------------------------------------------------+
|  boot.asm  -->  kernel.c  (kernel_main entry point)     |
+---------------------------------------------------------+
|  GDT (Ring 0+3)  |  IDT  |  TSS  |  Syscall (int 0x80) |
+---------------------------------------------------------+
|  PIT Timer  |  Keyboard  |  Mouse  |  Serial (COM1/2)  |
+---------------------------------------------------------+
|  Memory Manager    |  Task Scheduler  |  VFS + ATA PIO  |
+---------------------------------------------------------+
|  VGA/VESA Driver   |  Window Manager  |  PCI Scanner    |
+---------------------------------------------------------+
|  RTL8139 NIC  |  Network Stack (Ethernet/ARP/IP/UDP)    |
+---------------------------------------------------------+
|  Desktop  |  Taskbar  |  Start Menu  |  Applications    |
+---------------------------------------------------------+
```

### Target Platform
- CPU: Intel x86 (i386), Protected Mode
- Ring Levels: Ring 0 (Kernel) + Ring 3 segments ready (User Mode infrastructure)
- Display: VESA VBE Linear Framebuffer, 32-bit color (800x600)
- Storage: ATA PIO (IDE), 1MB virtual disk
- Audio: PC Speaker via PIT Channel 2
- Bus: PCI Configuration Space (ports 0xCF8/0xCFC)
- Network: RTL8139 NIC (virtual, via QEMU)
- Serial: COM1/COM2 UART 16550A

---

## Core Subsystems

### 1. Memory Management (src/sys/mem.c)
- Paging with CR0/CR3 control, identity-mapped up to 128MB
- Dynamic framebuffer mapping for VESA addresses above physical RAM
- Linked-list heap allocator with First-Fit search strategy
- Working `kmalloc()` and `kfree()` with 4-byte alignment
- Block coalescing on free: adjacent free blocks merge automatically to prevent heap fragmentation
- Dynamic bitmap allocation scaled to actual detected RAM size

### 2. Preemptive Multitasking (src/sys/task.c)
- Round-robin scheduler driven by IRQ0 (PIT hardware timer at 60Hz)
- Full context switching: registers, EFLAGS, ESP per task
- 16KB stack per task (8 task slots maximum)
- Lock-free background tasks run concurrently with GUI

### 3. Window Manager (src/gui/wm.c)
- Double-buffered rendering (back buffer to front buffer swap) using fast `memcpy`
- Z-ordered floating windows with proper overlap handling
- Titlebar with gradient rendering (focused vs unfocused states)
- Three window control buttons per window:
  - Minimize [_] : hides window to taskbar, click taskbar button to restore
  - Maximize [O] : expands window to fill screen, click again to restore original size
  - Close [x] : destroys window
- Mouse-driven drag-and-drop repositioning (disabled when maximized)
- Click-through hit testing from front to back
- Focus management: minimizing a window auto-focuses the next visible window

### 4. Desktop Environment (src/gui/desktop.c)
- **Custom Baked Wallpaper**: Full-color 800x600 32-bit BGRA image processed via a Python build script, converted into a raw binary, and injected directly into the kernel using `objcopy` as an `elf32-i386` object. Blitted instantly via `memcpy`.
- **MenuetOS-Style Icons**: Highly detailed procedural pixel-art icons stacked vertically on the left side of the screen with transparent backgrounds and text drop-shadows.
- **Draggable Persistent Icons**: Users can drag and drop icons to any location. Positions are automatically serialized to an `icons.cfg` file on the VFS and persist across system reboots.
- **Typography**: Interface uses the beautiful, highly readable Terminus Bold 16 font instead of standard VGA fonts.
- Double-click to launch applications
- Scrolling marquee banner at the top

### 5. Taskbar (src/gui/taskbar.c)
- "MectovOS" start menu button with application launcher
- **Icon-only window buttons**: each open app is shown as a 16x16 pixel-art icon (matching desktop icons) instead of text labels — compact and visually clean
- Minimized windows shown with dimmed icon colors
- Click behavior: restore minimized, minimize focused, raise unfocused
- System tray with:
  - CAPS indicator (orange when active)
  - HDD activity LED (red flash on disk I/O)
  - RAM usage bar (live percentage)
  - Digital clock with **day of week** (e.g. `Sun 22:36:17`), adjusted for WIB / UTC+7 timezone including day roll-over

### 6. Network Stack (src/drivers/net.c + src/drivers/rtl8139.c)
- **RTL8139 NIC Driver**: Full driver with PCI bus mastering, 4 TX descriptor rotation, RX ring buffer polling
- **Ethernet**: 14-byte frame encapsulation with MAC addressing
- **ARP**: IP-to-MAC resolution with request/reply handling
- **IPv4**: Packet construction with TTL, checksum (RFC 1071), and protocol demux
- **ICMP**: Echo Request/Reply — working `ping` command from terminal
- **UDP**: Connectionless datagram protocol for DNS queries
- **DNS Client**: Queries Google DNS (8.8.8.8), parses A records to resolve domain names
- **Terminal commands**: `ping [ip]` and `host [domain]`

### 7. User Mode & Syscall Infrastructure (src/sys/syscall.c + src/sys/gdt.c)
- **GDT expanded** from 3 to 6 entries:
  - Slot 0: Null
  - Slot 1: Kernel Code (Ring 0, selector 0x08)
  - Slot 2: Kernel Data (Ring 0, selector 0x10)
  - Slot 3: User Code (Ring 3, selector 0x18)
  - Slot 4: User Data (Ring 3, selector 0x20)
  - Slot 5: TSS (selector 0x28)
- **TSS (Task State Segment)**: Loaded at boot, stores kernel stack pointer (ESP0) for automatic Ring 3 → Ring 0 stack switching on interrupts
- **Syscall interface**: `int 0x80` with DPL=3 IDT gate, dispatches via EAX register

### 8. Serial Port Driver (src/drivers/serial.c)
- UART 16550A initialization (38400 baud, 8N1)
- COM1 and COM2 support with configurable `MODEM_PORT`
- Timeout-protected read/write — prevents OS hang if serial port is disconnected or unmapped

### 9. PCI Bus Scanner (src/drivers/pci.c)
- Full enumeration: 256 buses, 32 slots, 8 functions per slot
- Device class identification and PCI write support

### 10. Persistent File System (src/sys/vfs.c)
- Simple Virtual File System with auto-save to disk.img via ATA PIO
- File operations: create, read, write, delete
- GUI integration: File Explorer for browsing, Nano Editor for editing

### 11. FPS Counter (kernel.c)
- Real-time frames-per-second display at top-right corner
- Rendering throttled to 30 FPS, optimized via `memcpy` lines

### 12. Boot and Security
- Startup audio melody via PC Speaker
- Password-protected login screen (default: `mectov123`)

---

## Applications

| Application     | File                    | Description                                      |
|-----------------|-------------------------|--------------------------------------------------|
| Terminal        | src/apps/terminal_app.c | Full terminal emulator with command history       |
| Nano Editor     | src/apps/nano.c         | Windowed text editor for VFS files                |
| File Explorer   | src/apps/explorer_app.c | Browse and open stored files                      |
| System Info     | src/apps/sysinfo_app.c  | Live RAM, CPU, resolution, uptime, and MAC address|
| PCI Manager     | src/apps/pci_app.c      | Scrollable table of detected PCI hardware         |
| Clock           | src/apps/clock_app.c    | Digital clock with WIB timezone                   |
| Snake           | src/apps/snake.c        | Classic snake game                                |
| Power Options   | src/apps/power_app.c    | Shut Down, Restart, and Lock Screen dialog        |
| Mini Browser    | src/apps/browser_app.c  | Text-mode web browser via serial modem proxy      |

---

## Build and Run

### Requirements
- `gcc` (with `-m32` support for 32-bit x86)
- `nasm` (assembler)
- `make`
- `qemu-system-i386`
- `grub-mkrescue` + `xorriso` (for bootable ISO)
- `python3`
- `Pillow` (python library for wallpaper conversion)

### Commands
```bash
# Clean and build (auto-builds wallpaper via python script)
make clean && make

# Run in QEMU
./run.sh
```

---

## Version History

| Version | Highlights                                                                    |
|---------|-------------------------------------------------------------------------------|
| v17.0   | Terminus Bold font, Custom Wallpaper baking (objcopy), MenuetOS-style UI, Draggable persistent icons (saved to VFS), rendering optimizations via memcpy |
| v16.0   | Network stack (RTL8139, Ethernet, ARP, IPv4, ICMP, UDP, DNS), Mini Browser, Serial driver, User Mode infrastructure |
| v15.0   | Gradient wallpaper, procedural icons, window minimize/maximize, FPS counter |
| v14.0   | Linked-list heap allocator with kfree, PCI bus scanner, PCI Manager app |

---

## License

MIT License

Created by M Alif Fadlan.
