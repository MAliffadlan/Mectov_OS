# Mectov OS v18.0  (The Mectov Kernel)

The Mectov Kernel — an operating system kernel written from scratch in C and Assembly. No external libraries, no libc, no POSIX — every byte runs directly on hardware.

## About

Mectov OS is a hobby operating system designed as a learning project and technical showcase. It boots via GRUB Multiboot, sets up protected mode with paging, and provides a fully graphical desktop environment with floating windows, custom static wallpapers, persistent draggable icons, hardware detection, standalone Ring 3 user applications, and real internet connectivity.

Created by **M Alif Fadlan**.

---

## Architecture

```
+--------------------------------------------------------------------+
|                       GRUB Multiboot (VBE)                         |
+--------------------------------------------------------------------+
|  boot.asm  -->  kernel.c  (kernel_main entry point)                |
+--------------------------------------------------------------------+
|  GDT (Ring 0+3)  |  IDT (ISR+IRQ)  |  TSS  |  Syscall (int 0x80) |
+--------------------------------------------------------------------+
|  PIT Timer (1kHz) |  Keyboard (PS/2) |  Mouse (PS/2) |  Serial    |
+--------------------------------------------------------------------+
|  Memory Manager    |  Task Scheduler    |  VFS + ATA PIO           |
+--------------------------------------------------------------------+
|  VGA/VESA Driver   |  Window Manager    |  PCI Scanner             |
+--------------------------------------------------------------------+
|  RTL8139 NIC  |  Network Stack (Ethernet/ARP/IPv4/ICMP/UDP/DNS)   |
+--------------------------------------------------------------------+
|  MCT App Loader  |  Ring 3 User Tasks  |  Display List Renderer    |
+--------------------------------------------------------------------+
|  Desktop  |  Taskbar  |  Start Menu  |  8 Desktop Icons            |
+--------------------------------------------------------------------+
```

### Target Platform
- **Architecture**: i386 (32-bit x86), Monolithic Kernel
- **Ring Levels**: Ring 0 (Kernel) + Ring 3 (User Mode) — ACTIVE and Stable
- **Scheduler**: Preemptive Round-Robin, Ring-Aware Context Switching
- **Display**: VESA VBE Linear Framebuffer, 1024×768, 32-bit color
- **Storage**: ATA PIO (IDE), 1MB virtual disk
- **Audio**: PC Speaker via PIT Channel 2
- **Bus**: PCI Configuration Space (ports 0xCF8/0xCFC)
- **Network**: RTL8139 NIC (virtual, via QEMU)
- **Serial**: COM1/COM2 UART 16550A (38400 baud, 8N1)

---

## Core Subsystems

### 1. Memory Management (src/sys/mem.c)
- Paging with CR0/CR3 control, identity-mapped up to 128MB
- Dynamic framebuffer mapping for VESA addresses above physical RAM
- Linked-list heap allocator with First-Fit search strategy
- Working `kmalloc()` and `kfree()` with 4-byte alignment
- Block coalescing on free: adjacent free blocks merge automatically to prevent heap fragmentation
- Dynamic bitmap allocation scaled to actual detected RAM size
- User-mode page mapping (`PAGE_USER | PAGE_RW | PAGE_PRESENT`) for Ring 3 app memory at `0x02000000`

### 2. Preemptive Multitasking (src/sys/task.c)
- Round-robin scheduler driven by IRQ0 (PIT hardware timer at 1000Hz)
- Full context switching: registers, EFLAGS, ESP per task
- Per-task dual stacks: 16KB kernel stack + 8KB user stack
- 8 task slots maximum (Task 0 = kernel main loop)
- Ring-aware scheduling: handles both Ring 0 and Ring 3 tasks
- Graceful task termination: `task_exit()` cleanly marks task as free, scheduler auto-recovers to next ready task
- CPU-friendly idle: `hlt` instruction when no work to do

### 3. Window Manager (src/gui/wm.c)
- Double-buffered rendering (back buffer → front buffer swap) using fast `memcpy` with dirty region tracking
- Z-ordered floating windows with proper overlap handling
- Titlebar with gradient rendering (focused vs unfocused states)
- Glassmorphism transparency effects on inactive windows
- Three window control buttons per window:
  - Minimize [_] : hides window to taskbar, click taskbar button to restore
  - Maximize [O] : expands window to fill screen, click again to restore original size
  - Close [x] : destroys window and triggers app cleanup
