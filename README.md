# Mectov OS v23.0 — Performance & Stability (Shadow Framebuffer, Process Lifecycle, Terminal UX)

The Mectov Kernel — an operating system kernel written from scratch in C and Assembly. No external libraries, no libc, no POSIX — every byte runs directly on hardware.

## About

Mectov OS is a hobby operating system designed as a learning project and technical showcase. It boots via GRUB Multiboot, sets up protected mode with paging, and provides a fully graphical desktop environment with floating windows, custom static wallpapers, persistent draggable icons, hardware detection, standalone Ring 3 user applications, and real internet connectivity.

The v23.0 release delivers a massive performance and stability overhaul: Shadow Framebuffer rendering (delta-only MMIO writes), proper process lifecycle management with zombie detection and Ctrl+C signal support, a fully rewritten Snake game as a non-blocking WM app, smarter tab-completion, terminal input protection, and power menu bugfixes.

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
|  File Descriptors    |  UNIX Pipe           |  Login Screen         |
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
- **Aero Snap:** Drag windows to screen edges for automatic half-screen (left/right) or full-screen (top) snapping with saved geometry restore.
- **Window Resizing:** Drag any window edge or corner to resize. 8-directional edge detection with minimum size constraints (220×150).
- **Window Close Callback:** `wm_close` now notifies the terminal to kill child processes and reset state.
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

### 4. Real-time Rendering (kernel.c + src/drivers/vga.c)
- **Shadow Framebuffer:** Triple-buffer architecture (back_buffer → front_buffer_copy → VGA MMIO). Only pixels that actually changed are written to hardware, eliminating thousands of expensive KVM VM-Exits per frame.
- **VSync Disabled:** Removed VGA port 0x3DA polling which caused massive latency spikes under KVM virtualization.
- Forced 60Hz Loop: Constant 16ms redraw cycle with ultra-low render times (~4ms) thanks to delta-only copying.
- Microsecond Timing: Real-time FPS and render time measurement using PIT hardware counters.

### 5. Desktop Environment (src/gui/desktop.c)
- Custom Baked Wallpaper: Full-color 1024x768 image processed via Python build script.
- Professional Squircle Icons: High-end rounded-rect icons with minimalist glyphs and curated color palettes.
- Draggable Persistent Icons: Icon positions are saved to icons.cfg on the VFS and persist across system reboots.

### 6. Login Screen (src/gui/login.c)
- Graphical login with wallpaper backdrop and semi-transparent overlay.
- Password input field with masked characters (●) and shake animation on wrong password.
- CAPS LOCK indicator warning.
- Sound feedback (beep) on login events.

### 7. Virtual Memory Manager (src/sys/vmm.c + src/include/vmm.h)
- **Frame Bitmap Allocator:** Tracks 128MB of physical memory (32768 pages) with a compact bitmap for O(1) allocation/free.
- **Per-Process Address Spaces:** `vmm_create_page_dir()` creates new page directories, `vmm_clone_page_dir()` copies kernel mappings for fork-like scenarios, `vmm_free_page_dir()` tears down an address space.
- **Page Mapping:** `vmm_map_page()` / `vmm_map_pages()` / `vmm_unmap_page()` with automatic TLB invalidation on CR3 reload.
- **Region Allocator:** `vmm_find_free_region()` locates free virtual address ranges starting at 0x08000000.
- Foundation for future demand paging and Copy-on-Write (COW) for Ring 3 process isolation.

### 8. IPC — Inter-Process Communication (src/sys/ipc.c + src/include/ipc.h)
- **Named Message Queues:** Fixed-size 64-byte messages with a 16-deep circular buffer per queue.
- **Create & Connect:** `ipc_create_queue(name)` for server processes, `ipc_connect_queue(name)` for clients.
- **Blocking Receive:** `ipc_receive()` blocks the calling task until a message arrives, with tick-based timeout support.
- **Non-blocking Send:** `ipc_send()` returns -1 immediately if the queue is full.
- Enables service-oriented architecture and clean multitasking app ecosystem.

### 9. Priority Thread Scheduler (src/sys/task.c + src/include/task.h)
- **4-Level Priority Round-Robin:** IDLE(0) < LOW(1) < NORMAL(2) < HIGH(3). Higher priority runnable tasks always run first; round-robin scheduling within the same priority level.
- **Thread API:** `task_create_thread()` spawns a thread within the same process (shared address space) using a pid/tid model. `task_exit_thread()` terminates the current thread.
- **Sleep/Wake:** `task_sleep(ticks)` suspends a task for a specified duration; `task_wake()` resumes it. Used for efficient blocking I/O and timing.
- **Process Lifecycle:** `task_kill(tid)` for external termination (used by Ctrl+C). Zombie process detection in SYS_GET_EVENT auto-kills orphaned tasks.
- **True CPU Yielding:** SYS_YIELD now executes `sti;hlt;cli` to properly surrender the CPU until the next scheduler tick.
- Full context switching: general-purpose registers, EFLAGS, ESP, and per-task `page_dir` pointer for VMM integration.
- Per-task dual stacks: 16KB kernel stack + 8KB user stack.
- IRQ0-driven scheduler tick (1000Hz) with cooperative yield via SYS_YIELD.

