# Mectov OS v20.0 — Modern UI Modernization & Performance Fixes

The Mectov Kernel — an operating system kernel written from scratch in C and Assembly. No external libraries, no libc, no POSIX — every byte runs directly on hardware.

## About

Mectov OS is a hobby operating system designed as a learning project and technical showcase. It boots via GRUB Multiboot, sets up protected mode with paging, and provides a fully graphical desktop environment with floating windows, custom static wallpapers, persistent draggable icons, hardware detection, standalone Ring 3 user applications, and real internet connectivity.

The v20.0 release focuses on UI modernization and performance stability: professional squircle icon aesthetics, vibrant macOS-style window controls with indicator symbols, and a transition to event-driven rendering for optimized FPS.

Created by M Alif Fadlan.

---

## Architecture

```
+--------------------------------------------------------------------+
|                       GRUB Multiboot (VBE)                         |
+--------------------------------------------------------------------+
|  boot.asm  -->  kernel.c  (kernel_main entry point)                |
+--------------------------------------------------------------------+
|  GDT (Ring 0+3)  |  IDT (ISR+IRQ)  |  TSS  |  Syscall (int 0x80)   |
+--------------------------------------------------------------------+
|  PIT Timer (1kHz) |  Keyboard (PS/2) |  Mouse (PS/2) |  Serial     |
+--------------------------------------------------------------------+
|  Memory Manager    |  Task Scheduler    |  VFS + ATA PIO           |
+--------------------------------------------------------------------+
|  VGA/VESA Driver   |  Window Manager    |  PCI Scanner             |
+--------------------------------------------------------------------+
|  RTL8139 NIC  |  Network Stack (Ethernet/ARP/IPv4/ICMP/UDP/DNS)    |
+--------------------------------------------------------------------+
|  MCT App Loader  |  Ring 3 User Tasks  |  Display List Renderer    |
+--------------------------------------------------------------------+
|  Desktop (Squircle Icons) |  Taskbar (Glossy) |  Start Menu        |
+--------------------------------------------------------------------+
```

### Target Platform
- Architecture: i386 (32-bit x86), Monolithic Kernel
- Ring Levels: Ring 0 (Kernel) + Ring 3 (User Mode) — ACTIVE and Stable
- Scheduler: Preemptive Round-Robin, Ring-Aware Context Switching
- Display: VESA VBE Linear Framebuffer, 1024x768, 32-bit color
- Storage: ATA PIO (IDE), 1MB virtual disk
- Audio: PC Speaker via PIT Channel 2
- Bus: PCI Configuration Space (ports 0xCF8/0xCFC)
- Network: RTL8139 NIC (virtual, via QEMU)
- Serial: COM1/COM2 UART 16550A (38400 baud, 8N1)

---

## Core Subsystems

### 1. Memory Management (src/sys/mem.c)
- Paging with CR0/CR3 control, identity-mapped up to 128MB
- Dynamic framebuffer mapping for VESA addresses above physical RAM
- Linked-list heap allocator with First-Fit search strategy
- Working kmalloc() and kfree() with 4-byte alignment
- Block coalescing on free: adjacent free blocks merge automatically to prevent heap fragmentation
- Dynamic bitmap allocation scaled to actual detected RAM size
- User-mode page mapping (PAGE_USER | PAGE_RW | PAGE_PRESENT) for Ring 3 app memory at 0x02000000

### 2. Preemptive Multitasking (src/sys/task.c)
- Round-robin scheduler driven by IRQ0 (PIT hardware timer at 1000Hz)
- Full context switching: registers, EFLAGS, ESP per task
- Per-task dual stacks: 16KB kernel stack + 8KB user stack
- 8 task slots maximum (Task 0 = kernel main loop)
- Ring-aware scheduling: handles both Ring 0 and Ring 3 tasks
- Graceful task termination: task_exit() cleanly marks task as free, scheduler auto-recovers to next ready task
- CPU-friendly idle: hlt instruction when no work to do

### 3. Window Manager (src/gui/wm.c)
- Double-buffered rendering (back buffer -> front buffer swap) using fast memcpy with dirty region tracking
- Z-ordered floating windows with proper overlap handling
- Rounded corners (radius 8) on all windows for a modern look
- Vibrant macOS-style "traffic light" control buttons with indicator symbols:
  - Close (vibrant red circle with 'X' symbol)
  - Minimize (vibrant yellow circle with '-' symbol)
  - Maximize (vibrant green circle with '+' symbol)
- Titlebar with Catppuccin Mocha gradient (focused vibrant vs unfocused dimmed)
- Mouse-driven drag-and-drop repositioning (disabled when maximized)
- Focus management: closing/minimizing a window auto-focuses the next visible window
- callback system for per-window app rendering and input

### 4. Desktop Environment (src/gui/desktop.c)
- Custom Baked Wallpaper: Full-color 1024x768 32-bit BGRA image processed via Python build script (scratch/build_wallpaper.py), converted into a raw binary, and injected directly into the kernel binary.
- Professional Squircle Icons: High-end rounded-rect icons with minimalist glyphs and curated color palettes (Terminal: Dark Slate, Explorer: Vibrant Blue, etc.).
- Unified Icon Parity: The taskbar uses the same squircle design language as the desktop for a cohesive visual experience.
- Draggable Persistent Icons: Users can drag and drop icons to any location. Positions are saved to icons.cfg on the VFS and persist across system reboots.
- Start Menu: Left-click taskbar button opens a clean application launcher menu.

