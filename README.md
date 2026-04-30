# Mectov OS v22.0 — Kernel Modernization (VMM, IPC, Priority Threading)

The Mectov Kernel — an operating system kernel written from scratch in C and Assembly. No external libraries, no libc, no POSIX — every byte runs directly on hardware.

## About

Mectov OS is a hobby operating system designed as a learning project and technical showcase. It boots via GRUB Multiboot, sets up protected mode with paging, and provides a fully graphical desktop environment with floating windows, custom static wallpapers, persistent draggable icons, hardware detection, standalone Ring 3 user applications, and real internet connectivity.

The v22.0 release introduces kernel modernization: a full Virtual Memory Manager (VMM) with per-process address spaces, IPC named message queues for service-oriented architecture, 4-level priority thread scheduling with sleep/wake API, and 14 new syscalls enabling these capabilities.

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
|  Memory Manager   |  VMM (Virtual Mem)|  IPC Message Queues        |
+--------------------------------------------------------------------+
|  Priority Thread   |  VFS + ATA PIO   |  PCI Scanner               |
+--------------------------------------------------------------------+
|  VGA/VESA Driver   |  Window Manager   |  RTL8139 NIC              |
+--------------------------------------------------------------------+
|  Network Stack (Ethernet/ARP/IPv4/ICMP/UDP/DNS)                    |
+--------------------------------------------------------------------+
|  MCT App Loader   |  Ring 3 User Tasks|  Display List Renderer     |
+--------------------------------------------------------------------+
|  Desktop (Squircle Icons) |  Taskbar (Glossy) |  Start Menu        |
+--------------------------------------------------------------------+
```

### Target Platform
- Architecture: i386 (32-bit x86), Monolithic Kernel
- Ring Levels: Ring 0 (Kernel) + Ring 3 (User Mode) — ACTIVE and Stable
- Scheduler: Preemptive Priority Round-Robin, Ring-Aware Context Switching
- Display: VESA VBE Linear Framebuffer, 1024x768, 32-bit color
- Storage: ATA PIO (IDE), 1MB virtual disk
- Audio: PC Speaker via PIT Channel 2
- Bus: PCI Configuration Space (ports 0xCF8/0xCFC)
- Network: RTL8139 NIC (virtual, via QEMU)
- Serial: COM1/COM2 UART 16550A (38400 baud, 8N1)

---

## Core Subsystems

### 1. Modern Mouse Cursor (src/drivers/vga.c)
- High-Resolution Bitmap: Upgraded from 8x16 to a sleek 16x24 design.
- Premium Contrast: Black inner fill with a crisp white outline for maximum visibility on any background.
- Dynamic Drop Shadow: A real-time 50% alpha-blended shadow cast beneath the cursor for a depth effect.
- Zero Lag: Optimized drawing logic with dirty-region tracking.

### 2. Window Manager (src/gui/wm.c)
- Double-buffered rendering using fast memcpy with dirty region tracking.
- Z-ordered floating windows with proper overlap handling.
- Rounded corners (radius 8) on all windows for a modern look.
- Vibrant macOS-style "traffic light" control buttons with indicator symbols:
  - Close (vibrant red circle with 'X' symbol)
  - Minimize (vibrant yellow circle with '-' symbol)
  - Maximize (vibrant green circle with '+' symbol)
- Clean Flat Aesthetic: Removed heavy shadows around windows to focus on a crisp, modern UI.

### 3. Taskbar & System Tray (src/gui/taskbar.c)
- WiFi Status Indicator: Replaced the RAM bar with a classic 3-arc WiFi icon, representing the OS's network capability.
- "Mectov OS" Start button with vertical separator line for distinct UI partitioning.
- Glossy dark background with Catppuccin Mocha accent border.
- Icon-only window buttons: each open app shown as a 16x16 squircle icon matching the desktop style.
- System tray with:
  - CAPS indicator (vibrant red when active)
  - HDD activity LED (red flash on disk I/O)
  - Digital clock with day of week, adjusted for UTC+7 (WIB) timezone.

### 4. Real-time Rendering (kernel.c)
- Forced 60Hz Loop: Reverted to a constant 16ms redraw cycle to ensure the FPS stays high (~60 FPS) and the interface feels ultra-responsive at all times.
- Optimized Performance: By stripping away unneeded shadow logic, the system maintains high performance even under heavy window loads.
- Microsecond Timing: Real-time FPS and render time measurement using PIT hardware counters.

### 5. Desktop Environment (src/gui/desktop.c)
- Custom Baked Wallpaper: Full-color 1024x768 image processed via Python build script.
- Professional Squircle Icons: High-end rounded-rect icons with minimalist glyphs and curated color palettes.
- Draggable Persistent Icons: Icon positions are saved to icons.cfg on the VFS and persist across system reboots.

### 6. Virtual Memory Manager (src/sys/vmm.c + src/include/vmm.h)
- **Frame Bitmap Allocator:** Tracks 128MB of physical memory (32768 pages) with a compact bitmap for O(1) allocation/free.
- **Per-Process Address Spaces:** `vmm_create_page_dir()` creates new page directories, `vmm_clone_page_dir()` copies kernel mappings for fork-like scenarios, `vmm_free_page_dir()` tears down an address space.
- **Page Mapping:** `vmm_map_page()` / `vmm_map_pages()` / `vmm_unmap_page()` with automatic TLB invalidation on CR3 reload.
- **Region Allocator:** `vmm_find_free_region()` locates free virtual address ranges starting at 0x08000000.
- Foundation for future demand paging and Copy-on-Write (COW) for Ring 3 process isolation.

### 7. IPC — Inter-Process Communication (src/sys/ipc.c + src/include/ipc.h)
- **Named Message Queues:** Fixed-size 64-byte messages with a 16-deep circular buffer per queue.
- **Create & Connect:** `ipc_create_queue(name)` for server processes, `ipc_connect_queue(name)` for clients.
- **Blocking Receive:** `ipc_receive()` blocks the calling task until a message arrives, with tick-based timeout support.
- **Non-blocking Send:** `ipc_send()` returns -1 immediately if the queue is full.
- Enables service-oriented architecture and clean multitasking app ecosystem.

### 8. Priority Thread Scheduler (src/sys/task.c + src/include/task.h)
- **4-Level Priority Round-Robin:** IDLE(0) < LOW(1) < NORMAL(2) < HIGH(3). Higher priority runnable tasks always run first; round-robin scheduling within the same priority level.
- **Thread API:** `task_create_thread()` spawns a thread within the same process (shared address space) using a pid/tid model. `task_exit_thread()` terminates the current thread.
- **Sleep/Wake:** `task_sleep(ticks)` suspends a task for a specified duration; `task_wake()` resumes it. Used for efficient blocking I/O and timing.
- Full context switching: general-purpose registers, EFLAGS, ESP, and per-task `page_dir` pointer for VMM integration.
- Per-task dual stacks: 16KB kernel stack + 8KB user stack.
- IRQ0-driven scheduler tick (1000Hz) with cooperative yield via SYS_YIELD.

### 9. Network Stack (src/drivers/net.c + src/drivers/rtl8139.c)
- RTL8139 NIC Driver: Full driver with PCI bus mastering.
- Ethernet/ARP/IPv4/ICMP/UDP/DNS: Complete local stack.
- Terminal commands: ping [ip] and host [domain].

### 10. Persistent File System (src/sys/vfs.c)
- Virtual File System (16 file slots) with auto-save to disk.img via ATA PIO.
- Persistence Fix: Reliable saving for configuration files like icons.cfg.

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
| 11 | SYS_CREATE_THREAD | Create a thread in current process |
| 12 | SYS_THREAD_SLEEP | Sleep current thread for n ticks |
| 13 | SYS_THREAD_EXIT | Exit current thread |
| 14 | SYS_VMM_CREATE | Create new address space (page directory) |
| 15 | SYS_VMM_FREE | Free an address space |
| 16 | SYS_VMM_SWITCH | Switch to an address space (load CR3) |
| 17 | SYS_VMM_MAP | Map page into address space |
| 18 | SYS_VMM_UNMAP | Unmap page from address space |
| 19 | SYS_VMM_ALLOC_REGION | Allocate virtual region in address space |
| 20 | SYS_IPC_CREATE | Create IPC queue (server side) |
| 21 | SYS_IPC_CONNECT | Connect to existing IPC queue (client side) |
| 22 | SYS_IPC_SEND | Send message via IPC (non-blocking) |
| 23 | SYS_IPC_RECEIVE | Receive message via IPC (blocking with timeout) |
| 24 | SYS_DRAW_RECT | Draw rectangle in window |
| 25 | SYS_DRAW_TEXT | Draw text in window |
| 26 | SYS_GET_KEY | Get keyboard char |
| 27 | SYS_GET_MOUSE | Get mouse state |
| 28 | SYS_CREATE_WINDOW | Create GUI window |
| 29 | SYS_GET_EVENT | Get window event |
| 30 | SYS_UPDATE_WINDOW | Commit draw commands |

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
| v22.0 | **Kernel Modernization:** Virtual Memory Manager (per-process address spaces, page mapping, region allocator), IPC named message queues (non-blocking send, blocking receive with timeout), 4-level priority thread scheduling with sleep/wake API, and 14 new syscalls (VMM/thread/IPC). |
| v21.0 | **Premium UI Refinement:** High-res sleek mouse cursor with dynamic shadow, classic 3-arc WiFi indicator in system tray, and return to forced 60Hz real-time rendering loop. |
| v20.0 | Modern UI Modernization: Professional squircle icons, vibrant macOS buttons with symbols (X, -, +), taskbar separator, flat design removal of shadows. |
| v19.0 | Modern UI Redesign: Glass-morphism icons, Catppuccin theme, rounded corners, glossy taskbar. |
| v18.0 | External App Ecosystem: .mct format, syscalls, Ring 3 apps (Calculator). |
| v17.0 | Terminus Bold font, Draggable icons, VFS persistence. |

---

## License

MIT License

Created by M Alif Fadlan.