- Mouse-driven drag-and-drop repositioning (disabled when maximized)
- Click-through hit testing from front to back
- Focus management: closing/minimizing a window auto-focuses the next visible window
- `draw_fn` / `key_fn` / `mouse_fn` callback system for per-window app rendering and input

### 4. Desktop Environment (src/gui/desktop.c)
- **Custom Baked Wallpaper**: Full-color 1024×768 32-bit BGRA image processed via Python build script (`scratch/build_wallpaper.py`), converted into a raw binary, and injected directly into the kernel binary using `objcopy` as an `elf32-i386` object. Blitted instantly via `memcpy`.
- **MenuetOS-Style Icons**: 8 highly detailed procedural pixel-art icons arranged vertically on the left side of the screen with transparent backgrounds and text drop-shadows
- **Draggable Persistent Icons**: Users can drag and drop icons to any location. Positions are automatically serialized to an `icons.cfg` file on the VFS and persist across system reboots
- **Double-click to launch**: Applications open on double-click with configurable tick threshold
- **Scrolling marquee banner** at the top of the desktop

### 5. Taskbar (src/gui/taskbar.c)
- "MectovOS" start menu button with application launcher
- **Icon-only window buttons**: each open app is shown as a 16×16 pixel-art icon (matching desktop icons) instead of text labels — compact and visually clean
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
- **Ring 3 Isolation**: User programs run in Ring 3 with separate user stacks. CPU automatically switches to kernel stack via TSS on syscall/interrupt entry.
- **Ring-Aware Scheduler**: The scheduler correctly manages `SS` and `ESP` switching via `iret` and updates `TSS.esp0` for every task to ensure safe kernel entries from Ring 3.
- **Syscall interface**: `int 0x80` with DPL=3 IDT gate, 17 syscall functions available (see Syscall API table below).
- **Overflow Exception Handler**: ISR 4 (INTO) registered with DPL=3, allowing Ring 3 apps to safely trigger overflow checks.
- **Memory Isolation**: Identity-mapped memory (0–128MB) with `PAGE_USER` bit, allowing Ring 3 execution within the mapped physical range.
- **Display List Renderer**: Ring 3 apps draw via `SYS_DRAW_RECT` / `SYS_DRAW_TEXT` into a pending command buffer, which is swapped to the active display list on `SYS_UPDATE_WINDOW`. The Window Manager replays the display list during each frame via `draw_fn`.
- **Window Event Queue**: Per-window circular event buffer (64 events) delivers paint, keyboard, and mouse events to Ring 3 apps via `SYS_GET_EVENT`.

### 8. MCT App Loader (src/sys/loader.c)
- **`.mct` Executable Format**: Custom binary format with a 16-byte header containing magic number (`MCT1`), entry point offset, code size, and data/BSS size.
- **Load Process**: Reads `.mct` from VFS → maps pages at `0x02000000` with User/RW/Present flags → copies code → zeroes BSS → flushes TLB → creates Ring 3 task.
- **Python Toolchain**:
  - `build_mct.py`: Compiles C source → ELF → extracts binary → wraps with `.mct` header
  - `inject_vfs.py`: Injects `.mct` files directly into the VFS partition of `disk.img`

### 9. Serial Port Driver (src/drivers/serial.c)
- UART 16550A initialization (38400 baud, 8N1)
- COM1 and COM2 support with configurable `MODEM_PORT`
- Timeout-protected read/write — prevents OS hang if serial port is disconnected or unmapped
- Debug logging functions: `write_serial()`, `write_serial_string()`, `write_serial_hex()`

### 10. PCI Bus Scanner (src/drivers/pci.c)
- Full enumeration: 256 buses, 32 slots, 8 functions per slot
- Device class identification and PCI write support

### 11. Persistent File System (src/sys/vfs.c)
- Simple Virtual File System (16 file slots) with auto-save to `disk.img` via ATA PIO
- File operations: create, read, write, delete
- Signature-based integrity check (`MECTOV` magic at sector 0)
- GUI integration: File Explorer for browsing, Nano Editor for editing

### 12. FPS & Performance Counter (kernel.c)
- Real-time frames-per-second display at top-right corner
- **Render time measurement** in microseconds using PIT hardware counter (`timer_get_us()`)
- Target ~60 FPS (16ms frame interval), event-driven rendering with dirty region tracking
- V-Sync synchronization before buffer swap

