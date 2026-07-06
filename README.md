# Mectov OS v31.0 — The Graphics Pipeline & Compositing Performance Update

The Mectov Kernel — an operating system kernel written from scratch in C and Assembly. No external libraries, no libc, no POSIX — every byte runs directly on hardware.

## About

Mectov OS is a hobby operating system designed as a learning project and technical showcase. It boots via GRUB Multiboot, sets up protected mode with paging, and provides a fully graphical desktop environment with floating windows, custom static wallpapers, persistent draggable icons, hardware detection, standalone Ring 3 user applications, and real internet connectivity.

The v31.0 release delivers significant graphics compositing optimizations, an event-driven redraw architecture, 64-bit comparison pixel blitting, system-wide Alt+Tab window switching, and Escape key driver mappings:
1. **Event-Driven Compositor:** Replaced the forced 60Hz polling compositor with an event-driven compositor that only redraws on actual hardware interrupts or user space requests. This drops idle CPU load to 0% in virtual machines.
2. **64-Bit Pixel Comparison:** Casts window buffer scans to 64-bit (`uint64_t*`) in `swap_buffers()`, comparing 2 pixels per clock cycle and instantly skipping identical areas to drastically reduce RAM copy overhead.
3. **Redraw Syscall Trigger:** Configured `SYS_UPDATE_WINDOW` (17) syscall to set `needs_redraw = 1` globally, ensuring user-space window updates are instantly recomposed.
4. **Alt+Tab Window Switcher (v30.4):** Integrated Alt modifier tracking and Tab interception with a Catppuccin Alt+Tab HUD card overlay displaying active app icons, titles, and a selection highlight.
5. **Escape Key Mapping Fix (v30.1):** Fixed keyboard driver's `scancode_to_char` to map the Escape key (scancode `0x01`) to its proper ASCII value `27` (`0x1B`), fixing dialog dismiss behavior.
6. **Full English Localization (v30.1):** Localized all Indonesian menu labels, status indicators, and shell command centers to English.

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
- **Application Double-Click Launcher:** Desktop icons are fully interactive and support double-clicking to launch their respective `.mct` executable.

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
- **Per-task dual stacks:** 16KB kernel stack + **upgraded 64KB user stack** to fully isolate memory under intensive network socket, file parsing, and DNS payload operations.
- IRQ0-driven scheduler tick (1000Hz) with cooperative yield.

### 10. Network Stack (src/drivers/net.c + src/drivers/rtl8139.c)
- RTL8139 NIC Driver: Full driver with PCI bus mastering.
- **Ethernet/ARP/IPv4/ICMP/UDP/DNS:** Complete local stack built directly into the kernel space.
- **Reliable DNS Resolution:** Upgraded DNS queries to point to QEMU's virtual gateway DNS server at `10.0.2.3` (switching from hardcoded `8.8.8.8`) for robust internet routing.
- **Background Net Poller:** Embedded packet listening into the timer-based process scheduler to handle asynchronous inbound packets gracefully.
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
- **Launch Arguments:** The kernel can pass arguments (like filenames) to newly launched Ring 3 tasks, retrievable via `SYS_GET_LAUNCH_ARG`.
- **Independent Stacks:** Each Ring 3 task maintains separate 16KB kernel and **64KB user stacks** to avoid stack pointer overflows during large buffer parses.

### 14. User-Mode GUI API
- **Direct Window Management:** Ring 3 applications can now create, raise, and close their own GUI windows via syscalls.
- **Graphics Primitive Syscalls:** Accelerated `SYS_DRAW_RECT` and `SYS_DRAW_TEXT` for rendering directly into the application's window buffer.
- **Event Polling & Persistence:** `SYS_GET_EVENT` allows user apps to respond to window-specific mouse and keyboard input. It implements a two-phase close signal, giving apps a chance to safely save data to the VFS before terminating.
- **Flicker-Free Updates:** `SYS_UPDATE_WINDOW` ensures changes are committed to the display list and rendered during the next 60Hz frame sync.

