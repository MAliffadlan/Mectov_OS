# Mectov OS v35.5 — Multi-Connection TCP Update

The Mectov Kernel — an operating system kernel written from scratch in C and Assembly. No external libraries, no libc, no POSIX — every byte runs directly on hardware.

## About

Mectov OS is a hobby operating system designed as a learning project and technical showcase. It boots via GRUB Multiboot, sets up protected mode with paging, and provides a fully graphical desktop environment with floating windows, custom static wallpapers, persistent draggable icons, hardware detection, standalone Ring 3 user applications, and real internet connectivity.

The v35.5 release upgrades the network stack from a single hard-coded TCP socket to a real **multi-connection TCP layer**: eight independent connection slots, per-connection sequence numbers / receive buffers / retransmit state, and a proper FIN handshake — all exposed to Ring 3 via conn-id syscalls. A new `fetch [domain]` shell command and the Ring 3 browser both use it end-to-end. Along the way, testing surfaced and fixed a **latent SMP race in the RTL8139 driver** that could silently drop a TCP frame:

1. **Multi-Connection TCP (8 slots):** The kernel now tracks up to 8 simultaneous connections (`tcp_conn_t`), each with its own local/remote ports, ISN-derived sequence numbers, `ack`, a 16KB receive buffer, and retransmit state. `net_handle_tcp()` matches inbound segments by the full 4-tuple and drives a real state machine: SYN_SENT → ESTABLISHED → FIN_WAIT_1/2, CLOSE_WAIT, LAST_ACK → CLOSED. Duplicate/out-of-order segments get a re-ACK instead of polluting the buffer; corrupted checksums are dropped; RST kills the slot immediately.
2. **Retransmit & Timeouts:** `net_poll()` sweeps all connections each tick — unacked data is retransmitted (up to 5 tries, 6s apart) and a connect that stalls in SYN_SENT is aborted after 10s. The retransmit base is stamped only when new data (or SYN/FIN) is sent, so pure ACKs can't keep postponing a genuine retransmit.
3. **TCP API with Connection IDs:** `net_tcp_connect/send/recv/close/state/latest_state` plus syscalls `SYS_TCP_CONNECT` (40), `SYS_TCP_SEND` (41), `SYS_TCP_RECV` (42), `SYS_NET_STATUS` (43) — send/recv now take the conn id in `EDX` — and the new `SYS_TCP_CLOSE` (70) for graceful FIN shutdown. `sys_tcp_send/recv/close` wrappers expose the id-based API to Ring 3 apps.
4. **Browser Upgrade:** The Ring 3 mini-browser tracks its active conn id, closes it on abort/ESC/EOF, and distinguishes a peer FIN (page complete) from a reset (`Connection lost`).
5. **`fetch [domain]` Shell Command:** Resolves the gateway via ARP, the domain via DNS, connects to port 80 (redirected through the host Web Gateway Proxy), sends `GET /`, and streams the response until EOF — with bounded waits safe under the interrupt-gate syscall path.
6. **RTL8139 SMP TX/RX Race Fix:** The NIC's TX descriptors and RX ring were shared state with no lock. With SMP active, the IRQ handler on one CPU and `net_poll()` on another could both read `rtl_tx_cur`, copy into the *same* TX buffer, and fire the same descriptor — the second SYN silently never left the NIC and the handshake timed out. A spinlock (`rtl_enter`/`rtl_exit`, save/restore IF so it is safe from IRQ context) now serializes every TX send and RX poll. Verified in QEMU: two parallel connections both complete the handshake, fetch the page through the gateway, and close cleanly.

Previous release highlights (v35.4 security hardening, v35.3 English localization, v35.2 shell file management + ext2 fixes, v35.1 ext2 write support, v35.0 SMP/GDB/ELF/sync) remain below in the Version History.