### 13. Boot and Security
- Startup audio melody via PC Speaker
- Password-protected login screen (default: `mectov123`)
- **Terminus Bold 16 font** for beautiful, highly readable typography (replaces standard VGA fonts)

---

## Syscall API Reference

All syscalls are invoked via `int 0x80`. Register conventions: `EAX`=syscall number, `EBX`/`ECX`/`EDX`/`ESI`/`EDI`=arguments.

| # | Name | Args | Description |
|---|------|------|-------------|
| 1 | `SYS_PRINT` | EBX=str, ECX=color | Print string to screen |
| 2 | `SYS_OPEN` | EBX=filename | Open/create VFS file → returns fd |
| 3 | `SYS_READ` | EBX=fd, ECX=buf, EDX=size | Read from file → returns bytes read |
| 4 | `SYS_WRITE` | EBX=fd, ECX=buf, EDX=size | Write to file → returns bytes written |
| 5 | `SYS_CLOSE` | EBX=fd | Close file descriptor |
| 6 | `SYS_MALLOC` | EBX=size | Allocate kernel heap memory → returns pointer |
| 7 | `SYS_FREE` | EBX=ptr | Free allocated memory |
| 8 | `SYS_GET_TICKS` | — | Get PIT timer ticks → returns uint32 |
| 9 | `SYS_YIELD` | — | Yield CPU to scheduler |
| 10 | `SYS_EXIT` | — | Terminate current task |
| 11 | `SYS_DRAW_RECT` | EBX=wid, ECX=x, EDX=y, ESI=(w<<16\|h), EDI=color | Draw rectangle in window |
| 12 | `SYS_DRAW_TEXT` | EBX=wid, ECX=x, EDX=y, ESI=str, EDI=color | Draw text in window |
| 13 | `SYS_GET_KEY` | — | Get keyboard char (non-blocking) |
| 14 | `SYS_GET_MOUSE` | — | Get mouse state (x, y, buttons) |
| 15 | `SYS_CREATE_WINDOW` | EBX=x, ECX=y, EDX=w, ESI=h, EDI=title | Create GUI window → returns win_id |
| 16 | `SYS_GET_EVENT` | EBX=wid, ECX=event_ptr | Get window event (paint/key/mouse) |
| 17 | `SYS_UPDATE_WINDOW` | EBX=wid | Commit pending draw commands to display |

---

## Applications Mectov OS Kernel

| Application     | File                    | Type | Description                                      |
|-----------------|-------------------------|------|--------------------------------------------------|
| Terminal        | src/apps/terminal_app.c | Ring 0 | Full terminal emulator with command history       |
| Nano Editor     | src/apps/nano.c         | Ring 0 | Windowed text editor for VFS files                |
| File Explorer   | src/apps/explorer_app.c | Ring 0 | Browse and open stored files                      |
| System Info     | src/apps/sysinfo_app.c  | Ring 0 | Live RAM, CPU, resolution, uptime, and MAC address|
| PCI Manager     | src/apps/pci_app.c      | Ring 0 | Scrollable table of detected PCI hardware         |
| Clock           | src/apps/clock_app.c    | Ring 0 | Digital clock with WIB timezone                   |
| Snake           | src/apps/snake.c        | Ring 0 | Classic snake game with arrow key controls        |
| Power Options   | src/apps/power_app.c    | Ring 0 | Shut Down, Restart, and Lock Screen dialog        |
| Mini Browser    | src/apps/browser_app.c  | Ring 0 | Text-mode web browser via serial modem proxy      |
| GUI Calculator  | apps/gcalc.c            | **Ring 3 (.mct)** | Standalone external GUI calculator with button grid |

---

## Project Structure