### 15. Security & Pointer Validation (src/sys/syscall.c)
- **Safe Syscall Entry:** Every pointer passed from Ring 3 is validated via `validate_user_ptr` to prevent kernel memory corruption or unauthorized data access.
- **Address Boundary Checks:** Enforces strict memory boundaries (`USER_MEM_LIMIT`) for all syscall parameters.
- **Zombie Cleanup:** The kernel automatically detects and terminates Ring 3 processes whose GUI windows have been closed (if they refuse to exit voluntarily), preventing orphaned tasks and resource leaks.
- **Privilege Separation:** Use of Global Descriptor Table (GDT) and Task State Segment (TSS) to strictly enforce CPU privilege levels (Ring 0 vs Ring 3).

### 16. PS/2 IntelliMouse Scroll Wheel Support (src/drivers/mouse.c & src/gui/wm.c)
- **Driver Upgrade:** Upgraded PS/2 mouse driver to the 4-byte IntelliMouse protocol, using a custom rate-negotiation sequence (200 -> 100 -> 80) to detect scroll-capable hardware.
- **Kernel Event Routing:** The main kernel loop catches scroll deltas and dispatches them via `wm_handle_scroll()` to targeted windows, encoding up/down ticks as custom button events (0x10 and 0x20).
- **Ring 3 Event Propagation:** Emits standard event type 4 (Scroll Event) containing scroll direction delta (+1 for up, -1 for down) to Ring 3 apps via the `SYS_GET_EVENT` syscall.
- **Full Application Integration:** Native vertical scrolling integrated seamlessly across standard user-space applications:
  - **Mini Browser:** Scrolls active web page contents effortlessly (3 lines per tick).
  - **File Explorer:** Easy navigation through folder list structures.
  - **PCI Manager:** Smooth scrolling through the system hardware list.
  - **Volume Manager:** Intuitive scroll-to-adjust volume control (adjusts volume by ±5 per notch).

### 17. Sound Blaster 16 (SB16) Audio System (src/drivers/sb16.c)
- **Sound Blaster Hardware Driver:** Native ISA direct-register programming for Sound Blaster 16 compatibility.
- **Dynamic WAV Playback:** Supports loading and decoding dynamic 8-bit/16-bit mono/stereo `.wav` files via VFS and streaming audio through the virtual DSP.
- **Sound Synthesis:** Support for playing discrete frequencies (beeps) with programmable duration directly through Sound Blaster or PC Speaker hardware.
- **Volume Controller Integration:** Mapped to QEMU's PA sound engine (`-device sb16,audiodev=snd0`) for clear real-time system audio feedback.

### 18. Homegrown Dynamic Shared Library System (apps/lib/libc.c & libc.h)
- **Dynamic Runtime Linking (`libc.mct`):** Built a homegrown dynamic linking and loading subsystem. The dynamic loader (`SYS_LOAD_LIBRARY` / `mct_load_library`) retrieves the memory base of the export table for a library in memory.
- **Extremely Slim Binaries:** Ring 3 application executables (like `browser.mct`, `explorer.mct`, `notepad.mct`) no longer statically compile common functions, shrinking binary file sizes from 30KB+ to under 2KB.
- **Standard Lib Wrappers:** Full resolution of standard library functions at runtime via export table pointer indexes (`__mct_lib_ptr`):
  - **String Handling:** `strlen`, `strcpy`, `strcat`, `atoi`, `itoa`, `itoa_pad`
  - **Formatting & Output:** `printf`, `sprintf`
  - **POSIX Wrappers:** `open`, `read`, `write`, `close`, `malloc`, `free`, `exit`, `sleep`

### 19. Multi-Drive & Secondary ext2 Partition Support (src/sys/vfs.c & run.sh)
- **ATA Dual Drive Support:** Kernel ATA driver upgraded to detect and mount secondary IDE/ATA devices.
- **Secondary ext2 Disk:** Automatically creates and mounts a secondary 2MB virtual disk (`ext2.img`) initialized via `mkfs.ext2` at index 1 in QEMU.
- **Web Gateway Proxy Integration (`gateway.py`):** Background gateway process running on the host that translates QEMU network queries and streams live data between the guest OS and the real internet.
- **TCP Real-time Debugging Socket:** Serial port debugging upgraded to a TCP socket server on port `45454` (replacing files-only logging), allowing real-time, zero-lag log streaming into our python interactive debugger (`debug.py`).