### 5. Taskbar (src/gui/taskbar.c)
- "Mectov OS" Start button with vertical separator line for distinct UI partitioning.
- Glossy dark background with Catppuccin Mocha accent border.
- Icon-only window buttons: each open app shown as a 16x16 squircle icon matching the desktop style.
- System tray with:
  - CAPS indicator (vibrant red when active)
  - RAM usage percentage
  - Digital clock with day of week, adjusted for UTC+7 (WIB) timezone.

### 6. Network Stack (src/drivers/net.c + src/drivers/rtl8139.c)
- RTL8139 NIC Driver: Full driver with PCI bus mastering, 4 TX descriptor rotation, RX ring buffer polling
- Ethernet: 14-byte frame encapsulation with MAC addressing
- ARP: IP-to-MAC resolution with request/reply handling
- IPv4: Packet construction with TTL, checksum (RFC 1071), and protocol demux
- ICMP: Echo Request/Reply — working ping command from terminal
- UDP: Connectionless datagram protocol for DNS queries
- DNS Client: Queries Google DNS (8.8.8.8), parses A records to resolve domain names
- Terminal commands: ping [ip] and host [domain]

### 7. User Mode & Syscall Infrastructure (src/sys/syscall.c + src/sys/gdt.c)
- Ring 3 Isolation: User programs run in Ring 3 with separate user stacks. CPU automatically switches to kernel stack via TSS on syscall/interrupt entry.
- Ring-Aware Scheduler: Correctly manages SS and ESP switching via iret and updates TSS.esp0 for every task.
- Syscall interface: int 0x80 with 17 available functions.
- Memory Isolation: Identity-mapped memory with PAGE_USER bit.
- Display List Renderer: Ring 3 apps draw via system calls into a pending command buffer, replayed by the Window Manager.

### 8. Performance Optimization (kernel.c)
- Event-Driven Rendering: Removed forced 60Hz full-screen redraws. The OS now only redacts when a UI event occurs (mouse movement, clicks, keypresses, or clock ticks), significantly stabilizing FPS and reducing CPU overhead.
- Optimized Shadow Rendering: Removed complex alpha-blended shadows to maintain a clean "flat" aesthetic and ensure maximum performance during window dragging.
- Microsecond Timing: Real-time FPS and render time measurement using PIT hardware counters.

### 9. Persistent File System (src/sys/vfs.c)
- Virtual File System (16 file slots) with auto-save to disk.img via ATA PIO.
- Persistence Fix: Verified file creation and writing to ensure configuration files (like icons.cfg) commit successfully to physical storage.

---

## Syscall API Reference

All syscalls are invoked via int 0x80. Register conventions: EAX=syscall number, EBX/ECX/EDX/ESI/EDI=arguments.

| # | Name | Description |
|---|------|-------------|
| 1 | SYS_PRINT | Print string to screen |
| 2 | SYS_OPEN | Open/create VFS file |
| 3 | SYS_READ | Read from file |
| 4 | SYS_WRITE | Write to file |
| 5 | SYS_CLOSE | Close file descriptor |
| 6 | SYS_MALLOC | Allocate kernel heap memory |
| 7 | SYS_FREE | Free allocated memory |
| 8 | SYS_GET_TICKS | Get PIT timer ticks |
| 9 | SYS_YIELD | Yield CPU to scheduler |
| 10 | SYS_EXIT | Terminate current task |
| 11 | SYS_DRAW_RECT | Draw rectangle in window |
| 12 | SYS_DRAW_TEXT | Draw text in window |
| 13 | SYS_GET_KEY | Get keyboard char |
| 14 | SYS_GET_MOUSE | Get mouse state |
| 15 | SYS_CREATE_WINDOW | Create GUI window |
| 16 | SYS_GET_EVENT | Get window event |
| 17 | SYS_UPDATE_WINDOW | Commit draw commands |

---

## Applications

| Application | Type | Description |
|---|---|---|
| Terminal | Ring 0 | Full terminal emulator with command history |
| Nano Editor | Ring 0 | Windowed text editor for VFS files |
| File Explorer | Ring 0 | Browse and open stored files |
| System Info | Ring 0 | Live RAM, CPU, resolution, uptime, and MAC address |
| PCI Manager | Ring 0 | Scrollable table of detected PCI hardware |
| Clock | Ring 0 | Digital clock with WIB timezone |
| Snake | Ring 0 | Classic snake game with arrow key controls |
| Power Options | Ring 0 | Shut Down, Restart, and Lock Screen dialog |
| Mini Browser | Ring 0 | Text-mode web browser via serial modem proxy |
| GUI Calculator | Ring 3 (.mct) | Standalone external GUI calculator |

---

## Build and Run

### Requirements
- gcc (with -m32 support)
- nasm
- make
- qemu-system-i386
- python3 + Pillow

### Commands
```bash
# Clean and build the OS
make clean && make

# Run in QEMU
./run.sh
```

---

## Version History

| Version | Highlights |
|---|---|
| v20.0 | **Modern UI Modernization:** Professional squircle icon design, vibrant macOS-style window controls with symbols (X, -, +), taskbar layout separator, removal of shadows for clean flat look, and event-driven rendering for FPS stability. |
| v19.0 | Modern UI Redesign: Glass-morphism icons, Catppuccin theme, rounded corners, glossy taskbar. |
| v18.0 | External App Ecosystem: .mct format, syscalls, Ring 3 apps (Calculator). |
| v17.0 | Terminus Bold font, Draggable icons, VFS persistence. |
| v16.0 | Network stack, Mini Browser, ICMP/DNS support. |

---

## License

MIT License

Created by M Alif Fadlan.