```
my-os/
├── boot.asm                    # Multiboot entry, VBE 1024x768 setup
├── kernel.c                    # Main kernel loop, event handling, FPS counter
├── linker.ld                   # Kernel linker script
├── Makefile                    # Build system
├── run.sh                      # Build + QEMU launch script
├── build_mct.py                # .mct executable builder (C → MCT)
├── inject_vfs.py               # VFS disk image injector
├── apps/
│   └── gcalc.c                 # Ring 3 GUI Calculator source
├── src/
│   ├── drivers/
│   │   ├── ata.c               # ATA PIO disk driver
│   │   ├── font8x16.c          # Terminus Bold 16 font data
│   │   ├── keyboard.c          # PS/2 keyboard driver
│   │   ├── mouse.c             # PS/2 mouse driver
│   │   ├── net.c               # Network stack (Ethernet/ARP/IPv4/ICMP/UDP/DNS)
│   │   ├── pci.c               # PCI bus scanner
│   │   ├── rtl8139.c           # RTL8139 NIC driver
│   │   ├── serial.c            # COM1/COM2 UART driver
│   │   ├── speaker.c           # PC Speaker audio
│   │   ├── timer.c             # PIT timer (1000Hz) + microsecond counter
│   │   └── vga.c               # VESA framebuffer + double buffering
│   ├── gui/
│   │   ├── desktop.c           # Desktop icons, wallpaper, drag-and-drop
│   │   ├── login.c             # Password login screen
│   │   ├── taskbar.c           # Taskbar + system tray
│   │   └── wm.c                # Window Manager (Z-order, titlebar, glassmorphism)
│   ├── sys/
│   │   ├── gdt.c               # GDT + TSS setup (Ring 0/3)
│   │   ├── idt.c               # IDT + IRQ/ISR handlers
│   │   ├── interrupt_entry.asm # Low-level interrupt stubs
│   │   ├── loader.c            # .mct app loader (VFS → Ring 3)
│   │   ├── mem.c               # Paging + heap allocator
│   │   ├── security.c          # Login credentials
│   │   ├── shell.c             # Text-mode shell commands
│   │   ├── syscall.c           # Syscall dispatcher (17 functions)
│   │   ├── task.c              # Preemptive scheduler + task management
│   │   ├── utils.c             # String/memory utilities
│   │   └── vfs.c               # Virtual File System
│   ├── apps/                   # Ring 0 GUI applications
│   └── include/                # Header files
└── scratch/
    ├── build_wallpaper.py      # PNG → raw BGRA converter
    └── gen_font.py             # Font generation tool
```

---

## Build and Run

### Requirements
- `gcc` (with `-m32` support for 32-bit x86)
- `nasm` (assembler)
- `make`
- `qemu-system-i386`
- `grub-mkrescue` + `xorriso` (for bootable ISO)
- `python3`
- `Pillow` (Python library for wallpaper conversion)

### Commands
```bash
# Clean and build the OS (auto-builds wallpaper via python script)
make clean && make

# Run in QEMU
./run.sh
```

### Compiling and Injecting Standalone Apps (.mct)
Mectov OS supports external Ring 3 GUI applications. To build and install a standalone app like `gcalc`:

```bash
# 1. Compile C source to object file
gcc -m32 -ffreestanding -fno-stack-protector -fno-pie -fno-pic -static -O0 -s -c apps/gcalc.c -o apps/gcalc.o

# 2. Link to Ring 3 app memory address (0x02000000)
ld -m elf_i386 -Ttext 0x02000000 --entry _start apps/gcalc.o -o apps/gcalc.elf

# 3. Extract pure flat binary from ELF
objcopy -O binary apps/gcalc.elf apps/gcalc.bin

# 4. Wrap with .mct header (magic, entry point, sizes)
python3 build_mct.py apps/gcalc.bin gcalc.mct

# 5. Inject into the VFS partition of disk.img
python3 inject_vfs.py disk.img gcalc.mct gcalc.mct
```

---

## Version History

| Version | Highlights |
|---------|------------|
| v18.0   | **External App Ecosystem:** Standalone `.mct` binary format and loader, `build_mct.py` + `inject_vfs.py` toolchain, 17 Syscall API functions, GUI Display List renderer, Window event queues, Ring 3 GUI Calculator (`gcalc`), task scheduler auto-recovery on app exit, overflow exception handler (ISR 4), 1024×768 resolution upgrade. |
| v17.1   | **Stable Ring 3 User Mode**, Ring-aware scheduler, TSS stack switching fix, Syscall graphics rendering. |
| v17.0   | Terminus Bold font, Custom Wallpaper baking (objcopy), MenuetOS-style UI, Draggable persistent icons (saved to VFS), rendering optimizations via memcpy. |
| v16.0   | Network stack (RTL8139, Ethernet, ARP, IPv4, ICMP, UDP, DNS), Mini Browser, Serial driver, User Mode infrastructure. |
| v15.0   | Gradient wallpaper, procedural icons, window minimize/maximize, FPS counter. |
| v14.0   | Linked-list heap allocator with kfree, PCI bus scanner, PCI Manager app. |

---

## License

MIT License

Created by M Alif Fadlan.