### 20. User-Space Terminal Emulator Enhancements (terminal.c)
- **Interactive Shell Aliases & Shortcuts:** Allows local registration of shortcuts (e.g. `alias ll="ls -la"`, `alias ..="cd .."`) including wrapper aliases for complex MCT launches (e.g. `alias browser="jalankan apps/browser.mct"`). Type `alias` to print all active shortcuts.
- **Modern Zsh-style Auto-suggestions:** Searches command history backwards in real-time as the user types and renders suggestions inline in dim gray (`0x08`). Pressing Right Arrow or End at the end of the line instantly accepts the auto-suggested text.
- **Advanced Inline Editing & Cursor Navigation:** Fully supports non-ASCII key events for inline cursor movement via Left Arrow (`0xE04B`) and Right Arrow (`0xE04D`), instant jumping via Home (`0xE047`) and End (`0xE04F`), mid-string character insertion, and precise backspacing.
- **Terminal Scrollback & Mouse Wheel:** Expanded internal buffer from 24 rows to 128 rows (`SCROLLBACK_ROWS`). Incorporates mouse scroll wheel polling (Event Type 4) and Page Up/Page Down keyboard keys to scroll the terminal view smoothly, complete with a custom visual scrollbar indicator.
- **Arrow-Key Command History & VFS Tab Completion:** Tracks up to 16 commands in a circular buffer and dynamically queries the VFS to autocomplete both built-in shell commands and active directory files/folders.

### 21. Self-Hosted Gitea Integration (Git Hosting Migration)
- **Home Server Remote Tracking:** Added a dedicated `gitea` Git remote tracking the repository at `https://git.mectov.my.id/maliffadlan/Mectov-OS.git`.
- **Infrastructure Autonomy:** Seamlessly routes all future feature branch pushes, commit tracking, and code releases directly onto the self-hosted Gitea home server, operating as the primary remote alongside GitHub.

### 22. Copy-on-Write (COW) Virtual Paging (src/sys/vmm.c & src/sys/idt.c)
- **Reference Counting Allocator:** Added physical frame reference counting (`frame_ref_count`) to track shared pages across page directories.
- **Fork-Style COW Address Spaces:** Modified `vmm_clone_address_space()` to clone user space mappings without eager memory copy. Pages are marked read-only and flagged with `PAGE_COW` (bit 9).
- **On-Demand Page Duplication:** Page fault handler (Interrupt 14) intercepts writes to `PAGE_COW` pages, copying data dynamically when `ref_count > 1`, or directly promoting the page to writable if it's the sole remaining owner.

### 23. Environment Variables & Advanced Command Interpreter (src/sys/shell.c)
- **Active Shell Context:** Supports variable exports (e.g. `export USER=bos_alif`) and dynamic `$VAR` string interpolation for all script and terminal inputs.
- **Interactive Aliasing:** Built-in shell commands (`alias`, `unalias`, `history`) to manage custom command shortcuts, circular command history, and inline expansions.
- **Robust Path Sanitization:** Built-in `sanitize_path` to strip quotes, clean whitespace, and ensure resilient navigation (`cd`) and text display (`cat`, `baca`) even with spaces in pathnames (e.g. `"notepad tes"`).

### 24. Process Control & Task Diagnostics (src/sys/task.c & src/sys/shell.c)
- **Task Identification:** Scheduler tracks process names dynamically via `task_set_launch_arg()` and `task_get_launch_arg()`, naming system services and desktop binaries accordingly.
- **CLI Process Managers:** `ps` command prints a beautiful colored table of PID, Ring, priority, status, and name; `kill [PID]` safely terminates user processes with PID range guard validation.
- **Real-time Diagnostics:** Added `uptime` command (reads PIT ticks and renders human-readable hours/minutes/seconds) and `memstat` command (queries physical RAM allocations and outputs dynamic heap allocator `kmalloc_stats` fragmentation data).

### 25. Per-Task Working Directories (src/sys/task.c & src/sys/vfs.c)
- **Thread-Local Directories:** Replaced the global `current_dir` VFS variable with a task-specific property (`task_t.current_dir`). Task directories are fully isolated; changing directories in one terminal does not disrupt other processes or desktop widgets.
- **Boot Alignment:** Suppressed VFS startup active directory restoration. The kernel always boots at `/` to align perfectly with the GUI Terminal initialization.
- **Nano Absolute Path Resolution:** Solved relative-path saving context bugs by immediately resolving files (e.g. `nano note.txt` in `/home`) to absolute paths in `st_ed()` before Window Manager callbacks process saving events.