1. **ELF Loader Integer Wrap Fix:** The segment bounds check compared `p_offset + p_filesz` in 32-bit math; a crafted `p_offset` could wrap past the check and the follow-up `memcpy` read unmapped memory at CPL 0, panicking the kernel. The check now uses 64-bit math, and both loaders (`.mct` + ELF) abort cleanly instead of faulting when page allocation fails.
2. **Unbounded Shell Script Recursion Fix:** A script containing `sh a.sh` (or two scripts sourcing each other) recursed until the kernel stack overflowed — `is_script` was set but never read. Nested script execution is now refused.
3. **VFS Node Table Sanitization:** `vfs_load()` blindly trusted the on-disk 64-node table. A missing NUL byte in a 32-byte name made `strtolower()`/`strlen()` walk off the node buffer, and an out-of-range `parent` indexed `fs_nodes[]` out of bounds (or looped forever). Names are now force-terminated and parent links validated at load.
4. **Ext2 Superblock Hardening:** `s_inode_size` was only checked against a 128-byte minimum — a forged value that does not divide `block_size` let inode records span block boundaries, running `memcpy` past the fixed 4KB stack buffers. Mount now rejects non-divisor inode sizes; `ext2_read/write_block` also bound every block number against the filesystem's block count and the 4096-sector drive before the `block * sectors_per_block` multiplication can wrap; the unlink name comparison clamps `name_len` to the directory record; and a failed inode read no longer leaves a garbage `i_size` in the VFS tree.
5. **File Descriptor Lifecycle Fix:** `task_cleanup()` never released a task's 16 file descriptors on exit/kill — global fd slots and pipes leaked, and a pipe reader whose writer died blocked forever on a pipe that was never closed. All fds are now closed during teardown (`task_close_all_fds()`), and `vfs_delete_node()` refuses to delete a file that open fds still reference (the freed slot would be reused by the next create, silently redirecting reads/writes to an unrelated file).
6. **Precise `task_sleep()`:** The old 100k-`pause` busy-wait was a CPU burn and imprecise — it could return while the task was still marked SLEEP, after which the scheduler froze it at an arbitrary instruction. Tasks now park on `hlt` until the scheduler flips them back to READY.
7. **GUI Hardening:** Window resize re-clamps the position after forcing the 220×150 minimum, so the titlebar can no longer be pushed off-screen (uncloseable window); `wm_open()` clamps absurd x/y from Ring 3 (a fully off-screen window was unreachable by mouse — a zombie-window DoS); the deferred window-buffer free list grew from 16 to 64 with a drop warning; and the mouse x/y/button state is snapshotted with interrupts off so the hit-test can't see a torn coordinate pair.
8. **Shell Input Overflows:** Tab completion now clamps into `cmd_b[256]` (a long prefix + long match previously wrote past it into the adjacent `hist_b`/`env_vars`/`aliases` globals); the calendar popup clamps a dead-CMOS month-0 value before indexing `months[]` (was `months[-1]` — a wild-pointer dereference); and `vfs_read_file()` clamps reads to the 2048-sector disk.

The v35.3 release finishes translating the user interface to English. Every remaining Indonesian string is now English-first: the shell's command names (`snake`, `tone`, `sleep`, `date`, `color`, `lock`, `run`), its help screen, the calculator, and the editor footer. All legacy Indonesian names still work as built-in aliases (`buat`→`touch`, `hapus`→`rm`, `ular`→`snake`, `matikan`→`shutdown`, ...). The recent changes are:

1. **SMP Fix — MADT/XSDT Parsing:** Fixed the ACPI table walker that silently skipped every table (a bad bounds check compared table addresses against the RSDT region), so the MADT was never found and SMP fell back to a single core. The kernel now parses RSDT *and* XSDT, finds the MADT, boots all APs, and runs multitasking across 4 real CPU cores.
2. **In-Kernel GDB Stub:** A GDB Remote Serial Protocol stub on COM2 lets you attach a real `gdb` to the running kernel — F12 breaks in, registers/memory reads, software breakpoints (Z0/z0), and single-stepping all work, with DWARF symbols (`-g`) resolving `kernel_main` and friends.
3. **Standard ELF32 Loader:** The proprietary `.mct` format is now joined by a full ELF32 ET_EXEC loader that maps PT_LOAD segments at their `p_vaddr`, zero-fills BSS, and jumps to `e_entry`. `apps/elfdemo.elf` proves real ELF binaries run in Ring 3.
4. **Semaphores & Futexes:** Kernel semaphores (id-based, FIFO wait queues) and address-space-keyed futexes give Ring 3 apps proper blocking synchronization — a task parks in `TASK_STATE_BLOCKED` and burns zero CPU until woken.
5. **IRQ-Driven Networking + UDP API:** The RTL8139 now fires IRQ 11 (routed through the I/O APIC) instead of relying on polling; `net_irq_handler()` drains the RX ring in interrupt context. New `SYS_UDP_BIND` / `SYS_UDP_SEND` / `SYS_UDP_RECV` expose datagram sockets to user space.
6. **CI Boot Test:** A GitHub Actions workflow builds the kernel, boots it in QEMU (no KVM), logs in, and verifies `BOOTED KERNEL LOOP` — every commit is proven to boot.
7. **Self-Contained Build:** The wallpaper source is now bundled (`assets/wallpaper.png`) instead of a hardcoded absolute path, so `make` works on any machine.
8. **Ext2 Write Support:** The secondary ext2 partition is now fully writable — block/inode bitmaps, directory entry creation with slack-splitting, file grow/truncate (direct + singly-indirect blocks), rename and delete. Every VFS create/write/delete under `/ext2` routes to the real filesystem and survives reboot (verified with `fsck.ext2` and `debugfs` on the host, plus a two-boot persistence test on both 1024- and 4096-byte block images).
9. **Shell File Management Commands:** `cp [src] [dst]` copies any file (VFS or `/ext2`) via a heap buffer with a 4MB safety cap; `mv [src] [dst]` renames/moves through `vfs_rename` with friendly errors for ext2 cross-directory moves; `rmdir [path]` removes only empty directories (refuses non-empty, unlike recursive `rm`); `df` reports `mectovfs` (drive 0) and `ext2` (drive 1) capacity, free space and inode usage straight from the superblock counters.
10. **Ext2 Unlink/Delete Consistency Fixes:** Two latent on-disk bugs surfaced while testing delete/rename and were fixed — (a) `ext2_unlink_entry` used to absorb a removed last entry into its predecessor with `prev->rec_len += rec`, which left the block tail unaccounted when deleted-entry holes sat between them, producing a directory fsck sees as "directory corrupted" at the block end; it now sets `prev->rec_len = block_size - prev_off` so the chain always closes exactly at the block boundary. (b) `ext2_rm_inode` now zeroes the freed inode slot, so fsck no longer reports stale `i_blocks`/"zero-length directory" on orphaned inodes. Verified: full fsck.ext2 clean across boot1/boot2 and both 1024-/4096-byte block geometries.

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
|  ACPI (RSDT/XSDT/MADT)  |  SMP (APIC/IOAPIC, 4 cores)             |
+--------------------------------------------------------------------+
|  Priority Thread   |  Semaphores & Futexes |  VFS + ATA PIO        |
+--------------------------------------------------------------------+
|  VGA/VESA Driver   |  Window Manager   |  RTL8139 NIC (IRQ-driven)  |
+--------------------------------------------------------------------+
|  Network Stack (Ethernet/ARP/IPv4/ICMP/UDP/DNS)                    |
+--------------------------------------------------------------------+
|  MCT + ELF32 Loaders |  Ring 3 User Tasks |  GDB Stub (COM2)        |
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
- **Single-Owner Minimize/Restore:** `wm_minimize()` and `wm_restore()` are the only supported ways to change a window's minimized state. They mark the damaged region dirty as part of the operation, because `draw_one()` skips minimized windows — nothing will ever repaint over the pixels a window leaves behind, so a caller that sets `WmWin.minimized` by hand produces a ghost. `wm_focus_next()` is the one deliberate exception: it rebuilds the z-order itself and covers the damage with a fullscreen mark.
- Clean Flat Aesthetic: Removed heavy shadows around windows to focus on a crisp, modern UI.

### 3. Taskbar & System Tray (src/gui/taskbar.c)
- WiFi Status Indicator: Replaced the RAM bar with a classic 3-arc WiFi icon, representing the OS's network capability.
- "Mectov OS" Start button with vertical separator line for distinct UI partitioning.
- Glossy dark background with Catppuccin Mocha accent border.
- Icon-only window buttons: each open app shown as a 16x16 squircle icon matching the desktop style. Clicking a button raises, minimizes, or restores its window through the Window Manager's `wm_minimize()` / `wm_restore()` API rather than reimplementing the state transition locally.
- System tray with:
  - CAPS indicator (vibrant red when active)
  - HDD activity LED (red flash on disk I/O)
  - Digital clock with day of week, adjusted for UTC+7 (WIB) timezone.

