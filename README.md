Mectov OS v7.0 (Security & Power Edition)
========================================

Mectov OS is a custom, bare-metal monolithic operating system kernel written from scratch in C and x86 Assembly. Version 7.0 introduces security features, power management, and persistent storage.

New Features in v7.0
--------------------
- **Secure Boot Login:** Protected by password authentication with masked input (`*`).
- **Power Management:** Native support for `matikan` (ACPI Shutdown) and `mulaiulang` (PS/2 CPU Reset).
- **Boot Animation:** Professional kernel loading animation and splash screen.
- **MectovScript v2.0:** Improved execution speed and emergency abort (ESC key).

System Commands
---------------
### 🛠 System & Power
- `mfetch`      : Show system info and ASCII art.
- `waktu`       : Read hardware clock (RTC/CMOS).
- `warna`       : Toggle terminal color themes.
- `matikan`     : Power off the machine (ACPI).
- `mulaiulang`  : Reboot the system.
- `clear`       : Clear terminal workspace.
- `beep`        : Test PC Speaker.

### 📂 File System (Persistent ATA)
- `ls`          : List all files on the virtual HDD.
- `buat [name]` : Create a new empty file.
- `edit [name]` : Open Mectov Nano text editor.
- `baca [name]` : Print file content to terminal.
- `hapus [name]`: Delete a file permanently.
- `tulis [name] [text]`: Write text to a file via CLI.

### 📜 Scripting (MectovScript)
- `echo [text]` : Print text.
- `tunggu [ms]` : Pause execution.
- `nada [Hz] [ms]`: Play a specific tone.
- `jalankan [file]`: Run a batch script file.

Building and Running
--------------------
Requirements: `gcc`, `nasm`, `qemu`, `gcc-multilib`.

Build:
    nasm -f elf32 boot.asm -o boot.o
    gcc -m32 -c kernel.c -o kernel.o -std=gnu99 -ffreestanding -O2 -Wall -Wextra
    ld -m elf_i386 -T linker.ld boot.o kernel.o -o myos.bin

Run (with Full Features):
    qemu-system-i386 -kernel myos.bin -audiodev alsa,id=snd0 -machine pcspk-audiodev=snd0 -drive file=disk.img,format=raw,index=0,media=disk

*Note: Default Password is `mectov123`*

License
-------
Open-source educational software. Created by Alif Fadlan.