### 26. VFS Integrity & Parallel Build System
- **VFS Sector Growth Guard:** Rebuilt the ATA VFS sector allocator to prevent contiguous file overwrite bugs. Files that grow past their original sector limits are dynamically moved to the end of the disk (`next_data_sector`), ensuring robust data safety.
- **Build Isolation:** Links independent `.ld` script targets named after target binaries to eliminate parallel linker race conditions during concurrent builds (`make -j`).

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
| 20 | SYS_GET_PID | Get current task ID / TID |
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

### UNIX Compatibility & Hardware Interface (32–38)
| # | Name | Description |
|---|------|-------------|
| 32 | SYS_PIPE | Create pipe pair. EBX=pipefd[2] → return 0/-1 |
| 33 | SYS_GET_TIME | Get RTC clock time. EBX=rtc_time_t* ptr |
| 34 | SYS_PLAY_SOUND | Play PIT / Sound Blaster sound frequency. EBX=freq, ECX=duration_ms |
| 35 | SYS_GET_SYSINFO | Get hardware and kernel statistics. EBX=sysinfo_t* ptr |
| 36 | SYS_GET_PCI_INFO | Get list of detected PCI devices. EBX=pci_device_t* array, ECX=max |
| 37 | SYS_LIST_DIR | List VFS directory contents. EBX=dir_entry_t* array, ECX=max, EDX=parent_node |
| 38 | SYS_STAT_FILE | Get file attributes and node index. EBX=path_ptr |

### Network Stack (39–43)
| # | Name | Description |
|---|------|-------------|
| 39 | SYS_DNS_RESOLVE | Asynchronously resolve domain to IP. EBX=domain_ptr |
| 40 | SYS_TCP_CONNECT | Open TCP socket connection. EBX=ip_ptr, ECX=port |
| 41 | SYS_TCP_SEND | Send raw TCP packet payload. EBX=data_ptr, ECX=len |
| 42 | SYS_TCP_RECV | Read TCP socket stream. EBX=buf_ptr, ECX=max_len → bytes read / state |
| 43 | SYS_NET_STATUS | Get packed network state (DNS resolved, TCP state) |

### Terminal & Extended Execution (44–51)
| # | Name | Description |
|---|------|-------------|
| 44 | SYS_SET_STDOUT_IPC | Set process stdout redirection queue. EBX=qid (0 to disable) |
| 45 | SYS_EXEC_CMD | Run shell command program. EBX=cmd_string_ptr |
| 46 | SYS_GET_TASKS | Get list of running tasks. EBX=sys_task_info_t* array, ECX=max |
| 47 | SYS_GET_WINDOWS | Get list of open windows. EBX=sys_win_info_t* array, ECX=max |
| 48 | SYS_KILL_TASK | Force kill a task. EBX=tid |
| 49 | SYS_GET_LAUNCH_ARG| Get launch argument string. EBX=buf, ECX=max_len |
| 50 | SYS_CREATE_FILE | Directly create an empty file in VFS. EBX=filename |
| 51 | SYS_LOAD_LIBRARY | Dynamically load a shared library base. EBX=lib_name_ptr → base address |

---

## Applications