### 10. Network Stack (src/drivers/net.c + src/drivers/rtl8139.c)
- RTL8139 NIC Driver: Full driver with PCI bus mastering.
- Ethernet/ARP/IPv4/ICMP/UDP/DNS: Complete local stack.
- Terminal commands: ping [ip] and host [domain].

### 11. Persistent File System (src/sys/vfs.c)
- Virtual File System (16 file slots) with auto-save to disk.img via ATA PIO.
- Persistence Fix: Reliable saving for configuration files like icons.cfg.

### 12. File Descriptor Layer (src/sys/fd.c + src/include/fd.h)
- **UNIX-style FD abstraction:** Per-task file descriptor table (16 FDs per task, 128 global) wrapping VFS nodes.
- **Typed descriptors:** `FD_TYPE_FILE`, `FD_TYPE_PIPE_READ`, `FD_TYPE_PIPE_WRITE`, `FD_TYPE_DEV`.
- **Reference counting:** Global FD entries with `ref_count` for safe sharing between threads.
- **UNIX Pipe support:** `do_sys_pipe()` creates a unidirectional pipe pair for inter-process data streaming.
- Full syscall integration: `sys_open`, `sys_read`, `sys_write`, `sys_close`, `sys_pipe` all route through the FD layer.

### 13. MCT Application Runtime (.mct)
- **Custom Binary Format:** 16-byte header with magic number verification and entry point specification.
- **Fixed-Base Mapping:** Applications are mapped at virtual address `0x02000000` within their own isolated page directory.
- **Privilege Isolation:** Clean transition from Ring 0 to Ring 3 via `iret`, ensuring user apps cannot execute privileged instructions.
- **Independent Stacks:** Each Ring 3 task maintains separate 16KB kernel and 8KB user stacks.

### 14. User-Mode GUI API
- **Direct Window Management:** Ring 3 applications can now create, raise, and close their own GUI windows via syscalls.
- **Graphics Primitive Syscalls:** Accelerated `SYS_DRAW_RECT` and `SYS_DRAW_TEXT` for rendering directly into the application's window buffer.
- **Event Polling:** `SYS_GET_EVENT` allows user apps to respond to window-specific mouse and keyboard input.
- **Flicker-Free Updates:** `SYS_UPDATE_WINDOW` ensures changes are committed to the display list and rendered during the next 60Hz frame sync.

### 15. Security & Pointer Validation (src/sys/syscall.c)
- **Safe Syscall Entry:** Every pointer passed from Ring 3 is validated via `validate_user_ptr` to prevent kernel memory corruption or unauthorized data access.
- **Address Boundary Checks:** Enforces strict memory boundaries (`USER_MEM_LIMIT`) for all syscall parameters.
- **Zombie Cleanup:** The kernel automatically detects and terminates Ring 3 processes whose GUI windows have been closed, preventing orphaned tasks and resource leaks.
- **Privilege Separation:** Use of Global Descriptor Table (GDT) and Task State Segment (TSS) to strictly enforce CPU privilege levels (Ring 0 vs Ring 3).

---

## Syscall API Reference

All syscalls are invoked via `int 0x80`. Register conventions: `EAX`=syscall number, `EBX`/`ECX`/`EDX`/`ESI`/`EDI`=arguments.

### Core Syscalls (1–10)
| # | Name | Description |
|---|------|-------------|
| 1 | SYS_PRINT | Print string. EBX=str_ptr, ECX=color |
| 2 | SYS_OPEN | Open/create VFS file. EBX=filename → return fd |
| 3 | SYS_READ | Read file. EBX=fd, ECX=buf, EDX=size → bytes read |
| 4 | SYS_WRITE | Write file. EBX=fd, ECX=buf, EDX=size → bytes written |
| 5 | SYS_CLOSE | Close file descriptor. EBX=fd |
| 6 | SYS_MALLOC | Allocate memory (Identity-mapped heap) |
| 7 | SYS_FREE | Free allocated memory |
| 8 | SYS_GET_TICKS | Get PIT timer tick count |
| 9 | SYS_YIELD | Yield CPU (sti;hlt;cli for true CPU surrender) |
| 10 | SYS_EXIT | Terminate current task |