### 4. Real-time Rendering (kernel.c + src/drivers/vga.c)
- **Shadow Framebuffer:** Triple-buffer architecture (back_buffer → front_buffer_copy → VGA MMIO). Only pixels that actually changed are written to hardware, eliminating thousands of expensive KVM VM-Exits per frame.
- **VSync Disabled:** Removed VGA port 0x3DA polling which caused massive latency spikes under KVM virtualization.
- Forced 60Hz Loop: Constant 16ms redraw cycle with ultra-low render times (~4ms) thanks to delta-only copying.
- Microsecond Timing: Real-time FPS and render time measurement using PIT hardware counters.
- **Enforced Clip Rectangle:** `vga_set_clip()` / `vga_reset_clip()` define a rectangle in *active render target* coordinates that every primitive respects. `put_pixel()` gates on `clip_test()`, covering lines, circles, rounded-rect borders and all text rendering; `draw_rect()`, `draw_rect_alpha()` and `draw_soft_shadow()` gate on `clip_box()`, covering the filled and gradient primitives built on top of them. `vga_blit_buffer()` is deliberately exempt — it is a compositor operation that writes to `back_buffer` rather than to the render target, so it clips to the screen only.

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
- **Read Terminator Contract:** `vfs_read_file()` returns the byte count and appends a NUL terminator *only when the data left room for one*. It never clamps the payload to `max_size - 1` to make space, because `load_mct_app_with_arg()` passes `max_size` equal to the exact file size and needs every byte of it.

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
- **Shared-Buffer Arbitration:** The 8042 controller exposes one output buffer at port `0x60` for *both* the keyboard and the mouse, and status bit 5 is the only thing that says which device a byte came from. A single `ps2_drain()` (`src/drivers/keyboard.c`) owns the port: it loops while status bit 0 is set, dispatches each byte to `keyboard_feed_byte()` or `mouse_feed_byte()`, and is called by IRQ1, IRQ12, and `speaker.c`'s delay loop alike — whichever gets there first delivers the byte to the right device instead of consuming it. The loop is capped at 64 iterations so an ISR can never spin unbounded on hardware.
- **Packet Overflow Rejection:** Bits 6/7 of packet byte 0 flag X/Y delta overflow. Those deltas are meaningless, so the driver keeps the button state and discards the movement rather than teleporting the cursor.
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
- **Interactive Shell Aliases & Shortcuts:** Allows local registration of shortcuts (e.g. `alias ll="ls -la"`, `alias ..="cd .."`) including wrapper aliases for complex MCT launches (e.g. `alias browser="run apps/browser.mct"`). Type `alias` to print all active shortcuts.
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
- **Robust Path Sanitization:** Built-in `sanitize_path` to strip quotes, clean whitespace, and ensure resilient navigation (`cd`) and text display (`cat`) even with spaces in pathnames (e.g. `"notepad tes"`).
- **Hang-Free Waiting:** `ex_cmd()`'s live entry point is `SYS_EXEC_CMD`, and `int 0x80` is an interrupt gate — so the whole command runs with `IF=0` and the PIT cannot tick. Nothing in the shell may therefore block on the clock. `sleep` calls `task_sleep()`, which re-enables interrupts and yields to the scheduler; the ARP/ping/DNS waits go through `net_wait_for()`, bounded by both a tick deadline *and* a hard spin cap so they return even when the tick source is frozen. Making the syscall path itself preemptible (a trap gate) remains the deeper fix and requires auditing every handler for re-entrancy first.

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

### 27. SMP — ACPI MADT/XSDT Parsing & Multi-Core (src/sys/acpi.c + src/sys/smp.c)
- **Fixed Table Walker:** The RSDT scan compared every table's address against the RSDT's own memory region (`addr + len`), so *every* entry was skipped and the MADT never surfaced — SMP silently fell back to one core. The walker now validates pointers against the whole physical map and follows both the 32-bit RSDT and 64-bit XSDT (ACPI 2.0).
- **Verified MADT Parsing:** Local APIC base, all 4 CPU cores (APIC IDs 0–3), the I/O APIC, and PIT Interrupt Source Overrides are now read correctly.
- **Real Multi-Core:** `smp_init()` boots Application Processors via INIT-SIPI-SIPI with per-core GDT/TSS/IDT; log shows `[SMP] CPU 0x01 is awake!` / `Active APs: 0x03`. The scheduler spreads tasks across all 4 cores (`current_task[16]` is per-CPU).

### 28. In-Kernel GDB Stub (src/drivers/gdb_stub.c + src/drivers/gdb_stub.h)
- **GDB Remote Serial Protocol on COM2:** COM1 keeps the debug log; COM2 speaks RSP so a real `gdb` can attach over TCP (`-serial tcp:127.0.0.1:2345,server=on,wait=off`).
- **F12 Break-In:** Pressing F12 in the guest traps into the stub (int3, DPL=3 gate). `gdb_stub_poll()` answers GDB's connect handshake while the OS is running, so you can attach at any time.
- **Full Command Set:** register read/write (`g`/`G`/`p`/`P`, little-endian byte order), memory (`m`/`M`), software breakpoints (`Z0`/`z0`, 0xCC-based with original-byte restore), single-step (TF flag), continue, detach, and thread queries.
- **Crash-Safe:** all I/O is bounded-polling; a stray int3 with no debugger attached resumes after a timeout instead of hanging. Memory access is clamped to identity-mapped RAM, and breakpoint addresses pass the same clamp.
- **Debug Symbols:** `CFLAGS` now include `-g`, so `gdb myos.bin` resolves `kernel_main`, `gdb_stub_break`, and friends — verified: `0x00101991 in gdb_stub_break () at src/drivers/gdb_stub.c`.

