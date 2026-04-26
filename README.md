Mectov OS
=========

Mectov OS is a custom, bare-metal monolithic operating system kernel written from scratch in C and x86 Assembly. It is designed as an educational experiment in low-level system architecture, hardware interaction, and kernel development.

What is Mectov OS?
------------------
Mectov OS bypasses standard libraries (freestanding C) and interacts directly with PC hardware. It features a custom boot sequence, memory-mapped I/O, and direct port communication. 

Key features include:
- **Custom Window Manager (TUI):** A Linux-style TTY interface with a floating window manager illusion over a desktop background.
- **Virtual File System (VFS):** A RAM-based file system supporting file creation, reading, writing, and deletion.
- **Hardware Drivers:**
  - PS/2 Keyboard Driver with state tracking (Shift, Caps Lock).
  - PC Speaker Audio Driver (PIT-based).
  - Real-Time Clock (RTC/CMOS) Reader.
- **Built-in Shell:** An interactive command prompt featuring commands like `ls`, `buat`, `tulis`, `baca`, `hapus`, `waktu`, `warna`, and `beep`.

Building and Running
--------------------
To compile and run Mectov OS, you need a Linux environment with `gcc`, `nasm`, and `qemu` installed.

Dependencies:
    sudo apt update
    sudo apt install build-essential nasm qemu-system-x86 gcc-multilib

Build instructions:
    # 1. Compile the 32-bit Assembly Bootloader
    nasm -f elf32 boot.asm -o boot.o

    # 2. Compile the Freestanding C Kernel
    gcc -m32 -c kernel.c -o kernel.o -std=gnu99 -ffreestanding -O2 -Wall -Wextra

    # 3. Link objects into the final OS binary
    ld -m elf_i386 -T linker.ld boot.o kernel.o -o myos.bin

Running in QEMU:
    qemu-system-i386 -kernel myos.bin

Documentation
-------------
Since this is an experimental hobby OS, standard POSIX functions are not available. Functions such as `print`, `strcmp`, `atoi`, and hardware I/O (`inb`, `outb`) are implemented natively within `kernel.c`.

License
-------
Mectov OS is open-source software. You are free to modify, distribute, and use it for educational purposes.