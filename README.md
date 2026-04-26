# Mectov OS v16.0

A bare-metal x86 operating system kernel written from scratch in C and Assembly. No external libraries, no libc, no POSIX -- every byte runs directly on hardware.

## About

Mectov OS is a hobby operating system designed as a learning project and technical showcase. It boots via GRUB Multiboot, sets up protected mode with paging, and provides a fully graphical desktop environment with floating windows, mouse input, persistent file storage, hardware detection, and real internet connectivity.

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
- Double-buffered rendering (back buffer to front buffer swap)
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
- Gradient wallpaper: navy-to-charcoal gradient with teal midband, rendered procedurally and cached in RAM for zero per-frame cost
- Procedural pixel-art icons for each application (no bitmap files needed):
  - Terminal: black screen with green ">_" prompt
  - Snake: game board with snake body and red apple
  - Clock: white face with hour and minute hands
  - SysInfo: CPU chip with pin legs on all sides
  - Explorer: yellow folder with tab
  - PCI: green PCB board with IC and gold contacts
  - Browser: white page with blue lines and border
- Double-click to launch applications
- Scrolling marquee banner at the top

### 5. Taskbar (src/gui/taskbar.c)
- "Mectov" start menu button with application launcher
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
- **Syscall interface**: `int 0x80` with DPL=3 IDT gate, dispatches via EAX register:
  - `SYS_PRINT (0)` — Print string to screen
  - `SYS_GETKEY (1)` — Read keyboard input
  - `SYS_MALLOC (2)` — Allocate heap memory
  - `SYS_FREE (3)` — Free heap memory
  - `SYS_GET_TICKS (4)` — Read system timer
  - `SYS_YIELD (5)` — Yield CPU time
  - `SYS_EXIT (6)` — Terminate process
- **`switch_to_user_mode()`**: Ready to activate via `iret` — pushes Ring 3 selectors + EFLAGS with IF set

> **Note**: The switch to Ring 3 is currently disabled because all applications use direct hardware I/O (`in`/`out` instructions), which would trigger a General Protection Fault in user mode. The infrastructure is fully installed and can be activated once applications are refactored to use syscalls.

### 8. Serial Port Driver (src/drivers/serial.c)
- UART 16550A initialization (38400 baud, 8N1)
- COM1 and COM2 support with configurable `MODEM_PORT`
- Timeout-protected read/write — prevents OS hang if serial port is disconnected or unmapped
- Dead-port detection (0xFF status guard)
- Used for browser proxy communication

### 9. PCI Bus Scanner (src/drivers/pci.c)
- Full enumeration: 256 buses, 32 slots, 8 functions per slot
- Vendor database: Intel, AMD, NVIDIA, QEMU/Bochs, Red Hat, VMware, VirtualBox, Realtek, Broadcom
- Device class identification: VGA, IDE, SATA, NVMe, Ethernet, USB, Audio, Host Bridge, ISA Bridge, and more
- PCI write support for enabling bus mastering on network cards
- Results exposed via GUI (PCI Device Manager) and CLI (`lspci`)

### 10. Persistent File System (src/sys/vfs.c)
- Simple Virtual File System with auto-save to disk.img via ATA PIO
- File operations: create, read, write, delete
- GUI integration: File Explorer for browsing, Nano Editor for editing

### 11. FPS Counter (kernel.c)
- Real-time frames-per-second display at top-right corner
- Green text on black background, updated every second
- Rendering throttled to 30 FPS (every 2 ticks at 60Hz PIT)
- Input polling runs at full 60Hz for responsive mouse and keyboard

### 12. Boot and Security
- Startup audio melody via PC Speaker
- Password-protected login screen (default: `mectov123`)
- Power Options dialog: Shut Down (ACPI), Restart (triple-fault), Lock Screen

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

## Terminal Commands

```
GENERAL : help, clear, mfetch, mem, waktu
FILES   : ls, buat [file], baca [file], edit [file], hapus [file]
APPS    : ular, nada, beep, echo
POWER   : matikan, mulaiulang, kunci
HW      : lspci
NET     : ping [ip], host [domain]
```

### Network Examples
```bash
# Ping QEMU gateway
ping 10.0.2.2
# Reply from 10.0.2.2 time=0ms

# Resolve domain name via Google DNS
host google.com
# google.com has address 74.125.24.101
```

---

## Project Structure

