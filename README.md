# Mectov OS

[![CI Boot Test](https://github.com/MAliffadlan/Mectov_OS/actions/workflows/build-boot-test.yml/badge.svg)](https://github.com/MAliffadlan/Mectov_OS/actions/workflows/build-boot-test.yml)

A monolithic x86 kernel written from scratch in C and Assembly — no external libraries, no libc, no POSIX. Every byte runs directly on hardware: GRUB Multiboot boot, protected mode with paging, a graphical desktop with a floating-window manager, a real Unix process model, and an SMP scheduler that runs user tasks on all four CPU cores.

---

## Table of Contents

- [Overview](#overview)
- [Highlights](#highlights)
- [Architecture](#architecture)
- [Project Structure](#project-structure)
- [Core Subsystems](#core-subsystems)
- [Syscall API Reference](#syscall-api-reference)
- [Applications](#applications)
- [Build and Run](#build-and-run)
- [Testing](#testing)
- [Version History](#version-history)
- [License](#license)

---

## Overview

Mectov OS is a from-scratch i386 operating system with:

- **Ring 0 / Ring 3 isolation** — user applications run in separate address spaces with COW fork, demand-paged `mmap()`, and per-process page directories.
- **True SMP** — per-CPU runqueues, work-stealing migration, and a LAPIC timer per core; all four QEMU cores execute tasks (verified live by SysInfo's per-core load bars).
- **A Unix process model** — `fork` / `exec` / `waitpid`, POSIX signals (`sigaction`, `sa_mask`, `SA_RESTART`), process groups, sessions and a controlling terminal with SIGTTIN/SIGTTOU, plus System V shared memory, pipes, and fds.
- **A real shell (msh)** — env vars with `$VAR` expansion, aliases, tab completion, scripts, job control, and a growing POSIX toolkit (`uname`, `whoami`, `hostname`, `env`, `seq`, `head`, `grep`, `cp`, `mv`, `df`, ...).
- **A graphical desktop** — double-buffered window manager (resize, Aero snap, rounded corners), taskbar with system tray, draggable persistent icons, and a graphical login screen.
- **On-disk persistence** — an ATA-backed VFS, a fully writable ext2 partition (verified with host `fsck.ext2`), and a FAT32 volume at `/fat32` (drive 3, LFN read/write).
- **Networking** — RTL8139 driver with IRQ-driven RX, multi-connection TCP, UDP, ARP/ICMP/DNS, and a browser that reaches the real internet through a host gateway proxy.
- **Audio** — Sound Blaster 16 driver with WAV playback and PC-speaker tones.
- **Developer tooling** — in-kernel GDB stub (attach over TCP, breakpoints, single-step), ELF32 + custom `.mct` loaders, and a shared-library runtime that keeps app binaries under 2 KB.

---

## Highlights

**Scheduling & SMP (v36.7–v36.8)**
- Per-CPU runqueues: each core picks with priority + aging, steals work from idle peers (migration), and parks in a pinned Ring 0 idle task.
- LAPIC timer per Application Processor, PIT-calibrated once on the BSP (concurrent calibration corrupted the shared PIT and caused timer storms).
- Every runqueue mutation under one `task_lock`, cli-first — the timer IRQ can never self-deadlock in `schedule()`.
- Per-CPU load sampling exposed to Ring 3: SysInfo renders four live utilization bars; the serial log prints `[LOAD] c0=.. c1=..` every 3 s.
- `smpstress` forks 8 children (CPU burn + sleep + pipes + self-signals) in two parallel waves across all four cores — all 16 exit and are reaped.

**Process model & signals (v36.1–v36.6, v38.5–v38.7)**
- COW `fork()` with deterministic syscall-frame copying (the KVM fork-GPF fix), in-place `exec()` via live-frame patching, blocking `waitpid()` with WNOHANG.
- POSIX signals with `SIGACTION`/`SIGPROCMASK`, handler sigframes on the user stack, `SA_RESTART` re-parking interrupted sleeps — and unresolvable Ring 3 faults deliver `SIGSEGV` to user handlers instead of killing the task.
- Process groups + sessions + controlling terminal: `setpgid`, `setsid`, `tcsetpgrp`; background readers get stopped by SIGTTIN; Ctrl+C/Ctrl+Z target the foreground group.
- Real pipelines and redirection: `run A | run B`, `run A > file`, `run A < file` through dup2 + a per-task fd table.
- Job control: Ctrl+Z stops, `jobs` lists, `bg` resumes, `fg` waits, `kill %1` terminates.
- Demand paging for heap and stack: a shared lazy zero page + COW backs untouched pages, and an unmapped guard page catches stack overflow cleanly.

**Shell & toolkit (v36.9, v37.x)**
- POSIX-style builtins across three toolkit rounds: `uname`, `whoami`, `hostname`, `env`, `seq`, `head`, `wc`, `type`, `cat -n`, `yes`, `printf`, `sort`, `uniq`, `tee`, `find` — all pipe-aware.
- Shell scripting: `for VAR in list`, `while true/false`, `$VAR` expansion, `break`, 4-level nesting; env vars + aliases + tab completion.
- `passwd` stores the login password in `/etc/passwd` (single source, no hardcoded default); a single `OS_VERSION` constant keeps the help banner, `mfetch`, `uname` and login footer in sync.

**Memory & kernel hardening (v38.3–v38.8)**
- Frame-bitmap physical allocator sized from the multiboot memory map (RAM above 128 MB usable; identity map to 512 MB), per-process address spaces, COW with frame refcounting, demand-paged `mmap()` (`0x40000000..0x80000000`), System V shared memory.
- File-backed `mmap()` (`SYS_MMAP_FILE` + `SYS_MSYNC`): pages fault in from disk on demand, dirty pages write back on msync/munmap, `fork()` shares them MAP_SHARED-style.
- 16 KB kernel stacks with a 4 KB guard page below each; vector 8 is a hardware task gate so an overflow prints `[PANIC] DOUBLE FAULT` instead of triple-faulting into a silent reboot.
- Lock audit: per-structure irqsave spinlocks replaced global `cli()` — syscalls are preemptible and SMP parallelizes; heap gets magic + canary + redzone, clean panic on double-free, OOM returns NULL.
- `dmesg` — a 64 KB in-memory kernel log ring (crash lines included) exposed via `dmesg` and `/proc/dmesg`.

**Filesystems & storage (v37.0, v38.12, v38.15–v38.16)**
- Persistent VFS with auto-save; a fully writable ext2 partition (host-verified `fsck.ext2` clean); a **FAT32 volume at `/fat32` (drive 3) with long file names (LFN) read AND written**, interoperable with host `mkfs.fat`/mtools both ways.
- POSIX file positioning: `SYS_LSEEK` (`SEEK_SET/CUR/END`), `SYS_FSTAT`, honored `O_APPEND`; fd reads advance a real offset.
- Virtual `/proc` generated live from kernel state: `/proc/tasks`, `/proc/meminfo`, `/proc/cpuinfo`, `/proc/uptime`, `/proc/version`, `/proc/dmesg`.

**Network (v35.5, v38.11)**
- 8-slot multi-connection TCP with seq/ack, retransmit/timeout sweeps and a full FIN handshake; UDP bind/send/recv; DNS via the QEMU gateway.
- **DHCP client (RFC 2131)** configures IP/netmask/gateway/DNS at runtime over UDP broadcast, with static fallback (`ipconfig` prints the result); RTL8139 IRQ-driven RX (spinlock-guarded against the SMP TX race).

**GUI & input (v37.x–v38.x)**
- Double-buffered WM: dirty-region tracking, Aero snap, 8-directional resize, traffic-light controls, scroll-wheel events (IntelliMouse), 16-window capacity, shared `ps2_drain()` arbitration so keyboard and mouse never steal each other's bytes.
- Amber instrument-console identity: amber window chrome and taskbar, a Windows-style lock screen → password login, live RTC clock.
- Smooth compositing: delta-copy VRAM presentation (~10× less MMIO traffic), truthful FPS HUD, tick-scaled 60 FPS cap — <1 ms/frame on KVM.

**App runtime & tooling (v38.3, v38.13)**
- `.mct` binary format + standard ELF32 loader + shared-library runtime (app binaries ~2 KB); the first **Rust** Ring 3 app (`apps/rusthello.rs`, `no_std`, same MCT1 format) proves the loader is language-agnostic.
- The 3,384-line shell monolith split into a dispatcher + 72 per-command modules; in-kernel GDB stub (COM2, breakpoints, single-step, DWARF symbols).

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
|  PIT Timer (1kHz, BSP) |  LAPIC Timer (per AP)  |  PS/2  |  Serial |
+--------------------------------------------------------------------+
|  Memory Manager  |  VMM (per-process page dirs, COW, mmap, shm)    |
+--------------------------------------------------------------------+
|  ACPI (RSDT/XSDT/MADT)  |  SMP (APIC/IOAPIC, 4 cores)              |
+--------------------------------------------------------------------+
|  Per-CPU Runqueues  |  Semaphores & Futexes  |  VFS + ATA PIO      |
+--------------------------------------------------------------------+
|  VGA/VESA Driver   |  Window Manager  |  RTL8139 NIC (IRQ-driven)  |
+--------------------------------------------------------------------+
|  Network Stack (Ethernet/ARP/IPv4/ICMP/UDP/TCP/DNS)                |
+--------------------------------------------------------------------+
|  MCT + ELF32 Loaders |  Ring 3 User Tasks |  GDB Stub (COM2)       |
+--------------------------------------------------------------------+
|  Desktop |  Taskbar  |  Login Screen  |  fd layer  |  UNIX pipes   |
+--------------------------------------------------------------------+
```

### Target Platform

| Component | Detail |
|---|---|
| CPU | i386 (32-bit x86), monolithic kernel |
| Privilege | Ring 0 (kernel) + Ring 3 (user) |
| Scheduler | Preemptive, per-CPU runqueues, priority + aging, work stealing |
| Display | VESA VBE linear framebuffer, 1024×768, 32-bit color |
| Storage | ATA PIO (IDE) — drive 0 VFS, drive 1 ext2, drive 3 FAT32 (drive 2 CD-ROM) |
| Audio | Sound Blaster 16 + PC speaker |
| Network | RTL8139 (virtual, via QEMU) |
| Serial | COM1/COM2 UART 16550A (38400 baud, 8N1) |

---

## Project Structure

```
boot.asm               Multiboot header + 32-bit bootstrap
kernel.c               kernel_main: init order, main loop, input dispatch
linker.ld / Makefile   Kernel link script + parallel build (target-isolated)
run.sh                 QEMU launcher (KVM, 4 cores, gateway proxy, serial log)

src/
  sys/                 Kernel core
    task.c             Scheduler: per-CPU runqueues, fork/exec/waitpid/signals
    schedule, sleep/wake, per-CPU load sampling
    vmm.c              Frame allocator, page dirs, COW, mmap, shm frames
    loader.c           MCT + ELF32 loaders, loader_build_image/task_exec
    syscall.c/.gui/.vfs/.net/.ipc/.proc   Modular syscall handlers
    shell.c            Kernel shell (commands, env vars, aliases, jobs)
    smp.c apic.c acpi.c  AP boot (INIT-SIPI-SIPI), APIC/IOAPIC, MADT
    ext2.c vfs.c fd.c  ext2 write layer, VFS, file descriptors, pipes
    sync.c ipc.c shm.c clipboard.c security.c  Sync primitives, IPC, shm
    gdt.c idt.c timer.c interrupt_entry.asm  CPU setup, IRQ dispatch, 1 kHz tick
  drivers/
    vga.c font8x16.c   Framebuffer compositor, primitives, font
    keyboard.c mouse.c serial.c rtc.c timer.c speaker.c
    ata.c pci.c net.c rtl8139.c sb16.c dma.c gdb_stub.c
  gui/
    wm.c desktop.c taskbar.c login.c   Window manager + desktop shell
  include/             Public headers (syscall.h, task.h, vmm.h, ...)

apps/                  Ring 3 user applications
  terminal.c nano.c notepad.c explorer.c sysinfo.c taskmgr.c browser.c
  clock.c calc.c gcalc.c snake.c flappy.c pci.c mplayer.c volume.c
  hello.c elfdemo.c syncdemo.elf udptest.elf    (ELF32 demos)
  forkdemo.c execdemo.c shmdemo.c mmapdemo.c mmapfiledemo.c lseekfiledemo.c
  sigdemo.c bgread.c smpstress.c pipegen.c piperead.c tcpserver.c looper.c
  rusthello.rs               (FIRST RING 3 APP IN RUST — freestanding no_std,
                             built by scripts/build_rust_mct.py, same MCT1
                             format the C apps use; rustc ≥ stable via rustup,
                             target i686-unknown-uefi, entry resolved at link)
  lib/libc.c libc.h    Homegrown dynamic shared library (libc.mct)

scripts/               build_mct.py, build_elf.py, inject_vfs.py, gateway.py,
                       boot_test.py, debug.py (serial log reader)
docs/architecture/     scheduler.md, memory.md, syscalls.md, smp_and_apic.md
.github/workflows/     build-boot-test.yml (CI boot test)
doom/                  Embedded DOOM engine (GPLv2)
```

---

## Core Subsystems

### Kernel & Memory

| Subsystem | Highlights |
|---|---|
| Virtual Memory (`vmm.c`) | Frame-bitmap allocator (32 768 pages), per-process page dirs, region allocator, COW with refcounts |
| Demand paging | `mmap()` reserves zero frames; the PF handler lazily maps+zeroes one page per fault |
| Shared memory | System V segments, `PAGE_SHARED` skips COW, leak-proof attach bitmap |
| Syscall layer | Modular handlers, every user pointer validated (`validate_user_ptr`) |
| GDT/IDT/TSS | Ring 0/3 segments, interrupt-gate IRQs, trap-gate `int 0x80` (preemptible syscalls) |
| Debugging | In-kernel GDB stub (RSP on COM2): F12 break, breakpoints, single-step, DWARF symbols |

### Scheduling & SMP (`task.c`, `smp.c`, `apic.c`)

- Per-CPU runqueues with priority (IDLE < LOW < NORMAL < HIGH) + aging; work-stealing migration between cores.
- APs tick via their own LAPIC timer (PIT-calibrated once on the BSP before AP wake — prevents timer storms).
- One `task_lock`, cli-first everywhere; signal delivery on the syscall-return path is locked too.
- Task states: FREE / RUNNING / READY / SLEEP / BLOCKED / STOPPED / ZOMBIE; hlt-park for zero-CPU waits.
- Per-CPU load sampling → `SYS_GET_SYSINFO` (`cpu_count`, `cpu_load[4]`) → SysInfo live bars.
- 16 KB kernel + 64 KB user stacks per task; per-CPU TSS with kernel-stack switching.

### Process Model (`syscall_proc.c`, `loader.c`)

| Feature | Notes |
|---|---|
| `fork()` | COW address-space clone, kernel-stack frame copy from `TSS.esp0 - sizeof(registers_t)` (deterministic), fd/handler/mmap/shm inheritance |
| `exec()` | Live syscall-frame patching; caught handlers reset to default, pending signals cleared (POSIX) |
| `waitpid()` | Blocks in TASK_STATE_BLOCKED, reaps zombie, WNOHANG; 15 s orphan-zombie safety net |
| Signals | sigaction/sa_mask/sigprocmask/SA_RESTART; sigframes on user stack; default action = exit `128+sig` |
| Job control | pgrp/session per task; SIGTTIN/SIGTTOU on background tty access; fg/bg/jobs/kill %n |
| Shell | env vars + `$VAR`, aliases, tab completion, scripts, `ps`/`kill`/`uptime`/`memstat`, `fetch`; POSIX toolkit `uname`/`whoami`/`hostname`/`env`/`seq`/`head` |

### Filesystems & Storage

- **VFS** — 16-slot node table over ATA PIO, auto-save, growth guard (files move to the end of disk instead of overwriting neighbors).
- **ext2** — fully writable secondary partition: block/inode bitmap allocators, file grow/truncate (direct + singly-indirect), directory entry create/remove/rename with slack-splitting, superblock sync; allocators refuse metadata blocks; host `fsck.ext2` clean.
- **FAT32** — third drive (secondary slave, `index=3`), mounted at `/fat32`: BPB parsing, FAT cluster-chain traversal, **long file names (LFN) read + written** (reverse-order chain, checksum-verified; SFN fallback), cluster allocation/free on write/create/delete, whole-tree rebuild at mount; volumes created by host `mkfs.fat`/mtools work both ways.
- **FD layer** — 16 fds/task, 128 global, typed (`FILE`/`PIPE_READ`/`PIPE_WRITE`/`DEV`), refcounted, closed on task teardown.

### Network

- RTL8139 driver (bus mastering, IRQ 11 → INT 43), spinlock-serialized TX/RX (SMP race fix).
- Full in-kernel stack: Ethernet, ARP, IPv4, ICMP, UDP, DNS (via QEMU gateway `10.0.2.3`).
- Multi-connection TCP: 8 slots, per-conn seq/ack + 16 KB RX buffer, retransmit (5×6 s) + connect timeout (10 s), FIN handshake.
- UDP bind/send/recv syscalls; `fetch [domain]`, `ping`, `host` shell commands; host `gateway.py` proxy for real-internet HTTP.

### GUI & Input

- Double-buffered WM: dirty-region tracking, z-order, rounded corners, traffic-light buttons, Aero snap, 8-directional resize (min 220×150), minimized-window ghost prevention.
- Desktop: baked wallpaper, squircle icons, draggable + persistent positions, double-click launch.
- Taskbar: per-app icon buttons, WiFi indicator, CAPS + HDD LEDs, WIB clock; system tray.
- Input: IntelliMouse 4-byte protocol + scroll wheel (event type 4), shared `ps2_drain()` 8042 arbitration, modifier-key tracking (Alt+Tab).
- Rendering: triple-buffer delta compositing (~4 ms/frame), enforced clip rectangle on all primitives.

### Audio

- SB16 native driver, 8/16-bit mono/stereo WAV streaming, tone synthesis, volume control (scroll-adjust), mapped to QEMU's PulseAudio backend.

### App Runtime

- `.mct` binary format (16-byte header, fixed-base mapping) + standard ELF32 ET_EXEC loader (auto-detected by magic).
- Shared-library system (`libc.mct`): dynamic symbol resolution via export-table pointer; app binaries ~2 KB.
- `SYS_SET_STDOUT_IPC` lets apps stream output to queues; pipelines/redirection via `task_fork_exec` + dup2 rewiring.

---

## Syscall API Reference

All syscalls are invoked via `int 0x80` (trap gate). Registers: `EAX` = number, `EBX`/`ECX`/`EDX`/`ESI`/`EDI` = arguments.

### Core (1–10)
| # | Name | Description |
|---|------|-------------|
| 1 | SYS_PRINT | Print string. EBX=str_ptr, ECX=color |
| 2 | SYS_OPEN | Open/create VFS file. EBX=filename → fd |
| 3 | SYS_READ | Read file. EBX=fd, ECX=buf, EDX=size |
| 4 | SYS_WRITE | Write file/pipe/serial. EBX=fd, ECX=buf, EDX=size |
| 5 | SYS_CLOSE | Close fd. EBX=fd |
| 6–7 | SYS_MALLOC / SYS_FREE | Identity-mapped heap allocate/free |
| 8 | SYS_GET_TICKS | PIT tick count |
| 9 | SYS_YIELD | Yield CPU (`sti; hlt; cli`) |
| 10 | SYS_EXIT | Terminate current task |

### GUI (11–17)
| # | Name | Description |
|---|------|-------------|
| 11 | SYS_DRAW_RECT | EBX=win_id, ECX=x, EDX=y, ESI=(w<<16)\|h, EDI=color |
| 12 | SYS_DRAW_TEXT | EBX=win_id, ECX=x, EDX=y, ESI=str, EDI=color |
| 13 | SYS_GET_KEY | Non-blocking keyboard char |
| 14 | SYS_GET_MOUSE | Mouse state → EAX=x, EBX=y, ECX=buttons |
| 15 | SYS_CREATE_WINDOW | EBX=x, ECX=y, EDX=w, ESI=h, EDI=title → win_id |
| 16 | SYS_GET_EVENT | Window event (paint/key/mouse/scroll/close) |
| 17 | SYS_UPDATE_WINDOW | Commit draw commands |

### Threads & Processes (18–22, 46–51)
| # | Name | Description |
|---|------|-------------|
| 18 | SYS_THREAD_CREATE | EBX=entry, ECX=priority, EDX=page_dir → TID |
| 19 | SYS_SLEEP | Sleep EBX ticks |
| 20–22 | SYS_GET_PID / SET_PRIORITY / GET_PRIORITY | Task identity + priority |
| 46 | SYS_GET_TASKS | List tasks. EBX=array, ECX=max |
| 47 | SYS_GET_WINDOWS | List windows. EBX=array, ECX=max |
| 48 | SYS_KILL_TASK | Force kill EBX=tid |
| 49 | SYS_GET_LAUNCH_ARG | Launch argument string |
| 50 | SYS_CREATE_FILE | Create empty VFS file |
| 51 | SYS_LOAD_LIBRARY | Dynamically load shared library → base |

### IPC (23–28)
| # | Name | Description |
|---|------|-------------|
| 23–26 | SYS_IPC_CREATE / SEND / RECV / DESTROY | Named 64-byte message queues |
| 27–28 | SYS_IPC_TRY_SEND / TRY_RECV | Non-blocking variants |

### Memory & VM (29–31, 78–83, 93–94)
| # | Name | Description |
|---|------|-------------|
| 29–31 | SYS_VMM_MAP / ALLOC / FREE | Low-level page mapping |
| 78 | SYS_SHMGET | Create/attach segment. EBX=key, ECX=size → id |
| 79–81 | SYS_SHMAT / SHMDT / SHMCTL | Attach / detach / IPC_RMID |
| 82 | SYS_MMAP | Reserve VA range (lazy zero-fault). EBX=size → base |
| 83 | SYS_MUNMAP | Free faulted frames of a range |
| 93 | SYS_MMAP_FILE | Map an open VFS file fd; pages fault in lazily **from disk**; dirty pages write back on msync/munmap. EBX=fd, ECX=flags |
| 94 | SYS_MSYNC | Flush dirty pages of a file-backed mapping back to the file |
| 95 | SYS_LSEEK | Reposition fd read/write offset. EBX=fd, ECX=offset, EDX=whence(SEEK_SET/CUR/END) → new offset |
| 96 | SYS_FSTAT | File metadata by fd. EBX=fd, ECX=stat_t* {size,type,node_idx,parent,data_sector,name} |

### I/O Multiplexing & POSIX misc (97–101)
| # | Name | Description |
|---|------|-------------|
| 97 | SYS_POLL | POSIX poll(). EBX=pollfd_t*, ECX=nfds, EDX=timeout_ms (<0 = forever) → ready count |
| 98 | SYS_SELECT | POSIX select(). EBX=nfds, ECX=readfds*, EDX=writefds*, ESI=exceptfds*, EDI=timeout_ms (uint32_t bitmaps) → ready count |
| 99 | SYS_GETCWD | Absolute path of the working directory. EBX=buf, ECX=size → 0 |
| 100 | SYS_CHDIR | Change working directory. EBX=path (relative or absolute) → 0 |
| 101 | SYS_CLOCK_GETTIME | Monotonic uptime. EBX=CLOCK_MONOTONIC, ECX=timespec_t* {tv_sec,tv_nsec} → 0 |

### UNIX Compatibility & Hardware (32–38)
| # | Name | Description |
|---|------|-------------|
| 32 | SYS_PIPE | Create pipe pair. EBX=pipefd[2] |
| 33 | SYS_GET_TIME | RTC time. EBX=rtc_time_t* |
| 34 | SYS_PLAY_SOUND | Tone. EBX=freq, ECX=ms |
| 35 | SYS_GET_SYSINFO | Hardware + stats incl. per-core load |
| 36–38 | SYS_GET_PCI_INFO / LIST_DIR / STAT_FILE | PCI, directory, file info |

### Network (39–43, 67–70)
| # | Name | Description |
|---|------|-------------|
| 39 | SYS_DNS_RESOLVE | Resolve domain |
| 40 | SYS_TCP_CONNECT | EBX=ip, ECX=port → conn id 0–7 |
| 41–42 | SYS_TCP_SEND / RECV | EDX=conn_id |
| 43 | SYS_NET_STATUS | Packed network state |
| 67–69 | SYS_UDP_BIND / SEND / RECV | Datagram sockets |
| 70 | SYS_TCP_CLOSE | Graceful FIN shutdown. EBX=conn_id |
| 85 | SYS_TCP_LISTEN | Listen on a port (TCP server) |

### Synchronization (61–66)
| # | Name | Description |
|---|------|-------------|
| 61–64 | SYS_SEM_CREATE / WAIT / POST / DESTROY | Counting semaphores, FIFO waiters |
| 65 | SYS_FUTEX_WAIT | Sleep while *addr==expected. EBX=addr, ECX=expected |
| 66 | SYS_FUTEX_WAKE | EBX=addr, ECX=max_waiters → woken |

### Process Model & Signals (71–77, 84, 86–92)
| # | Name | Description |
|---|------|-------------|
| 71 | SYS_FORK | COW clone → child tid / 0 / -1 |
| 72 | SYS_WAITPID | EBX=pid, ECX=status, EDX=WNOHANG |
| 73 | SYS_KILL | Send signal. EBX=pid, ECX=sig |
| 74 | SYS_SIGNAL | Install handler (0 default, 1 ignore) |
| 75 | SYS_SIGRETURN | Restore pre-handler frame |
| 76–77 | SYS_GETPPID / SYS_EXEC | Parent id / replace image |
| 84 | SYS_DUP2 | Redirect an fd (pipelines/redirection) |
| 86 | SYS_SIGACTION | Full sigaction: handler, sa_mask, flags |
| 87 | SYS_SIGPROCMASK | Block/unblock signals |
| 88–92 | SYS_SETPGID / GETPGRP / SETSID / TCSETPGRP / TCGETPGRP | Process groups, sessions, controlling terminal |

---

## Applications

| App | Type | Description |
|---|---|---|
| Terminal | .mct | Terminal emulator: history, tab completion, auto-suggest, scrollback, job control; POSIX builtins `uname`/`env`/`seq`/`head`/... |
| Nano / Notepad | .mct | Text editors with auto-save and dialogs |
| File Explorer | .mct | Browse/open files, CRUD, context menus, file associations |
| SysInfo | .mct | Live RAM/CPU/uptime + **per-core load bars** |
| Task Manager | .mct | Task/window list, kill processes |
| PCI Manager | .mct | Scrollable PCI device table |
| Music Player | .mct | SB16 WAV streaming |
| Volume Control | .mct | Scroll-adjustable system volume |
| Clock | .mct | WIB digital clock |
| Snake / Flappy | .mct | Games in WM windows |
| Mini Browser | .mct | Text-mode web browser via gateway proxy |
| Calculator | .mct | GUI calculator |
| DOOM | kernel | Full 1993 DOOM engine embedded in the kernel |
| ELF Demo | .elf | Proves the ELF32 loader |
| Sync Demo | .elf | Semaphore + futex workers |
| UDP Test | .elf | UDP datagram round-trip |
| Fork Demo | .mct | COW fork + SIGUSR1 handler, exit 42 |
| Exec Demo | .mct | fork → exec different image → exit 7 |
| SHM Demo | .mct | Two processes share a segment |
| mmap Demo | .mct | Sparse 1 MiB mapping, lazy zero-fill |
| mmap File Demo | .mct | File-backed mmap: fault pages in from disk, write back via msync/munmap |
| lseek File Demo | .mct | SYS_LSEEK/SYS_FSTAT/O_APPEND on real files |
| FAT32 Demo | .mct | FAT32 drive 3: read mtools files, create/write/append/delete in `/fat32` |
| Sig Demo | .mct | sigaction/mask/SA_RESTART semantics |
| BG Read | .mct | Background-group reader stopped by SIGTTIN |
| SMP Stress | .mct | 8 children × 2 waves across 4 cores |
| Pipe Gen/Read | .mct | Pipeline producer/consumer |
| TCP Server | .mct | Echo server (host-connectable) |
| Looper | .mct | Infinite loop (Ctrl+C testing) |

---

## Build and Run

### Requirements
`gcc (-m32)` · `nasm` · `make` · `qemu-system-i386` · `python3` + `Pillow` · `grub-mkrescue` + `mtools` · `mkfs.ext2` (for the ext2 drive)

### Commands
```bash
make clean && make        # build kernel + apps + ISO assets
./run.sh                  # KVM boot: 4 cores, gateway proxy, serial -> serial_debug.log
```

`run.sh` boots `mectov.iso` with `-smp 4`, the RTL8139 NIC, SB16 audio, and three drives: `disk.img` (MECTOVFS, index 0), `ext2.img` (index 1) and `fat32.img` (a 16 MB FAT32 volume at index 3 — index 2 is the CD-ROM; created with `mkfs.fat` if missing), streaming the serial log to `serial_debug.log` (also copied to `log.txt`). The Web Gateway Proxy (`scripts/gateway.py`) gives the guest real internet access.

### Building User Applications
```bash
make apps/hello.mct       # builds any apps/<name>.c -> apps/<name>.mct (baked into the VFS)
```
App binaries are produced by `scripts/build_mct.py` (freestanding, entry `_start`, linked at `0x08000000`) and injected into the VFS by the Makefile. Standard ELF32 binaries work too:
```bash
python3 scripts/build_elf.py apps/elfdemo.c elfdemo.elf
```

### Debugging with GDB
```bash
# Terminal 1 — boot with COM2 as a GDB server
qemu-system-i386 -cdrom mectov.iso -m 128 -smp 4 \
  -serial file:serial.log -serial tcp:127.0.0.1:2345,server=on,wait=off \
  -drive file=disk.img,format=raw,index=0,media=disk

# Terminal 2 — attach (press F12 inside the guest to break in)
gdb myos.bin -ex "target remote :2345" -ex "c"
```

---

## Testing

The project is tested on QEMU under **both KVM (4-core)** and TCG:

| Suite | What it verifies |
|---|---|
| `scripts/boot_test.py` | Boot → login → `BOOTED KERNEL LOOP` smoke window |
| `scripts/fork_test.py` | COW fork + waitpid + signals (TCG) |
| `scripts/jobcontrol_test.py` | `sleep 2 &`, `jobs`, `bg`, `fg`, `kill %1` (TCG) |
| `scripts/fat32_test.py` | FAT32 drive 3: mtools-created files, nested dir, create/write/read-back, O_APPEND, delete (TCG) |
| KVM regressions | fork, exec, pipe, redir, mmap, shm, sig, ctrlc, stop, job, bgread, tcp, smpstress (4 cores) |

`kvm_smp_test.py` runs `smpstress` twice (16 children total) and asserts every child exits with the right code and the OS stays alive; `kvm_cpumon_test.py` verifies the per-core load sampler (`idle [0,0,0,0]` → stress `[50,40,46,30]`). The CI workflow (`.github/workflows/build-boot-test.yml`) runs the boot test on every push.

---

## Version History

| Version | Highlights |
|---|---|
| v38.18 | **Build fix: `doom_libc.h` was missing declarations that break the DOOM build on GCC 14+.** The DOOM tree compiles with `-w`, which on older GCC silently swallowed three implicit-declaration *warnings* — but GCC 14 promoted implicit function declarations to hard errors, so `make` died on `doom/i_system.c`, `m_config.c` and `v_video.c` the moment CI's runner upgraded its toolchain. All three functions were already defined in `doom_libc.c`, just never declared in the header: added `double atof(const char*)` and `int system(const char*)` to the stdlib section, and `double fabs(double)` to the math section. Verified: full `doom/*.c` syntax-check under GCC 15 is clean. `OS_VERSION` bumped to 38.18. |
| v38.17 | **Smooth-compositing overhaul: delta-copy VRAM presentation, truthful FPS HUD, tick-scaled 60 FPS cap.** Render profiling (`scripts/render_prof.py`) pinned the desktop's stutter on `swap_buffers()`: it memcpy'd the whole dirty rect to the framebuffer every frame, and the framebuffer is MMIO — under KVM every write is a VM exit, under TCG they're just slow. `swap_buffers()` now does the delta copy the design always intended (`docs/drivers/vga_vbe.md`): it compares the back buffer against the shadow copy of the last presented frame and writes only the *changed runs* to VRAM. Dragging a window changes mostly the leading/trailing strips (the overlap is identical), cutting VRAM traffic ~10x — measured drag render time on TCG dropped ~44 ms → ~10 ms and resize ~127 ms → ~9 ms; on KVM the desktop renders in well under 1 ms/frame. The hardware cursor lives only on VRAM, so the old cursor rect is now force-repainted from the back buffer before drawing the new one (otherwise delta-copy would skip it and leave a ghost cursor). The FPS HUD is now truthful: it counts cursor-only swaps too (previously a moving mouse still read "5 FPS"), converts the elapsed window by the *measured* tick rate (`fps = frames × ticks_per_sec / elapsed_ticks`) instead of assuming 1000 Hz, and on a static desktop it keeps the last reading instead of decaying to the HUD's own 200 ms cadence — and no longer forces a redraw just to refresh itself. The 60 FPS composition cap is scaled the same way: a raw 16-tick window at TCG's drifted 549 ticks/s capped the desktop at ~34 FPS; it's now a real 60 FPS in wall time (`ticks_per_sec × 16 / 1000`). All 7 GUI/regression tests still green (boot, fork, jobcontrol, lseekfile, mmapfile, rusthello, fat32). `OS_VERSION` bumped to 38.17. |
| v38.16 | **FAT32 long file names (LFN) — read AND write.** The v38.15 driver was 8.3-SFN-only: names from Windows/real tools came back truncated (`THEQUI~1.TXT`) and files created by the OS were SFN-only. Directory scanning now reassembles full names from the 0x0F LFN prefix entries — stored in reverse order (the entry farthest from the SFN holds the last chunk and carries the 0x40 chain-start bit), each verified against the FAT checksum of the 8.3 short name before being trusted, so mtools/Windows volumes read correctly regardless of their `~1`-style SFN generation. New files get an LFN chain whenever the name doesn't round-trip through its uppercase 8.3 form (Windows convention — lowercase like `write.txt` gets one too); the SFN uses the `first6~1.EXT` scheme. Lookup/update/remove/rename match by the full reassembled name (falling back to the SFN), and delete marks the whole LFN prefix + SFN run deleted. `apps/fat32demo.mct` now reads mtools-created LFN files (incl. a long-named nested dir `My Vacation Photos/summer2026 beach.txt`), creates a long-named file itself, and the test verifies with host mtools that the OS-written LFN entry is readable **by its full long name** — interop proven both ways. Also fixed two latent bugs surfaced by long paths: the directory scanner used a shared static buffer that recursive subdirectory population clobbered (entries after the first subdirectory were silently skipped — heap-buffered per scan now), and `SYS_OPEN`/`SYS_CREATE_FILE` capped the *whole path* at `MAX_FILENAME` (32) instead of `MAX_PATH` (256), so any path over 32 chars — e.g. `/fat32/demo/long file name test.txt` — was rejected. `OS_VERSION` bumped to 38.16. |
| v38.15 | **FAT32 read/write — mount, list, read, write, create/delete, integrated into the VFS.** Mectov could only use its own MECTOVFS (drive 0) and ext2 (drive 1); now a real FAT32 volume on drive 3 (secondary slave — the CD-ROM owns drive 2) mounts at `/fat32`. New `src/sys/fat32.c` parses the BPB (512-byte sectors, ≤16 sectors/cluster), walks FAT cluster chains, and reads/writes 8.3 SFN directory entries (LFN entries skipped on read; new files are created SFN-only). The ATA driver grew secondary-channel support (`0x170` port base for drives 2–3, master/slave by parity). VFS integration mirrors ext2 exactly: `FS_FAT32_FILE`/`FS_FAT32_DIR` node types dispatch to the driver from `vfs_read_file`/`vfs_write_file`/`vfs_create_node`/`vfs_delete_node`/`vfs_rename`; the first cluster of each object lives in the node's `data_sector`; the mount point is rebuilt from the disk every boot (`vfs_clear_children`) so an external volume swapped at runtime is never shadowed by stale persisted nodes. Because fd writes route through the whole-file `vfs_write_file`, lseek/O_APPEND work on FAT32 files for free. The disk image is a 16 MB FAT32 volume (mkfs.fat + mtools), and `df` gains a `fat32` row via `fat32_get_stats`. `apps/fat32demo.mct` proves it from Ring 3: reads mtools-created files (incl. a nested subdirectory), creates a directory + file, writes, reads back, appends via O_APPEND, and deletes — and the resulting volume is readable by real mtools on the host (verified: mtools lists the OS-created `DEMO` directory). New `scripts/fat32_test.py` runs it end-to-end in CI (drive attached at index=3, TCG). `OS_VERSION` bumped to 38.15. |
| v38.14 | **RTC-calibrated tick rate + CI regression fixes.** The GUI assumed a fixed 1000 Hz PIT tick rate, but under TCG/emulation the virtual clock drifts wildly (measured 549 Hz at boot, ~1.9 kHz under load, faster still on CI runners) — so the desktop double-click window of "800 ticks" was really ~0.1–0.3 s of real time and clicks always missed. `timer_calibrate_ticks_per_sec()` now counts PIT ticks across one full CMOS RTC second at boot, and a rolling per-second update in the main loop keeps `ticks_per_sec` tracking the live rate; the double-click window is computed as `ticks_per_sec × 0.8` — real 0.8 s regardless of clock speed, making double-clicks reliable for real users under slow emulation, not just tests. Also fixed a fresh-disk seeding bug: the VFS had two app-seeding paths and the fresh-disk one silently omitted `forkdemo.mct` (plus `volume.mct`/`mplayer.mct`/`music.wav`), so `run /apps/forkdemo.mct` failed on any new disk — including CI's — while existing disks worked. CI hardened: GUI tests share a `scripts/terminal_launch.py` helper that verifies the cursor is exactly on the Terminal icon via screendump before clicking (with cursor-position reporting on failure), the KVM step guards on `/dev/kvm` availability, and a failed boot dumps the serial tail. Result: the Build & Boot Test workflow is fully green (10/10 steps) for the first time since v38.8. `OS_VERSION` bumped to 38.14. |
| v38.13 | **First Ring 3 application written in Rust — `apps/rusthello.rs`.** Proves the `.mct` format is language-agnostic: a freestanding `no_std`/`no_main` Rust binary (entry `_start`, linked at `0x08000000`) is flattened with objcopy, wrapped in the standard MCT1 header, and runs through the exact same loader path as the C apps — no kernel changes. `scripts/build_rust_mct.py` mirrors `build_mct.py` using rustc targeting the built-in `i686-unknown-uefi` (the only freestanding 32-bit x86 target with a prebuilt core; its C ABI is cdecl; SSE/MMX disabled via `-C target-feature` so no FPU state escapes the kernel, which never saves it). The entry symbol resolves via `nm` (`__start` — that target decorates extern "C" symbols with a leading underscore). CI installs rustup + the UEFI target so the build stays green. `rusthello.mct` prints three markers to serial and exits 0 — verified under QEMU through the Terminal (`run /apps/rusthello.mct`); all regressions (boot/fork/jobcontrol/mmapfile/lseekfile/dhcp) green. `OS_VERSION` bumped to 38.13. |
| v38.12 | **POSIX file positioning & metadata — `SYS_LSEEK` (95) + `SYS_FSTAT` (96) + `O_APPEND`.** File descriptors now track a real read/write offset: `SYS_READ` reads at the descriptor's current position and advances it (POSIX sequential I/O), instead of always starting from byte 0. `SYS_LSEEK` repositions with `SEEK_SET`/`SEEK_CUR`/`SEEK_END` and returns the new offset (negative results and pipes → -1). `SYS_OPEN`'s mode argument is now honored — `O_APPEND` sends every write to the end of the file regardless of the offset. `SYS_FSTAT` fills a `stat_t {size, type, node_idx, parent, data_sector, name}` straight from the fd's VFS node — no path resolution, no rename race. The offset-aware reads reuse `vfs_read_file_offset()` (the unlocked reader built for mmap faults — only `ata_lock`, safe under `fd_lock`); dev/proc nodes keep the legacy whole-read path. `apps/lseekfiledemo.mct` proves it end-to-end: SEEK_SET/END/CUR reads at the right offsets, EOF read = 0, fstat size/type, O_APPEND grows the file from 27→31 bytes at the tail, in-place overwrite via lseek, and lseek-on-pipe/bad-whence → -1. `OS_VERSION` bumped to 38.12. |
| v38.11 | **DHCP client — IP/gateway/DNS configured at runtime over UDP broadcast.** The network config (`my_ip`/`gateway_ip`/DNS server/netmask) was hardcoded to the QEMU slirp layout; now `net_init()` runs a full RFC 2131 client: DISCOVER → OFFER → REQUEST → ACK, all broadcast (`0.0.0.0:68 → 255.255.255.255:67` to Ethernet `FF:FF:FF:FF:FF:FF`) via a new `net_send_udp_raw()` that bypasses the `net_ready` gate (normal UDP/TCP sends wait for the gateway MAC). Pre-bind the RX path accepts broadcast and `0.0.0.0` destinations (the client sets the broadcast flag), and the ACK handler overwrites `my_ip`, `gateway_ip`, `dns_server_ip` (option 6) and a new `netmask_ip` (option 1) in place, then re-resolves the gateway MAC — the ARP reply is what flips `net_ready` and dispatches queued DNS/TCP ops, so the whole stack works unchanged off DHCP. The DNS query path now targets `dns_server_ip` instead of a hardcoded address. The state machine is driven from `net_poll()` inside its cli window (1 s retries × 3, NAK restarts discovery); if no server answers, it falls back to the static defaults after ~4 s — networking comes up exactly as before on serverless links. New `ipconfig` shell command prints the runtime config (IP/netmask/gateway/DNS, DHCP-bound vs static, link state). KVM+slirp-verified: DISCOVER→OFFER→REQUEST→ACK→bound 10.0.2.15 gw=10.0.2.2 dns=10.0.2.3, then `net_ready` via ARP; all -net none regressions (boot/fork/jobcontrol/mmapfile) green. `OS_VERSION` bumped to 38.11. |
| v38.10 | **File-backed mmap — `SYS_MMAP_FILE` (93) + `SYS_MSYNC` (94).** `mmap()` could only reserve anonymous VA ranges; now an open VFS file fd maps into the mmap window and its pages are demand-paged *from the disk* on first access — the `#PF` handler reads each page's sectors straight off the ATA drive via a new unlocked offset-aware reader (`vfs_read_file_offset`, takes only `ata_lock`), because a user fault can fire while the same CPU already holds `vfs_lock` (e.g. `SYS_READ` copying into an mmap'd buffer) and taking it again would self-deadlock. File pages fault in read-only; the first write faults again (RO→RW upgrade), marks the page dirty in a per-region kmalloc'd bitmap, and `SYS_MSYNC`/`SYS_MUNMAP` flush dirty pages back to the file (whole-file RMW bounded by the file's current size — writes past EOF are dropped, POSIX-style, so the mapping never grows the file). File-backed PTEs carry `PAGE_SHARED`, so `fork()` leaves them shared between parent and child — true MAP_SHARED semantics — while each task deep-copies its own dirty bitmap; exec/exit discard mappings without writeback (POSIX-style). The mapping records the VFS node itself, so closing the fd after mmap is legal. `apps/mmapfiledemo.mct` proves it end-to-end: create+seed file → mmap → lazy fault-in from disk → in-place write → msync → re-read shows the change on disk → munmap. `OS_VERSION` bumped to 38.10. |
| v38.9 | **Single-consumer keyboard — SMP input race fixed + real foreground-app key buffer.** The PS/2 scancode ring (`kbd_buffer`) used to have TWO consumers — the main loop (task 0) and `SYS_GET_KEY` — that stole scancodes from each other nondeterministically on 4-core KVM (CI's TCG runs never reproduced it), so keystrokes vanished between the main loop and apps and the terminal sometimes never received typed input at all. Now the main loop is the ONLY reader: it resolves each key with the feed-time modifier snapshot and forwards to the focused window as before, or queues it to a NEW real foreground-app buffer when `term_app_running` (previously `term_app_push_key`/`term_app_pop_key` were stubs returning 0, so interactive apps like `calc`/`echo_test` could never read input). `SYS_GET_KEY` pops that buffer — a background task still gets SIGTTIN, anything else gets 0, and the buffer is cleared on app launch/exit/Ctrl+Z so stale keys never leak. Also made the `[TASK] fork:` marker a single locked write (the COW #PF handler's raw fallback was interleaving bytes into it under real SMP) and added `write_serial_if_free()` for routine per-fault diagnostics so they skip instead of garbling another core's locked line. KVM-verified: `fork_test --kvm` (4-core) passes deterministically 2× — now a mandatory CI regression — plus new `apps/keyshow.mct` proving a Ring 3 app receives typed keys via `SYS_GET_KEY` (`KEY=x` on serial); boot/fork/jobcontrol all green; calc runs interactively in the terminal. |
| v38.8 | **`dmesg` — kernel log ring buffer + `/proc/dmesg` + shell command.** Every byte the kernel writes to the serial port is now captured in a 64 KB in-memory ring (`src/sys/klog.c`) via both serial putc paths, including the exception path (`write_serial_try`), so crash/panic lines are queryable in-guest instead of only on the serial console. Locking is deliberate: writers take no lock (the exception path must be able to log even mid-`klog_snapshot` — the same deadlock `write_serial_try` avoids by try-locking), readers take `klog_lock` with interrupts disabled. Shell command `dmesg` (8 KB heap-buffered chunk, not the 16 KB kernel stack) and `/proc/dmesg` expose the newest `KLOG_SIZE` bytes with tail semantics (a small read shows the most recent activity, not the boot prefix). Bonus VFS fix: new `/proc` nodes previously never appeared on existing disks because the directory already existed and creation was gated on `/proc` missing — now missing nodes are added to an existing `/proc`, so an upgraded disk gains `dmesg` without a rebuild. KVM-verified: `dmesg` prints ~3.5 KB of boot log; `cat /proc/dmesg` shows VFS/tasking/BOOTED markers; ring length tracks serial output exactly over time (probe 5×); crashme's `[CRASH]` line (exception path) is present in the ring buffer; OS alive throughout; fork 6/6, jobcontrol 6/6, boot regressions green. |
| v38.7 | **SIGSEGV delivered to user handlers on unresolvable Ring 3 faults.** A user #PF/#GP/#UD that demand paging / COW cannot resolve used to kill the task immediately; now it routes through the normal signal path — a task that installed a handler (sigaction/sys_signal) gets its sigframe pushed and EIP/USERESP rewritten so the handler runs, and the faulting instruction is re-executed on return (a handler that fixes the cause — or exits — makes progress; otherwise SIGSEGV re-delivers). Default action (no handler; SIG_IGN on a fault is treated as kill) terminates with exit status `128+SIGSEGV` = 139 after full cleanup (WM + VMM + fds), and the frame is parked so a dead task's user context is never iret'd. `apps/segvtest.mct` exercises both modes: NULL dereference without a handler → task ZOMBIE with exit_code=139 (verified by reading kernel memory), with `catch` → handler runs twice (delivery + re-delivery) and exits cleanly. Verified deterministic 2×; fork/jobcontrol/smpstress/boot regressions green. |
| v38.6 | **Lock audit, heap hardening, unified physical allocator, keyboard shift fix.** Global `cli()` in syscall paths replaced with irqsave spinlocks per structure (VFS, fd table, WM windows, IPC queues, task runqueues) so syscalls are preemptible and SMP actually parallelizes; the audit caught two real bugs — `gui_unlock`'s unconditional `sti` inside a held `wm_lock` (deadlock: main loop preempted while holding the lock, terminal spun forever) and a background child inheriting the shell lock then releasing it on exit (blocking every later command). Heap hardening in `mem.c`: every `kmalloc` block gets a magic + canary, `kfree` panics cleanly on non-allocated/double-free, overflow detection via redzone, OOM returns NULL instead of hanging, and allocator stats land in `/proc/meminfo`. Physical allocator unified: the hardcoded 128 MB frame bitmap in `vmm.c` and the dead multiboot bitmap in `mem.c` became one source of truth driven by detected RAM (identity map to 512 MB, framebuffer reserved) — verified with `-m 256`: `MemTotal: 262016 KB`, frames above 128 MB allocated and usable. Keyboard: per-byte modifier snapshot fixes shift loss (shift-7 → `&`) when the whole scancode sequence drains in one IRQ. |
| v38.5 | **COW fork + lazy zero page + demand paging for heap and stack.** One shared zero frame (refcount-pinned) backs every untouched heap/stack page: a read fault maps it read-only (COW marker), the first write duplicates it into a private frame — a process that mallocs a lot but writes little costs almost nothing. User stacks are now demand-paged per task (`[TOP-(tid+2)·64KB, TOP-(tid+1)·64KB)`), with the page below the range left unmapped as a guard page: a genuine stack overflow faults there and kills the task cleanly instead of corrupting memory (verified by deliberately overflowing a 64 KB stack). `apps/demandtest.mct` proves heap zero-fill, write/readback, 100-level recursion growing the stack on demand, and COW fork isolation — no #PF ever leaks to user space; 2× deterministic + full regressions green. |
| v38.4 | **Unified physical allocator (RAM > 128 MB).** The frame bitmap in `vmm.c` was hardcoded to 128 MB while `mem.c` carried a multiboot-derived bitmap that nothing read — RAM above 128 MB was simply unusable. Now one allocator owns the truth: `phys_init(total_pages)` sizes the bitmap from the multiboot memory map, `frame_alloc`/`frame_free`/`zero_phys_page` respect the dynamic ceiling, `phys_reserve_region()` keeps the framebuffer/MMIO out of the free pool, `get_used_memory`/`get_free_memory` report real numbers from the bitmap, and the identity map covers 512 MB. Verified under QEMU `-m 256`: allocator sees `0x0FFE0000`, `MemFree: 212452 KB` (~207 MB free vs ~80 MB with the old cap), and a probe allocated 128 MB of frames past the 128 MB mark, wrote/read a pattern, and freed them all. |
| v38.3 | **Kernel hardening: stack guard pages with clean panic, shell split into modules, stack watermark.** Kernel stacks moved out of `task_t` into a page-aligned arena — a 4 KB unmapped guard page below each 16 KB stack, so an overflow now #PFs on the guard page and panics with a clear message instead of silently corrupting memory. Because an overflow can strike while the CPU is mid-push (the #PF frame itself can't be saved), vector 8 became a hardware task gate with its own TSS/stack/CR3: worst-case overflow prints a full `[PANIC] DOUBLE FAULT` (CPU, EIP, ESP, CR3) and halts instead of triple-faulting into a silent reboot. Exception handlers run on per-CPU fault stacks with nested-fault detection, and logging is deadlock-free (`spin_try_lock` + `write_serial_try`) so a 4-core race can't freeze the panic path. Surfaced and fixed two real bugs: a fork frame offset bug (child could iret with cs=0 → #GP) and a shared fault stack clobbered by simultaneous COW faults. The 3,384-line `src/sys/shell.c` monolith was split into `src/sys/shell/` — dispatcher core + 72 per-command files + `job/` + `script/` — extracted mechanically with token-identical reconstruction and A/B-verified against the old build; all toolkit tests byte-identical to baseline. New stack watermark: the scheduler records each task's peak kernel-stack depth at every preemption, and `/proc/tasks` gains a `STK%` column (peak bytes / 16 KB) — verified live: terminal 1% idle, 16% after a blocking `sleep 2`. `OS_VERSION` bumped to 38.3. |
| v38.2 | **Password stored in /etc/passwd + `passwd` command.** The login password is no longer hardcoded: it lives in `/etc/passwd` (MECTOVFS), with `mectov123` kept only as a fallback for a fresh disk where the file doesn't exist yet. New `src/sys/passwd.c` exposes `sys_get_password`/`sys_set_password` — both the login screen and the shell go through them, so the default lives in exactly one place. Shell builtin `passwd <current> <new>` verifies the current password (against the file or the fallback) before writing, creating `/etc` and the file on first use; it's in tab completion, `help` and `man`. KVM-verified end-to-end across three boots: fallback login on fresh disk, wrong-current-password writes nothing (checked by parsing the raw disk image), change persists across reboot, old password rejected / new one accepted. `OS_VERSION` bumped to 38.2. |
| v38.1 | **Windows-style lock screen + PS/2 init race fix.** Login flow split into two stages like Windows 10: a minimal lock screen with just a large 8× scaled live clock (blinking colon), date, small wordmark and a dismiss hint — any keypress or click dismisses it (the dismissing press is consumed, so it never leaks into the password field; the dismissing click is debounced 300 ms so it can't instantly hit Sign In) — then the existing amber instrument panel for password entry with the clock/date shrunk to the top-left. Clock rendering rewritten: the old `draw_char_scale` advance bug (return value assigned instead of accumulated) stacked every glyph after the first at x=64, rendering the big clock as one digit plus a solid amber blob — fixed with `gx +=`. Also fixes a PS/2 init race: IRQ1/IRQ12 firing mid-protocol could steal the controller's command-byte reply (e.g. `0x41`) into the keyboard buffer as a bogus scancode, instantly dismissing the lock screen at boot — the whole `init_mouse` protocol now runs with interrupts disabled (eflags preserved). KVM-verified: lock screen layout (pixel audit), space-dismiss, click-dismiss, wrong-password stays, correct login → desktop; ABC regression 10/10, toolkit round 3 green. |
| v38.0 | **Amber GUI identity + critical stack-overflow fix.** Window chrome (`TOARU_*` in `theme.h`) and taskbar (`TB_*`/`RETRO_*`, start-logo accent) unified from neutral gray to the amber palette introduced by the login redesign. Fixes a deterministic #PF: v37.9's `sort`/`uniq` had `fbuf[4096]` + `lines[256]` as `run_cmd_internal` stack locals, pushing its frame to 6.3 KB — combined with the `load_mct_app → vmm_create_address_space` chain it overflowed the 16 KB kernel stack when launching any app after using sort. Buffers moved to `kmalloc`/`kfree` (frame back to 2.4 KB); regression suite 10/10, toolkit + chrome KVM tests green. |
| v37.9 | **Shell toolkit round 3:** `printf FORMAT [args]` (`%s`/`%d`/`%x`/`%c`/`%%`, `\n`/`\t` escapes, unsigned `%x` parse, INT_MIN-safe `%d`), `sort [file]` (stdin via pipe/redirect, insertion sort), `uniq [-c] [file]` (consecutive-duplicate collapse with optional counts), `tee FILE` (stdin → file + stdout), `find [dir] [-name GLOB]` (flat-node-table tree walk via parent chains, `*`/`?` glob). Linear-time greedy glob matcher (the recursive backtracking version was exponential on `*a*a*…*b` patterns). All five KVM-verified end-to-end; helper unit tests 24/24 (empty lines, CRLF, pathological glob). |
| v37.8 | **Instrument-console login + VFS layout constants + RTC fix:** graphical login redesigned from the generic glassmorphism template to a machine console — warm charcoal + phosphor amber palette, a live CMOS RTC clock rendered at 2× bitmap scale (the RTC read itself is gated to ≤2/s so the UIP busy-wait can't stall a frame; previously re-read every 16 ms frame), and a real system manifest (core blocks from `smp_cpu_count`, live uptime). Hit-test/render geometry unified under one `LOGIN_PW/PH/PY` define (the click region had drifted from the drawn button); actionable error copy; footer composes from `OS_VERSION` (bumped 37.0 → 37.8). VFS on-disk layout constants moved to `vfs.h` as a single source of truth — shell `df` now counts the 256-sector node table (was hardcoded `65`, under-reporting usage since the 64→256 node migration) and the allocator/read clamps use `VFS_DISK_SECTORS`. winman test app drops its redundant per-window hex echo (the kernel `[WM] create wid=` mirror is authoritative). Doom auto-deps completed (`-MMD -MP` in DOOM_CFLAGS). KVM-verified: login → desktop, wrong-password shake + error copy, pixel audit of clock/wordmark/manifest, ABC regression 10/10, window capacity 5/5. |
| v37.7 | **Exception stubs (no silent reboot), VFS 256 nodes, WM 16 windows, soft-float:** every real fault vector gets a dedicated stub so a user fault (#UD, #GP, #PF…) prints the exception + `int_no` and kills only the faulting task instead of rebooting the kernel (crashme demo app exercises #UD); MECTOVFS node table grows 64 → 256 (on-disk layout v1 → v2, old images rejected and rebuilt from embedded apps); window manager capacity 8 → 16 (desktop apps can now open up to 13 windows); kernel compiled with soft-float (`-msoft-float -mno-80387`) with a software fmod fallback for Doom. |
| v37.6 | **Ctrl+C interrupts the foreground app, Ctrl+Shift+C copies:** POSIX-style SIGINT delivered to the foreground process group from the Ring-3 terminal (interrupts `looper`/`snake`, etc.), while Ctrl+Shift+C keeps the clipboard-copy binding — the two are now distinct. Kernel-side `SYS_EXIT_GROUP`-style signal dispatch + terminal event plumbing. |
| v37.5 | **Shell toolkit round 2:** `wc` (words/lines/bytes), `type` (builtin / alias / external classification), `cd -` (OLDPWD), `cat -n` (line numbers), `yes [string]` — all shell builtins with pipe support, KVM-verified. |
| v37.4 | **DOOM silent by default:** SB16 DMA/IRQ activity while Doom streams audio stalled the game loop (window freeze), so sound is now opt-in (`doom -sound`); without it the SB16 module is never touched and Doom runs clean. Also fixed the broken doom_libc `vsnprintf` (integer overflow). run.sh gains audio backend auto-fallback (pipewire > pulseaudio) and drops the flaky pcspk audio device. |
| v37.3 | **Doom freeze diagnostics:** serial heartbeat (`[DOOM] tick`) + on-screen frame counter so a frozen window can be told apart from a dead OS; `doom -nosound` escape hatch; traced the freeze to SB16 DMA/IRQ during streaming (→ fixed properly in v37.4). |
| v37.2 | **Doom sound attempt:** enabled `FEATURE_SOUND`, unmasked SB16 IRQ5, wired the DSP IRQ path — exposed the DMA/IRQ freeze that v37.3 diagnosed and v37.4 fixed. |
| v37.1 | **Shell scripting:** `for VAR in list` + `while true/false` loops with `$VAR` expansion, `break`, nested loops (4 levels), all state on the script interpreter's own stack (re-entrancy fix: nested `run` inside a loop no longer clobbers loop state). KVM-verified step by step. |
| v37.0 | **Virtual /proc filesystem:** `/proc/tasks`, `/proc/meminfo`, `/proc/cpuinfo`, `/proc/uptime`, `/proc/version` — generated on read from live kernel state via `FS_PROC` nodes; KVM-verified each file. |
| v36.9 | **Shell standard toolkit:** six POSIX-style builtins — `uname` (with `-a` kernel banner), `whoami`, `hostname`, `env` (lists exported vars), `seq [FIRST] LAST`, `head [-n N] [file\|stdin]` with a serial mirror so truncation/pipes are testable; single `OS_VERSION` constant shared by help/mfetch/uname (was stale at v35.2); env/alias tables lazy-init in the Ring-3 terminal path under one shared module-level guard (defaults like `$USER` previously missing there); `head -n` dispatch, tab completion, help and `man` pages updated. KVM-verified: all six commands, `-n` truncation, and `cat f \| head -n 1` pipes. |
| v36.8 | **Per-CPU load monitor:** scheduler samples each core's utilization every tick (50 ms window) and exposes it via `SYS_GET_SYSINFO` (`cpu_count`, `cpu_load[4]`); SysInfo renders four live per-core bars; serial `[LOAD]` line every 3 s; SysInfo sleeps between frames instead of spinning. |
| v36.7 | **Per-CPU SMP scheduler:** per-core runqueues with priority+aging pick and work-stealing migration; pinned Ring 0 idle task per AP; LAPIC timer per AP (PIT-calibrated once on the BSP — concurrent calibration caused timer storms); one cli-first `task_lock` for every runqueue mutation; locked signal delivery on the syscall-return path; SMP-safe serial (locked buffer writes). Fixed along the way: missing `spin_unlock` in `task_signal` (froze OS at `waitpid`), phantom-CPU scan in `rq_least_loaded`, cross-CPU page-dir free race. `smpstress` passes 16/16 children on 4 cores. |
| v36.6 | **Process groups, sessions & controlling terminal:** `SYS_SETPGID/GETPGRP/SETSID/TCSETPGRP/TCGETPGRP` (88–92); per-task `pgrp`/`session`; the terminal becomes session leader and claims the foreground group; `SYS_GET_KEY` delivers SIGTTIN to background readers (bgread demo); Ctrl+C/Ctrl+Z target the foreground process group. |
| v36.5 | **Signal hardening:** `SYS_SIGACTION`/`SYS_SIGPROCMASK` (86–87); blocked signals stay pending, SIGKILL/SIGSTOP/SIGCONT bypass the mask; handler delivery auto-blocks per `sa_mask` (unless SA_NODEFER); `SA_RESTART` re-parks interrupted sleeps; fork copies masks, exec preserves them. |
| v36.4 | **Real pipelines, app redirection & TCP server:** `SYS_DUP2` (84) + per-task fd inheritance; `task_fork_exec` spawns children directly into apps with rewired stdin/stdout; `run A \| run B`, `run A > file`, `< file`; job control completed (`jobs`/`bg`/`fg`/SIGTSTP); `SYS_TCP_LISTEN` (85) + echo server verified end-to-end. |
| v36.3 | **mmap + Ctrl+C:** `SYS_MMAP/MUNMAP` (82–83) demand paging; Ctrl+C sends SIGINT to the whole foreground job tree. |
| v36.2 | **exec, shm & trap-gate syscalls:** `SYS_EXEC` (77) via live-frame patching; System V shared memory (78–81); `int 0x80` switched to a trap gate (preemptible syscalls). |
| v36.1 | **KVM fork-GPF fix:** child frame copied from `TSS.esp0 - sizeof(registers_t)` instead of the possibly-stale saved ESP. |
| v35.5 | **Multi-connection TCP:** 8-slot conn table, per-conn seq/ack, retransmit/timeout sweeps, FIN handshake; `SYS_TCP_CLOSE` (70); RTL8139 SMP TX/RX race fixed with a spinlock. |
| v35.4 | **Security hardening:** ELF loader integer-wrap fix, script-recursion guard, VFS node sanitization, ext2 superblock hardening, fd lifecycle fix, precise `task_sleep()`, GUI/clipboard bounds. |
| v35.3 | **English UI localization:** shell commands, help, apps translated; legacy Indonesian aliases kept. |
| v35.2 | **Shell file management:** `cp`, `mv`, `rmdir`, `df`; ext2 unlink/delete consistency fixes. |
| v35.1 | **Ext2 write support:** full on-disk write layer on the secondary partition, verified with `fsck.ext2`. |
| v35.0 | **SMP, GDB, ELF & sync:** fixed ACPI/MADT parsing (all 4 cores boot), in-kernel GDB stub, ELF32 loader, semaphores/futexes, IRQ-driven networking + UDP API, CI boot test. |
| v34.x | Scheduler aging overhaul + VFS reclamation; memory/syscall hardening (kmalloc overflow checks, fd cap); kernel-ownership teardown cleanup; real clip-rectangle enforcement, single-owner minimize/restore, unified `ps2_drain()`, kill-free shell waits. |
| v33.x | Start-menu ghosting fix; multi-core SMP bring-up (INIT-SIPI-SIPI, per-core GDT/TSS/IDT, LAPIC/IOAPIC, MADT ISO routing). |
| v32.0 | Syscall modularization; heap/VMM memory-layout safety fixes. |
| v31.0 | Event-driven rendering (idle CPU 0%), 64-bit delta compositing, `SYS_UPDATE_WINDOW` redraw triggers. |
| v30.x | Alt+Tab switcher; English localization; kernel clipboard + Explorer CRUD + context menus. |
| v29.0 | File associations + Explorer double-click; shell argument parsing for `run`. |
| v28.0 | DOOM memory protection (heap at 24 MB), HUD font precision, fullscreen-reset crash fix. |
| v27.x | Per-task working directories, absolute-path launchers, `uptime`/`memstat`, path sanitization, notepad shortcuts, heap/VMM separation fixes. |
| v27.0–v26.0 | TCP redirection through the web gateway; COW paging; integrated nano editor. |
| v25.x | Scripts + env vars + aliasing; IntelliMouse scroll; SB16 audio; shared-library runtime; Gitea remote. |
| v24.0 | **DOOM engine port** (full game in the kernel). |
| v23.0 | Shadow framebuffer delta rendering, zombie auto-kill, Ctrl+C, snake rewrite, power-menu fixes. |
| v22.0 | VMM (per-process address spaces), IPC queues, 4-level priority scheduler, 14 new syscalls. |
| v21.0 | Premium UI: hi-res cursor, WiFi indicator, 60 Hz loop. |
| v20.0–v18.0 | Squircle icons + macOS buttons; glass-morphism UI; `.mct` app ecosystem + Ring 3 syscalls. |
| v17.0 | Terminus Bold font, draggable icons, VFS persistence. |

---

## License

GNU General Public License v2.0 (GPLv2). Created by **M Alif Fadlan**.

Kamu bebas menggunakan, memodifikasi, dan mendistribusikan project ini selama turunan yang kamu distribusikan juga dilisensikan GPLv2 dan menyertakan source code-nya. Karena kernel ini terhubung langsung dengan source code DOOM (juga GPLv2), seluruh work gabungan dilisensikan GPLv2.
