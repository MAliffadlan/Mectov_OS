# Mectov OS v15.0

A bare-metal x86 operating system kernel written from scratch in C and Assembly. No external libraries, no libc, no POSIX -- every byte runs directly on hardware.

## About

Mectov OS is a hobby operating system designed as a learning project and technical showcase. It boots via GRUB Multiboot, sets up protected mode with paging, and provides a fully graphical desktop environment with floating windows, mouse input, persistent file storage, and hardware detection.

Created by M Alif Fadlan.

---

## Architecture

```
+---------------------------------------------------------+
|                    GRUB Multiboot                       |
+---------------------------------------------------------+
|  boot.asm  -->  kernel.c  (kernel_main entry point)     |
+---------------------------------------------------------+
|  GDT  |  IDT  |  PIT Timer  |  Keyboard  |  Mouse      |
+---------------------------------------------------------+
|  Memory Manager    |  Task Scheduler  |  VFS + ATA PIO  |
+---------------------------------------------------------+
|  VGA/VESA Driver   |  Window Manager  |  PCI Scanner    |
+---------------------------------------------------------+
|  Desktop  |  Taskbar  |  Start Menu  |  Applications    |
+---------------------------------------------------------+
```

### Target Platform
- CPU: Intel x86 (i386), Protected Mode, Ring 0
- Display: VESA VBE Linear Framebuffer, 32-bit color
- Storage: ATA PIO (IDE), 1MB virtual disk
- Audio: PC Speaker via PIT Channel 2
- Bus: PCI Configuration Space (ports 0xCF8/0xCFC)

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
- Double-click to launch applications
- Scrolling marquee banner at the top

### 5. Taskbar (src/gui/taskbar.c)
- "Mectov" start menu button with application launcher
- Window buttons showing all open apps (minimized windows shown dimmed)
- Click behavior: restore minimized, minimize focused, raise unfocused
- System tray with:
  - CAPS indicator (orange when active)
  - HDD activity LED (red flash on disk I/O)
  - RAM usage bar (live percentage)
  - Digital clock (WIB / UTC+7)

### 6. PCI Bus Scanner (src/drivers/pci.c)
- Full enumeration: 256 buses, 32 slots, 8 functions per slot
- Vendor database: Intel, AMD, NVIDIA, QEMU/Bochs, Red Hat, VMware, VirtualBox, Realtek, Broadcom
- Device class identification: VGA, IDE, SATA, NVMe, Ethernet, USB, Audio, Host Bridge, ISA Bridge, and more
- Results exposed via GUI (PCI Device Manager) and CLI (`lspci`)

### 7. Persistent File System (src/sys/vfs.c)
- Simple Virtual File System with auto-save to disk.img via ATA PIO
- File operations: create, read, write, delete
- GUI integration: File Explorer for browsing, Nano Editor for editing

### 8. FPS Counter (kernel.c)
- Real-time frames-per-second display at top-right corner
- Green text on black background, updated every second
- Rendering throttled to 30 FPS (every 2 ticks at 60Hz PIT)
- Input polling runs at full 60Hz for responsive mouse and keyboard

### 9. Boot and Security
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
| System Info     | src/apps/sysinfo_app.c  | Live RAM, CPU, resolution, and uptime monitor     |
| PCI Manager     | src/apps/pci_app.c      | Scrollable table of detected PCI hardware         |
| Clock           | src/apps/clock_app.c    | Digital clock with WIB timezone                   |
| Snake           | src/apps/snake.c        | Classic snake game                                |
| Power Options   | src/apps/power_app.c    | Shut Down, Restart, and Lock Screen dialog        |

---

## Terminal Commands

```
help          Show all available commands
mfetch        System fetch display
mem           RAM usage statistics
waktu         Hardware RTC time (WIB)
ls            List all files in VFS
buat [file]   Create a new file
baca [file]   Read file contents
edit [file]   Open file in Nano Editor
hapus [file]  Delete a file
lspci         List all detected PCI devices
clear         Clear terminal buffer
matikan       ACPI soft-off shutdown
mulaiulang    Triple-fault CPU reset
kunci         Lock screen
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
  src/
    drivers/
      ata.c             ATA PIO disk driver
      font8x16.c        Built-in 8x16 bitmap font
      keyboard.c        PS/2 keyboard with scancode translation
      mouse.c           PS/2 mouse with interrupt-driven input
      pci.c             PCI configuration space bus scanner
      speaker.c         PC Speaker tone generation
      timer.c           PIT Channel 0 (60Hz system tick)
      vga.c             VESA VBE framebuffer, drawing primitives, double buffer
    sys/
      gdt.c             Global Descriptor Table setup
      idt.c             Interrupt Descriptor Table and ISR registration
      interrupt_entry.asm  ISR/IRQ assembly stubs
      mem.c             Paging, kmalloc/kfree heap allocator
      security.c        Password verification
      shell.c           Command parser and built-in commands
      task.c            Preemptive multitasking scheduler
      utils.c           String utilities, memset, memcpy, BCD conversion
      vfs.c             Virtual file system with ATA persistence
    gui/
      desktop.c         Desktop rendering, gradient wallpaper, icons
      login.c           Login screen with password input
      taskbar.c         Taskbar, start menu, system tray
      wm.c              Window manager (open, close, minimize, maximize, drag)
    apps/
      clock_app.c       Digital clock application
      explorer_app.c    File explorer GUI
      nano.c            Text editor
      pci_app.c         PCI device manager
      power_app.c       Power options dialog
      snake.c           Snake game
      sysinfo_app.c     System information monitor
      terminal_app.c    Terminal emulator
    include/
      (header files for all modules)
```

---

## Build and Run

### Requirements
- `i386-elf-gcc` (cross-compiler for 32-bit x86)
- `nasm` (assembler)
- `make`
- `qemu-system-i386`

### Commands
```bash
# Clean and build
make clean && make

# Run in QEMU
./run.sh
```

### Default Login
```
Password: mectov123
```

---

## Version History

| Version | Highlights                                                                    |
|---------|-------------------------------------------------------------------------------|
| v15.0   | Gradient wallpaper, procedural icons, window minimize/maximize, FPS counter, heap coalescing, 16KB task stacks |
| v14.0   | Linked-list heap allocator with kfree, PCI bus scanner, PCI Manager app, Power Options dialog |
| v13.0   | File Explorer GUI, Start Menu with Power entry                                |
| v12.0   | Double-buffered rendering, mouse-driven window manager                        |
| v11.0   | ATA PIO persistence, Nano Editor, VFS auto-save                              |
| v10.0   | Preemptive multitasking, context switching                                    |

---

## Technical Notes

- All code runs in Ring 0 (kernel mode). There is no user-mode separation yet.
- The heap allocator uses a linked-list with forward and backward coalescing on kfree to prevent fragmentation.
- PCI detection is scan-only; no active driver loading for detected devices (planned for future versions).
- The gradient wallpaper is computed once at boot and cached in heap memory for zero per-frame rendering cost.
- FPS is measured by counting rendered frames between 60-tick (1 second) intervals.

---

## License

MIT License

Created by M Alif Fadlan.