```
my-os/
  boot.asm              Multiboot header and entry point (NASM)
  kernel.c              Kernel main, event loop, FPS counter
  linker.ld             Linker script (load at 1MB)
  Makefile              Build system (auto-discovers .c and .asm files)
  run.sh                QEMU launch script with GRUB ISO
  disk.img              1MB persistent storage image
  modem.py              Python serial modem proxy for browser
  src/
    drivers/
      ata.c             ATA PIO disk driver
      font8x16.c        Built-in 8x16 bitmap font
      keyboard.c        PS/2 keyboard with scancode translation
      mouse.c           PS/2 mouse with interrupt-driven input
      net.c             Network stack: Ethernet, ARP, IPv4, ICMP, UDP, DNS
      pci.c             PCI configuration space bus scanner + write
      rtl8139.c         RTL8139 network card driver
      serial.c          UART 16550A serial port driver (COM1/COM2)
      speaker.c         PC Speaker tone generation
      timer.c           PIT Channel 0 (60Hz system tick)
      vga.c             VESA VBE framebuffer, drawing primitives, double buffer
    sys/
      gdt.c             Global Descriptor Table (Ring 0 + Ring 3 + TSS)
      idt.c             Interrupt Descriptor Table, ISR, and syscall gate
      interrupt_entry.asm  ISR/IRQ/Syscall assembly stubs
      mem.c             Paging, kmalloc/kfree heap allocator
      security.c        Password verification
      shell.c           Command parser and built-in commands
      syscall.c         Syscall handler (int 0x80) and user mode switch
      task.c            Preemptive multitasking scheduler
      utils.c           String utilities, memset, memcpy, BCD conversion
      vfs.c             Virtual file system with ATA persistence
    gui/
      desktop.c         Desktop rendering, gradient wallpaper, icons
      login.c           Login screen with password input
      taskbar.c         Taskbar, start menu, system tray
      wm.c              Window manager (open, close, minimize, maximize, drag)
    apps/
      browser_app.c     Text-mode web browser (serial proxy)
      clock_app.c       Digital clock application
      explorer_app.c    File explorer GUI
      nano.c            Text editor
      pci_app.c         PCI device manager
      power_app.c       Power options dialog
      snake.c           Snake game
      sysinfo_app.c     System information monitor (with network status)
      terminal_app.c    Terminal emulator
    include/
      (header files for all modules)
```

---

## Build and Run

### Requirements
- `gcc` (with `-m32` support for 32-bit x86)
- `nasm` (assembler)
- `make`
- `qemu-system-i386`
- `grub-mkrescue` + `xorriso` (for bootable ISO)
- `python3` (optional, for browser modem proxy)

### Commands
```bash
# Clean and build
make clean && make

# Run in QEMU
./run.sh

# Optional: start the browser modem proxy (in another terminal)
python3 modem.py
```

### Default Login
```
Password: mectov123
```

---

## Version History

| Version | Highlights                                                                    |
|---------|-------------------------------------------------------------------------------|
| v16.0   | Network stack (RTL8139, Ethernet, ARP, IPv4, ICMP, UDP, DNS), Mini Browser, Serial driver, User Mode infrastructure (GDT Ring 3, TSS, Syscall int 0x80) |
| v15.0   | Gradient wallpaper, procedural icons, window minimize/maximize, FPS counter, heap coalescing, 16KB task stacks |
| v14.0   | Linked-list heap allocator with kfree, PCI bus scanner, PCI Manager app, Power Options dialog |
| v13.0   | File Explorer GUI, Start Menu with Power entry                                |
| v12.0   | Double-buffered rendering, mouse-driven window manager                        |
| v11.0   | ATA PIO persistence, Nano Editor, VFS auto-save                              |
| v10.0   | Preemptive multitasking, context switching                                    |

---

## Technical Notes

- The kernel runs in Ring 0. Ring 3 user mode segments and TSS are installed but not yet activated — all apps currently use direct hardware I/O.
- The syscall interface (`int 0x80`) is registered and functional. User-mode programs can invoke kernel services via the `syscall()` inline function defined in `syscall.h`.
- The network stack supports ICMP ping and UDP-based DNS resolution through a custom RTL8139 driver. TCP is not yet implemented.
- The browser application uses a serial port proxy (`modem.py`) running on the host to fetch web content, stripping HTML and forwarding plain text to Mectov OS.
- The heap allocator uses a linked-list with forward and backward coalescing on kfree to prevent fragmentation.
- The gradient wallpaper is computed once at boot and cached in heap memory for zero per-frame rendering cost.
- FPS is measured by counting rendered frames between 60-tick (1 second) intervals.

---

## Roadmap

- [ ] **Activate User Mode**: Refactor applications to use syscalls instead of direct I/O, then enable `switch_to_user_mode()`
- [ ] **DHCP Client**: Automatic IP assignment instead of hardcoded `10.0.2.15`
- [ ] **TCP Stack**: Enable HTTP connections for native web browsing
- [ ] **ELF Loader**: Load and execute external user programs from disk
- [ ] **Sound Blaster Driver**: PCM audio playback beyond PC Speaker beeps

---

## License

MIT License

Created by M Alif Fadlan.