### 29. ELF32 Application Loader (src/sys/loader.c + scripts/build_elf.py)
- **Auto-Detecting Loader:** `load_mct_app_with_arg()` sniffs the magic — `\x7fELF` routes to the ELF path, `MCT1` to the legacy path. Every existing launcher (desktop icons, `jalankan`, taskbar) supports ELF with no changes.
- **Program-Header Based Mapping:** Parses the ELF header (full 16-byte `e_ident`, so field alignment is correct), validates the PHDR table with 64-bit math, and maps each `PT_LOAD` segment at its own `p_vaddr`, zero-filling the BSS remainder. Entry point comes from `e_entry`.
- **Hardened Against Malformed Inputs:** segments are clamped to 16MB and kept below the shared-library region, PHDR-table overflow can't slip a crafted binary past the bounds check, and every segment is verified to lie inside the read file.
- **Build Tooling:** `scripts/build_elf.py` produces a freestanding ELF32 ET_EXEC linked at `0x08000000` (entry `_start`) — the same layout as `.mct`, so existing apps build unchanged. Verified end-to-end: `apps/elfdemo.elf` boots, prints from Ring 3, and opens a window.

### 30. Semaphores & Futexes (src/sys/sync.c + src/include/sync.h)
- **Counting Semaphores:** id-based (`sys_sem_create` / `wait` / `post` / `destroy`) with FIFO wait queues. `wait` on count 0 parks the task in `TASK_STATE_BLOCKED`; `post` hands the token directly to the first waiter. Queue-full refuses to block instead of hanging the task forever.
- **Futexes:** keyed by (address-space, address) — `sys_futex_wait(addr, expected)` sleeps only while `*addr == expected`, with the classic double-check under lock against missed wakeups; `sys_futex_wake` wakes up to N waiters and reclaims the slot when empty.
- **Scheduler Integration:** a BLOCKED task is invisible to the scheduler's READY scan, so it burns zero CPU until woken — verified by `apps/syncdemo.elf` (two worker threads, semaphore + futex, clean exit).
- **User Pointer Validation:** futex addresses pass `validate_user_ptr` before being dereferenced — an unmapped Ring 3 address can't page-fault the kernel.

### 31. IRQ-Driven Networking & UDP API (src/drivers/net.c + src/drivers/rtl8139.c)
- **RTL8139 Interrupts Enabled:** `RTL_IMR` now unmasks ROK/RER/TOK/TER, and IRQ 11 is routed through the I/O APIC to INT 43 (with a legacy-PIC fallback unmask).
- **IRQ-Driven RX:** `net_irq_handler()` (registered for INT 43) drains the RX ring directly in interrupt context — packets are processed the instant they arrive, no more waiting for the main loop. `net_poll()` remains as a bounded, `cli`-wrapped fallback for the shell's busy-waits.
- **UDP Datagram API:** `SYS_UDP_BIND` / `SYS_UDP_SEND` / `SYS_UDP_RECV` give Ring 3 apps a real UDP socket: bind a local port, send to any IP, and receive queued datagrams (single-socket design, DNS traffic stays separate). Verified by `apps/udptest.elf`.