### GUI Syscalls (11–17)
| # | Name | Description |
|---|------|-------------|
| 11 | SYS_DRAW_RECT | Draw rectangle. EBX=win_id, ECX=x, EDX=y, ESI=(w<<16)\|h, EDI=color |
| 12 | SYS_DRAW_TEXT | Draw text. EBX=win_id, ECX=x, EDX=y, ESI=str_ptr, EDI=color |
| 13 | SYS_GET_KEY | Get keyboard char (non-blocking) → char or 0 |
| 14 | SYS_GET_MOUSE | Get mouse state → EAX=x, EBX=y, ECX=buttons |
| 15 | SYS_CREATE_WINDOW | Create window. EBX=x, ECX=y, EDX=w, ESI=h, EDI=title → win_id |
| 16 | SYS_GET_EVENT | Get window event. EBX=win_id, ECX=event_ptr (auto-kills zombie tasks) |
| 17 | SYS_UPDATE_WINDOW | Commit draw commands. EBX=win_id |

### Thread & Process Management (18–22)
| # | Name | Description |
|---|------|-------------|
| 18 | SYS_THREAD_CREATE | Create thread. EBX=entry, ECX=priority, EDX=page_dir → TID |
| 19 | SYS_SLEEP | Sleep current task. EBX=ticks |
| 20 | SYS_GET_PID | Get current task ID |
| 21 | SYS_SET_PRIORITY | Set task priority. EBX=tid, ECX=priority |
| 22 | SYS_GET_PRIORITY | Get task priority. EBX=tid → priority |

### IPC — Inter-Process Communication (23–28)
| # | Name | Description |
|---|------|-------------|
| 23 | SYS_IPC_CREATE | Create message queue. EBX=key → queue ID |
| 24 | SYS_IPC_SEND | Send message (blocking). EBX=qid, ECX=type, EDX=data, ESI=len |
| 25 | SYS_IPC_RECV | Receive message (blocking). EBX=qid, ECX=type_out, EDX=data_out, ESI=len_out |
| 26 | SYS_IPC_DESTROY | Destroy queue. EBX=qid |
| 27 | SYS_IPC_TRY_SEND | Non-blocking send. Returns 0/-1 |
| 28 | SYS_IPC_TRY_RECV | Non-blocking receive. Returns 0/-1 |

### Virtual Memory (29–31)
| # | Name | Description |
|---|------|-------------|
| 29 | SYS_VMM_MAP | Map page. EBX=vaddr, ECX=paddr, EDX=flags |
| 30 | SYS_VMM_ALLOC | Allocate virtual page. EBX=vaddr, ECX=flags → vaddr or 0 |
| 31 | SYS_VMM_FREE | Free virtual page. EBX=vaddr |

### UNIX Compatibility (32)
| # | Name | Description |
|---|------|-------------|
| 32 | SYS_PIPE | Create pipe pair. EBX=pipefd[2] → return 0/-1 |

---

## Applications

| Application | Type | Description |
|---|---|---|
| Terminal | Ring 0 | Full terminal emulator with command history, smart tab-completion, Ctrl+C, prompt protection |
| Nano Editor | Ring 0 | Windowed text editor for VFS files |
| File Explorer | Ring 0 | Browse and open stored files |
| System Info | Ring 0 | Live RAM, CPU, resolution, uptime, and MAC address |
| PCI Manager | Ring 0 | Scrollable table of detected PCI hardware |
| Clock | Ring 0 | Digital clock with WIB timezone |
| Snake | Ring 0 | Modern grid-based snake game in WM window with gradient body, eyes, score, speed scaling |
| Power Options | Ring 0 | Shut Down, Restart, and Log Out dialog with accurate button hit-zones |
| Mini Browser | Ring 0 | Text-mode web browser via serial modem proxy |
| Hello Ring 3 | Ring 3 (.mct) | Demo user-space app with isolated memory and GUI window |
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

### Building User Applications (.mct)
User mode applications are written in C, compiled with `gcc -m32`, and processed into the `.mct` format:
1. **Compile**: `gcc -m32 -march=i386 -fno-pie -ffreestanding -c app.c -o app.o`
2. **Link**: `ld -m elf_i386 -T apps/app.ld app.o -o app.elf`
3. **Format**: `python3 build_mct.py app.elf app.mct`
4. **Deploy**: The `Makefile` automatically handles this and uses `inject_vfs.py` to bake the `.mct` binaries into `disk.img`.

---

---

## Version History

| Version | Highlights |
|---|---|
| v23.0 | **Performance & Stability:** Shadow Framebuffer (delta-only MMIO), VSync removal, zombie process detection + auto-kill, `task_kill()` API, Ctrl+C signal, Ctrl key tracking, Snake rewritten as WM app, terminal prompt protection, smart tab-completion with trailing space/slash, carriage return support, history display fix, power menu restart fix, `-no-reboot` removal. |
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

# Mectov OS - Sistem Operasi 32-bit Pertama di Indonesia