| Application | Type | Description |
|---|---|---|
| Terminal | Ring 3 (.mct) | Full terminal emulator with 16-command circular history (Up/Down arrows) and VFS tab autocomplete |
| Nano Editor | Ring 3 (.mct) | Windowed text editor for VFS files with stable auto-save |
| Notepad | Ring 3 (.mct) | Sleek text editor with menu bar options, Save As dialog, and dirty-state tracking |
| File Explorer | Ring 3 (.mct) | Browse and open stored files |
| System Info | Ring 3 (.mct) | Live RAM, CPU, resolution, uptime, and MAC address |
| Task Manager | Ring 3 (.mct) | Monitor CPU, RAM, and kill active user processes |
| PCI Manager | Ring 3 (.mct) | Scrollable table of detected PCI hardware with scroll wheel support |
| Music Player | Ring 3 (.mct) | Graphical audio player to stream and play mono/stereo dynamic `.wav` files via SB16 |
| Volume Control | Ring 3 (.mct) | Slider utility to adjust system sound level, now supporting wheel scrolling |
| Clock | Ring 3 (.mct) | Digital clock with WIB timezone |
| Snake | Ring 3 (.mct) | Modern grid-based snake game in WM window with gradient body, score, speed scaling |
| Mini Browser | Ring 3 (.mct) | Text-mode web browser navigating via host proxy gateway with scroll support |
| Hello Ring 3 | Ring 3 (.mct) | Demo user-space app with isolated memory and GUI window |
| GUI Calculator | Ring 3 (.mct) | Standalone external GUI calculator |
| Power Options | Ring 0 | Shut Down, Restart, and Log Out dialog with accurate button hit-zones |
| DOOM Engine | Ring 0 (Port) | Full integration of the legendary 1993 DOOM engine with graceful exit to desktop |

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
| v31.0 | **Graphics Pipeline & Compositing Performance Update:** Shifted the main rendering loop in `kernel.c` to an event-driven model, removing forced 60Hz polling to lower idle CPU load to 0%. Optimized VRAM compositing in `swap_buffers` (`src/drivers/vga.c`) by comparing 2 pixels per iteration using 64-bit casting (`uint64_t*`), skipping static content instantly. Configured `SYS_UPDATE_WINDOW` syscall to trigger compositor redraws via global `needs_redraw`. |
| v30.2 | **Alt+Tab Window Switcher & English Localization Update:** Added Left Alt modifier key press/release state tracking in the keyboard driver and intercepted Tab scancodes in the main loop to cycle focus between active windows via `wm_focus_next()`. Fixed Escape key (scancode 0x01) ASCII translation mapping. Translated all remaining Indonesian strings across menus, shell feedback, dialog boxes, and toolbar layouts to English. |
| v30.0 | **Clipboard, Explorer CRUD & Context Menu Update:** Implemented global kernel clipboard manager (`src/sys/clipboard.c`) and user stubs for app copy-paste capability. Added "+File", "+Folder", and "Hapus" toolbar buttons with name input modals in File Explorer. Added kernel syscalls `SYS_DELETE_FILE` (58), `SYS_MKDIR` (59), and `SYS_RENAME_FILE` (60) to support Ring 3 CRUD actions. Implemented custom Catppuccin right-click context menus on Desktop and File Explorer for direct app launches, deletion, and renaming. |
| v29.0 | **File Association & Explorer Double-Click Update:** Upgraded File Explorer (`explorer.c`) to support double-click (second-click on selected item) file associations. Double-clicking `.mct` runs the binary, `.wav` plays in Media Player (`/apps/mplayer.mct`), and other text files edit in Notepad (`/apps/notepad.mct`). Upgraded kernel command executor (`src/sys/shell.c`) to parse arguments for the `jalankan` command, splitting program path from arguments and launching via `load_mct_app_with_arg()`. |
| v28.0 | **DOOM Memory Protection, HUD Font & Crash Recovery Update:** Relocated kernel heap base from 16MB to 24MB (`src/sys/mem.c`) to completely resolve memory overlap/collision with the expanded kernel BSS section caused by DOOM's embedded static variables. Implemented integer precision formatting (`%.3d`) in `doom_vsnprintf` (`doom/doom_libc.c`) to fix HUD and quit-confirm pop-up font loading. Implemented automatic fullscreen flag reset (`doom_fullscreen = 0`) on task exit inside `wm_cleanup_task` (`src/gui/wm.c`) to prevent system-wide freezes on game exit/crash. Fixed overlapping desktop icons by aligning `ICON_COUNT` to 11 (`src/include/desktop.h`). |
| v27.8 | **Nano Path Resolution Update:** Fixed relative path saving context bugs inside the kernel Nano editor (`src/apps/nano.c`) by resolving files to absolute paths via `vfs_resolve_path()` on startup. Upgraded the `ed_fn` buffer size from `MAX_FILENAME` (32) to `MAX_PATH` (256) in `nano.c` and `apps.h` to fully support deep path strings. |
| v27.7 | **Per-Task Working Directory Update:** Refactored the VFS `current_dir` from a global system variable to a task-specific thread-local attribute (`task_t.current_dir`). This isolates working directories between terminals and GUI applications. Disabled active directory persistent restoration on boot to prevent prompt synchronization desync on startup. |
| v27.5 | **Absolute Path Launcher Update:** Fixed a bug where changing the active directory in the Terminal (e.g. `cd home`) caused Desktop application icons and aliases to fail to open by rewriting all launchers and stubs to load apps using absolute paths (e.g., `/apps/gcalc.mct` instead of relative paths). |
| v27.4 | **System Diagnostics Update:** Implemented native `uptime` and `memstat` commands in both the kernel shell and user-space Terminal. `uptime` displays human-readable runtime duration and total timer ticks, while `memstat` renders a complete breakdown of physical RAM allocation alongside heap allocator (`kmalloc`) metrics. |
| v27.3 | **VFS Path Sanitization Update:** Implemented automatic quote-stripping and trailing-space trimming in all shell command path arguments, resolving file read and navigation failures for files with spaces (e.g., `"notepad tes"`). |
| v27.2 | **Notepad GUI & Shortcut Update:** Implemented Ctrl+S, Ctrl+N, and Ctrl+Q keyboard shortcuts for Notepad GUI in user mode. Redesigned the Save As interface into a centered modal dialog box. Fixed a bug where Notepad loaded its own binary (`notepad.mct`) on startup. |
| v27.1 | **Memory Overlap & Stability Fix:** Fixed page directory/table corruption by adjusting `KERNEL_RESERVED_PAGES` to 64MB and capping `max_heap` at 32MB, ensuring 100% physical separation between heap and VMM frame pool. |
| v27.0 | **TCP Socket Redirection & Web Gateway Update:** Implemented transparent HTTP port 80 redirection inside the kernel TCP stack (`net_tcp_connect`) routing to the host gateway at `10.0.2.2:8888`. Enabled clean modern web browsing inside the Ring 3 Browser app (`apps/browser.mct`) using a Python gateway proxy (`gateway.py`) to parse real HTTPS web pages into memory-safe text. |
| v26.0 | **Copy-on-Write (COW) Paging & Integrated Editor Update:** Added virtual page Reference Counting (`frame_ref_count`) and fully implemented Copy-on-Write (COW) address space cloning for Ring 3 process isolation. Fully integrated built-in GUI editor to `edit`, `tulis`, and new `nano` shell commands. Added sleek text editor status footer showing count and controls. Fixed persistent disk storage by removing the destructive `disk.img` deletion in `run.sh`. |
| v25.1 | **Advanced Shell Scripting, Environment Variables & Aliasing Update:** Implemented automated script file execution (`sh`/`source` commands) with inline comment (`#`) parsing, whitespace trimming, and empty line skipping. Implemented Shell Environment Variables (`export` and `$VAR` expansion) in both terminal commands and scripts. Added command aliasing (`alias`, `unalias`, and a new `history` command) in the shell. Fixed `nano` editor to automatically create files if they don't already exist on save. Implemented kernel `memmove`, achieving a 100% warning-free build. |
| v25.0 | **IntelliMouse, Audio, Shell & Git Enhancements:** Upgraded mouse driver to 4-byte IntelliMouse protocol supporting smooth scrolling in Browser, Explorer, PCI, and Volume Manager. Upgraded kernel audio to Sound Blaster 16 supporting dynamic WAV music stream playback. Built a dynamic shared library system (`libc.mct`) with a dynamic loading subsystem, reducing app binary sizes to ~1KB. Increased user stacks to 64KB and updated DNS to route over virtual gateway. **Enhanced GUI Terminal** with 16-command history buffer (Up/Down arrow navigation) and dynamic client-side VFS Tab completion. **Gitea Migration:** Added self-hosted home server remote `gitea` for private repository tracking. |
| v24.0 | **DOOM Engine Port:** Fully playable port of the classic 1993 DOOM engine integrated directly into the kernel. Features keyboard polling, double buffer to MMIO front buffer translation, graceful OS exiting (`vga_force_sync`), and proper process teardown. |
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

Salah satu proyek OS hobi buatan Indonesia yang paling lengkap dan terdokumentasi dengan baik di GitHub saat ini