### 32. Ext2 Write Support — Full Persistence (src/sys/ext2.c + src/sys/vfs.c)
- **Real On-Disk Writes:** The secondary ext2 partition is no longer read-only. A new write layer persists every change: `ext2_write_inode` (read-modify-write of the inode slot), `ext2_sync_super` (superblock + BGD table), block/inode bitmap allocators with free-count tracking, and `ata_write_sector_drive` (the ATA driver's missing slave-drive write path).
- **File Grow & Truncate:** `ext2_write_file_data` allocates blocks on demand across direct (0–11) and singly-indirect pointers, frees blocks past the new end on shrink, and zeroes the tail of the last partial block so stale bytes never survive a truncate. `i_blocks` is recomputed, so `fsck` sees consistent sizes.
- **Directory Entry Management:** `ext2_create_entry` builds files and directories (`.`, `..`, parent link-count bump); directory entries are inserted by splitting slack entries (e.g. the big `lost+found` tail record) or reusing deleted-entry holes, with a fallback to appending a fresh data block. `ext2_remove_entry` and `ext2_rename_entry` (same-directory) free inodes/blocks and compact the directory.
- **VFS Routing:** `vfs_create_node`, `vfs_write_file`, `vfs_delete_node` and `vfs_rename` now detect `FS_EXT2_FILE` / `FS_EXT2_DIR` nodes and call the real filesystem — so `mkdir`, `touch`, `nano`, `cat` and `rm` all work under `/ext2` and survive reboot.
- **Two Pre-Existing Mount Bugs Fixed:** `/ext2` was created as a plain `FS_DIR` instead of `FS_EXT2_DIR`, silently routing every "ext2" write into the MECTOVFS disk, and the mount node carried `ext2_inode = 0` instead of the root-dir inode 2. Both are corrected (with in-place migration for existing disk images).
- **Hardened:** allocators refuse metadata blocks (boot/superblock/BGD/bitmaps/inode tables) even if a corrupt bitmap marks them free; all bitmap/sync buffers are heap-allocated to keep the 16KB kernel stack safe; cross-directory ext2 renames are rejected rather than corrupting the tree. Validated end-to-end: write → reboot → read-back, plus `fsck.ext2` clean and `debugfs` reading the files straight from the image, on both 1024- and 4096-byte-block filesystems.

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
| 40 | SYS_TCP_CONNECT | Open TCP connection. EBX=ip_ptr, ECX=port → conn id (0–7) or -1 |
| 41 | SYS_TCP_SEND | Send TCP payload. EBX=data, ECX=len, EDX=conn_id → bytes sent / -1 / -2 |
| 42 | SYS_TCP_RECV | Read TCP stream. EBX=buf, ECX=max_len, EDX=conn_id → bytes / 0 / -1 EOF / -2 bad id |
| 43 | SYS_NET_STATUS | Get packed network state (DNS resolved, latest TCP state) |

### TCP Close (70)
| # | Name | Description |
|---|------|-------------|
| 70 | SYS_TCP_CLOSE | Gracefully close a TCP connection (FIN handshake). EBX=conn_id → 0/-1 |

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

### Synchronization — Semaphores & Futexes (61–66)
| # | Name | Description |
|---|------|-------------|
| 61 | SYS_SEM_CREATE | Create counting semaphore. EBX=initial_count → sem_id |
| 62 | SYS_SEM_WAIT | Block on semaphore. EBX=sem_id → 0/-1 (parks task when count==0) |
| 63 | SYS_SEM_POST | Release semaphore. EBX=sem_id → 0/-1 (wakes one waiter) |
| 64 | SYS_SEM_DESTROY | Destroy semaphore, wake all waiters. EBX=sem_id |
| 65 | SYS_FUTEX_WAIT | Sleep while *addr==expected. EBX=addr, ECX=expected → 0 slept / -1 changed / -2 invalid |
| 66 | SYS_FUTEX_WAKE | Wake waiters on addr. EBX=addr, ECX=max_waiters → woken count |

### UDP Networking (67–69)
| # | Name | Description |
|---|------|-------------|
| 67 | SYS_UDP_BIND | Bind local UDP port. EBX=port → 0/-1 |
| 68 | SYS_UDP_SEND | Send UDP datagram. EBX=ip_ptr, ECX=dst_port, EDX=data, ESI=len → 0/-1 |
| 69 | SYS_UDP_RECV | Receive queued UDP datagram. EBX=buf, ECX=max_len → bytes |

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
| ELF Demo | Ring 3 (.elf) | Proves the ELF loader — a real ELF32 binary with its own PT_LOAD segments running in Ring 3 |
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

### Building ELF Applications (.elf)
The kernel also loads standard ELF32 executables (auto-detected by magic):
```bash
python3 scripts/build_elf.py apps/elfdemo.c elfdemo.elf
```
This produces a freestanding ET_EXEC linked at `0x08000000` with entry `_start`. Embed and inject it into the VFS the same way the Makefile handles `elfdemo.elf` / `syncdemo.elf` / `udptest.elf`.

### Debugging the Kernel with GDB
```bash
# Terminal 1 — boot QEMU with COM2 as a TCP server
qemu-system-i386 -cdrom mectov.iso -m 128 -smp 4 \
  -serial file:serial.log -serial tcp:127.0.0.1:2345,server=on,wait=off \
  -drive file=disk.img,format=raw,index=0,media=disk

# Terminal 2 — attach GDB (press F12 inside the guest to break in)
gdb myos.bin -ex "target remote :2345" -ex "c"
```

### CI Boot Test
```bash
python3 scripts/boot_test.py   # boots mectov.iso in QEMU, logs in, checks BOOTED KERNEL LOOP
```
The `.github/workflows/build-boot-test.yml` workflow runs this on every push — build, ISO, QEMU boot (no KVM), login, and smoke window, all in CI.

---

---

## Version History

| Version | Highlights |
|---|---|
| v35.5 | **Multi-Connection TCP Update:** Replaced the single-socket TCP layer with an 8-slot connection table — per-connection seq/ack, 16KB receive buffers, retransmit (5×6s) and connect-timeout (10s) sweeps, and a full FIN handshake (FIN_WAIT_1/2, CLOSE_WAIT, LAST_ACK). Conn-id based API: `net_tcp_connect/send/recv/close/state/latest_state`, syscalls 40–43 now carry the conn id (`EDX`) and new `SYS_TCP_CLOSE` (70) does graceful shutdown; `sys_tcp_*` wrappers updated for Ring 3. Browser tracks/closed its conn on abort/ESC/EOF and handles peer-FIN vs reset; new `fetch [domain]` shell command streams an HTTP GET through the host gateway to EOF. Fixed a latent RTL8139 SMP race — TX descriptors/RX ring now under a spinlock (`rtl_enter`/`rtl_exit`), so the IRQ path and `net_poll()` can no longer both fire the same TX descriptor and silently drop a SYN. Validated in QEMU: two parallel connections establish, both fetch through the gateway, and close cleanly; CI boot test passes. |
| v35.4 | **Security Hardening Update:** Audited every trust boundary the kernel accepts input from (disk images, ELF/.mct binaries, Ring 3 syscalls, shell commands). Fixed the ELF loader's 32-bit wrap in the segment bounds check (crafted `p_offset` → CPL0 panic) and made both loaders abort cleanly on OOM; refused nested `sh`/`source` recursion (kernel stack overflow via `is_script` that was set but never read); sanitized the on-disk VFS node table (missing NUL in 32-byte names, out-of-range `parent`); rejected ext2 superblocks whose `s_inode_size` doesn't divide `block_size` (memcpy past 4KB stack buffers) and bound every ext2 block read/write against the block count/drive sectors; released all 16 per-task fds on exit/kill (pipe+fd leak, hung pipe readers) and refused deleting files that open fds reference (stale-slot aliasing); replaced the 100k-`pause` busy-wait `task_sleep()` with an exact hlt-park; clamped window resize/open coordinates (uncloseable and zombie windows), grew the wm free-list to 64; snapshotted mouse state with interrupts off; clamped tab completion into `cmd_b[256]`, dead-CMOS month-0, and `vfs_read_file()` disk bounds. |
| v35.3 | **English UI Localization Update:** Translated the last remaining Indonesian user-facing strings to English. Shell commands are now English-first — `snake`, `tone`, `sleep`, `date`, `color`, `lock`, `run` replace `ular`, `nada`, `tunggu`, `waktu`, `warna`, `kunci`, `jalankan` — while every legacy Indonesian name still works via built-in aliases (`buat`→`touch`, `tulis`→`nano`, `baca`→`cat`, `hapus`→`rm`, `matikan`→`shutdown`, `mulaiulang`→`reboot`). Translated shell help/mfetch text, the nano footer (`ESC: Save & Exit`), calculator prompts/results, updated explorer app launches to `run`, and synced the GUI terminal's tab-completion list with the shell's. Help now lists only English commands, with a note that legacy aliases remain functional. |
| v35.2 | **Shell File Management & Ext2 Hardening Update:** Added `cp` (VFS/ext2 copy via heap buffer), `mv` (rename with cross-directory ext2 guard), `rmdir` (empty-directory check) and `df` (mectovfs + ext2 capacity/inode usage from superblock counters) to the shell with help/man/tab-completion entries. Fixed two latent ext2 on-disk bugs found while exercising delete/rename: directory-entry absorption now extends the previous entry to the exact block end (`rec_len = block_size - prev_off`) so deleted-entry holes can't leave the directory chain short, and freed inode slots are zeroed so fsck no longer sees stale `i_blocks`/orphaned directories. Validated end-to-end: cp/mv/rmdir/df all pass in-guest, host `fsck.ext2` clean across two boots on both 1024- and 4096-byte block filesystems. |
| v35.1 | **Ext2 Write Support Update:** Made the secondary ext2 partition fully writable — block/inode bitmap allocators with superblock sync, file grow/truncate across direct + singly-indirect blocks, directory entry creation via slack-splitting and hole reuse, remove and same-dir rename. All VFS create/write/delete/rename ops under `/ext2` now hit the real filesystem. Added `ata_write_sector_drive` (slave-drive ATA writes). Fixed two pre-existing mount bugs that silently routed `/ext2` writes into the MECTOVFS disk (`/ext2` is now `FS_EXT2_DIR` with `ext2_inode = 2`). Hardened allocators against metadata-block allocation and kept stack usage under 16KB. Validated: write → reboot → read-back persistence, host `fsck.ext2` clean, `debugfs` reads files straight from the image (1024- and 4096-byte block filesystems). |
| v35.0 | **SMP, In-Kernel GDB, ELF Loader & Synchronization Update:** Fixed the ACPI table walker (RSDT/XSDT + MADT) so SMP actually boots all 4 cores; added a GDB Remote Serial Protocol stub on COM2 (F12 break, breakpoints, single-step, DWARF symbols); added a hardened ELF32 ET_EXEC loader alongside `.mct`; added kernel semaphores and address-space-keyed futexes (`TASK_STATE_BLOCKED`, zero CPU while parked); switched the RTL8139 to IRQ-driven RX (I/O APIC route, INT 43) and exposed a UDP bind/send/recv syscall API; bundled the wallpaper into the repo; and added a GitHub Actions CI boot test (build + QEMU login + smoke). |
| v34.3 | **Scheduler, VFS Reclamation, and TCP Buffer Update:** Overhauled the scheduler with a starvation-proof priority aging algorithm; implemented dynamic first-fit VFS sector reclamation to resolve file deletion leaks; and upgraded TCP receive path to a 64KB buffer with dynamic window advertising and partial read shifting. |
| v34.2 | **Memory & Syscall Hardening Update:** Enforced integer overflow checks on `kmalloc` and `kcalloc` sizes; protected `frame_ref_count` from uint8_t overflow via saturation capping; and secured `SYS_EXEC_CMD` and `SYS_KILL_TASK` syscalls to restrict shell access and task termination to authorized binaries (`terminal.mct`, `explorer.mct`, `taskmgr.mct`). |
| v34.1 | **Kernel Ownership Hardening Update:** Centralized logout/session cleanup through `wm_reset_session()` so the taskbar and kernel no longer duplicate teardown; hardened syscall array validation for window/PCI/clipboard buffers; fixed VFS path splitting and bootstrap directory creation; aligned shell sleep with its documented seconds-based behavior; added browser request timeouts; and tightened ACPI EBDA/MADT discovery bounds. |
| v34.0 | **Input Integrity & Compositor Correctness Update:** Made `vga_set_clip()` real — the clip rectangle was previously four globals that no drawing primitive ever read, so the WM's content-area clip silently did nothing; all primitives now gate on shared `clip_test()`/`clip_box()` helpers with 64-bit edge arithmetic, and `vga_set_render_target()` resets the clip between targets. Consolidated minimize/restore into `wm_minimize()`/`wm_restore()` so the state change and its dirty-region marking can no longer be separated, fixing windows minimized from the taskbar staying painted on screen. Unified all port `0x60` access behind a single `ps2_drain()` that dispatches on the 8042 AUX status bit, ending the keyboard/mouse byte theft behind stuck modifier keys and cursor teleporting, and added mouse packet overflow-bit rejection. Removed every unkillable spin from the shell: `tunggu` now uses `task_sleep()` and the ARP/ping/DNS waits use the doubly-bounded `net_wait_for()`, since `int 0x80` is an interrupt gate and `get_ticks()` cannot advance during a syscall. Fixed `vfs_read_file()` writing its NUL terminator one byte past the caller's buffer on exact-fit reads. Marked `timer_ticks` `volatile`. |
| v33.1 | **Start Menu Ghosting Fix:** Reordered `full_redraw` to call `taskbar_pre_draw()` before `desktop_draw()` so dirty rectangles are applied before the background is painted, ensuring closed popups are erased from the screen. |
| v33.0 | **Multi-Core (SMP) & APIC Update:** Boot and initialize Application Processors via the INIT-SIPI-SIPI sequence with per-core GDT/TSS and IDT loading. Added Local APIC and I/O APIC drivers, disabling the legacy PIC and routing keyboard, mouse, and timer interrupts to the BSP. Parses the MADT for Interrupt Source Overrides to correctly route the PIT (IRQ0) to GSI 2 under QEMU. Resolved nested-interrupt scheduler deadlocks by removing spinlocks from `schedule()` and disabling interrupts before acquiring `task_lock` in task helpers. Also fixed Start Menu dismissal on outside clicks and desktop icon dragging. |
| v32.0 | **Syscall Modularization & VMM Memory Safety Update:** Refactored monolithic `syscall.c` into modular handlers (`syscall_gui.c`, `syscall_vfs.c`, etc.). Fixed virtual memory heap overlap at `0x08000000` by placing `heap_ptr` after loaded app segments. Patched `task_cleanup` to switch `CR3` back to kernel boot directory before freeing process address space. Bound-checked VFS node allocation in `ext2.c` to prevent `fs_nodes[-1]` array underflow. |
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

GNU General Public License v2.0 (GPLv2)

Created by M Alif Fadlan.

Kamu bebas menggunakan, memodifikasi, dan mendistribusikan project ini
selama turunan yang kamu distribusikan juga dilisensikan GPLv2 dan
menyertakan source code-nya. Karena kernel ini terhubung langsung dengan
source code DOOM (juga GPLv2), seluruh work gabungan dilisensikan GPLv2. 

Salah satu proyek OS hobi buatan Indonesia yang paling lengkap dan terdokumentasi dengan baik di GitHub saat ini