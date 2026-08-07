CC = gcc
AS = nasm
LD = ld

CFLAGS = -m32 -std=gnu99 -ffreestanding -O2 -Wall -Wextra -g
# -g keeps DWARF debug info in myos.bin so GDB can resolve kernel symbols
# (break kernel_main, bt, list, etc.) when debugging via the in-kernel stub.
LDFLAGS = -m elf_i386 -T linker.ld
ASFLAGS = -f elf32

# DOOM compile flags: redirect standard headers to our mini libc
DOOM_CFLAGS = -m32 -std=gnu99 -ffreestanding -O2 \
              -isystem doom/include_override -Idoom \
              -fno-builtin \
              -DDOOMGENERIC_RESX=1024 -DDOOMGENERIC_RESY=768 \
              -w

SRC_DIR = src
OBJ_DIR = obj

# List of source files
SRCS = $(wildcard $(SRC_DIR)/drivers/*.c) \
       $(wildcard $(SRC_DIR)/sys/*.c) \
       $(wildcard $(SRC_DIR)/apps/*.c) \
       $(wildcard $(SRC_DIR)/gui/*.c) \
       kernel.c

# DOOM source files (all .c in doom/ directory)
DOOM_SRCS = $(wildcard doom/*.c)
DOOM_OBJS = $(DOOM_SRCS:doom/%.c=$(OBJ_DIR)/doom/%.o)

OBJS = $(OBJ_DIR)/src/sys/interrupt_entry.o \
       $(OBJ_DIR)/src/sys/smp_trampoline.o \
       $(SRCS:%.c=$(OBJ_DIR)/%.o) \
       $(OBJ_DIR)/boot.o \
       $(OBJ_DIR)/wallpaper.o \
       $(OBJ_DIR)/gcalc_mct.o \
       $(OBJ_DIR)/hello_mct.o \
       $(OBJ_DIR)/clock_mct.o \
       $(OBJ_DIR)/snake_mct.o \
       $(OBJ_DIR)/sysinfo_mct.o \
       $(OBJ_DIR)/pci_mct.o \
       $(OBJ_DIR)/explorer_mct.o \
       $(OBJ_DIR)/browser_mct.o \
       $(OBJ_DIR)/terminal_mct.o \
       $(OBJ_DIR)/taskmgr_mct.o \
       $(OBJ_DIR)/notepad_mct.o \
       $(OBJ_DIR)/flappy_mct.o \
       $(OBJ_DIR)/forkdemo_mct.o \
       $(OBJ_DIR)/execdemo_mct.o \
       $(OBJ_DIR)/execchild_mct.o \
       $(OBJ_DIR)/tcpserver_mct.o \
       $(OBJ_DIR)/shmdemo_mct.o \
       $(OBJ_DIR)/mmapdemo_mct.o \
       $(OBJ_DIR)/looper_mct.o \
       $(OBJ_DIR)/pipegen_mct.o \
       $(OBJ_DIR)/piperead_mct.o \
       $(OBJ_DIR)/sigdemo_mct.o \
       $(OBJ_DIR)/bgread_mct.o \
       $(OBJ_DIR)/libc_mct.o \
       $(OBJ_DIR)/calc_mct.o \
       $(OBJ_DIR)/volume_mct.o \
       $(OBJ_DIR)/mplayer_mct.o \
       $(OBJ_DIR)/music_wav.o \
       $(OBJ_DIR)/elfdemo_elf.o \
       $(OBJ_DIR)/syncdemo_elf.o \
       $(OBJ_DIR)/udptest_elf.o \
       $(DOOM_OBJS) \
       $(OBJ_DIR)/doom1_wad.o

all: $(OBJ_DIR) myos.bin

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)
	mkdir -p $(OBJ_DIR)/src/drivers
	mkdir -p $(OBJ_DIR)/src/sys
	mkdir -p $(OBJ_DIR)/src/apps
	mkdir -p $(OBJ_DIR)/src/gui
	mkdir -p $(OBJ_DIR)/doom

$(OBJ_DIR)/boot.o: boot.asm | $(OBJ_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(OBJ_DIR)/src/sys/interrupt_entry.o: src/sys/interrupt_entry.asm | $(OBJ_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(OBJ_DIR)/src/sys/smp_trampoline.bin: src/sys/smp_trampoline.asm | $(OBJ_DIR)
	nasm -f bin $< -o $@

$(OBJ_DIR)/src/sys/smp_trampoline.o: $(OBJ_DIR)/src/sys/smp_trampoline.bin
	objcopy -I binary -O elf32-i386 -B i386 $< $@

$(OBJ_DIR)/gcalc_mct.o: gcalc.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 $< $@

$(OBJ_DIR)/hello_mct.o: hello.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 $< $@

MCT_LIBC_H = apps/lib/libc.h

gcalc.mct: apps/gcalc.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/gcalc.c gcalc.mct

hello.mct: apps/hello.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/hello.c hello.mct

clock.mct: apps/clock.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/clock.c clock.mct

snake.mct: apps/snake.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/snake.c snake.mct

sysinfo.mct: apps/sysinfo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/sysinfo.c sysinfo.mct

explorer.mct: apps/explorer.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/explorer.c explorer.mct

pci.mct: apps/pci.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/pci.c pci.mct

browser.mct: apps/browser.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/browser.c browser.mct

terminal.mct: apps/terminal.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/terminal.c terminal.mct

taskmgr.mct: apps/taskmgr.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/taskmgr.c taskmgr.mct

notepad.mct: apps/notepad.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/notepad.c notepad.mct

flappy.mct: apps/flappy.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/flappy.c flappy.mct

libc.mct: apps/lib/libc.c $(MCT_LIBC_H)
	python3 scripts/build_lib.py apps/lib/libc.c libc.mct

calc.mct: apps/calc.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/calc.c calc.mct

volume.mct: apps/volume.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/volume.c volume.mct

mplayer.mct: apps/mplayer.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/mplayer.c mplayer.mct

forkdemo.mct: apps/forkdemo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/forkdemo.c forkdemo.mct

execdemo.mct: apps/execdemo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/execdemo.c execdemo.mct

execchild.mct: apps/execchild.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/execchild.c execchild.mct

shmdemo.mct: apps/shmdemo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/shmdemo.c shmdemo.mct

mmapdemo.mct: apps/mmapdemo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/mmapdemo.c mmapdemo.mct

looper.mct: apps/looper.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/looper.c looper.mct

pipegen.mct: apps/pipegen.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/pipegen.c pipegen.mct

piperead.mct: apps/piperead.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/piperead.c piperead.mct

sigdemo.mct: apps/sigdemo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/sigdemo.c sigdemo.mct

bgread.mct: apps/bgread.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/bgread.c bgread.mct

tcpserver.mct: apps/tcpserver.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/tcpserver.c tcpserver.mct

$(OBJ_DIR)/clock_mct.o: clock.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 $< $@

$(OBJ_DIR)/snake_mct.o: snake.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 $< $@

$(OBJ_DIR)/sysinfo_mct.o: sysinfo.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 $< $@

$(OBJ_DIR)/pci_mct.o: pci.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 $< $@

$(OBJ_DIR)/explorer_mct.o: explorer.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 $< $@

$(OBJ_DIR)/browser_mct.o: browser.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 $< $@

$(OBJ_DIR)/terminal_mct.o: terminal.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 $< $@

$(OBJ_DIR)/taskmgr_mct.o: taskmgr.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 taskmgr.mct $(OBJ_DIR)/taskmgr_mct.o

$(OBJ_DIR)/notepad_mct.o: notepad.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 notepad.mct $(OBJ_DIR)/notepad_mct.o

$(OBJ_DIR)/flappy_mct.o: flappy.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 flappy.mct $(OBJ_DIR)/flappy_mct.o

$(OBJ_DIR)/libc_mct.o: libc.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 libc.mct $(OBJ_DIR)/libc_mct.o

$(OBJ_DIR)/calc_mct.o: calc.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 calc.mct $(OBJ_DIR)/calc_mct.o

$(OBJ_DIR)/volume_mct.o: volume.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 volume.mct $(OBJ_DIR)/volume_mct.o

$(OBJ_DIR)/mplayer_mct.o: mplayer.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 mplayer.mct $(OBJ_DIR)/mplayer_mct.o

$(OBJ_DIR)/forkdemo_mct.o: forkdemo.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 forkdemo.mct $(OBJ_DIR)/forkdemo_mct.o

$(OBJ_DIR)/execdemo_mct.o: execdemo.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 execdemo.mct $(OBJ_DIR)/execdemo_mct.o

$(OBJ_DIR)/execchild_mct.o: execchild.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 execchild.mct $(OBJ_DIR)/execchild_mct.o

$(OBJ_DIR)/tcpserver_mct.o: tcpserver.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 tcpserver.mct $(OBJ_DIR)/tcpserver_mct.o

$(OBJ_DIR)/shmdemo_mct.o: shmdemo.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 shmdemo.mct $(OBJ_DIR)/shmdemo_mct.o

$(OBJ_DIR)/mmapdemo_mct.o: mmapdemo.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 mmapdemo.mct $(OBJ_DIR)/mmapdemo_mct.o

$(OBJ_DIR)/looper_mct.o: looper.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 looper.mct $(OBJ_DIR)/looper_mct.o

$(OBJ_DIR)/pipegen_mct.o: pipegen.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 pipegen.mct $(OBJ_DIR)/pipegen_mct.o

$(OBJ_DIR)/piperead_mct.o: piperead.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 piperead.mct $(OBJ_DIR)/piperead_mct.o

$(OBJ_DIR)/sigdemo_mct.o: sigdemo.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 sigdemo.mct $(OBJ_DIR)/sigdemo_mct.o

$(OBJ_DIR)/bgread_mct.o: bgread.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 bgread.mct $(OBJ_DIR)/bgread_mct.o

$(OBJ_DIR)/music_wav.o: apps/music.wav | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 apps/music.wav $(OBJ_DIR)/music_wav.o

# ELF demo app: built as a real ELF32 ET_EXEC and embedded for VFS injection
elfdemo.elf: apps/elfdemo.c
	python3 scripts/build_elf.py apps/elfdemo.c elfdemo.elf

$(OBJ_DIR)/elfdemo_elf.o: elfdemo.elf | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 elfdemo.elf $(OBJ_DIR)/elfdemo_elf.o

# Sync demo (semaphore + futex test, built as ELF)
syncdemo.elf: apps/syncdemo.c
	python3 scripts/build_elf.py apps/syncdemo.c syncdemo.elf

$(OBJ_DIR)/syncdemo_elf.o: syncdemo.elf | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 syncdemo.elf $(OBJ_DIR)/syncdemo_elf.o

# UDP test app (validates the UDP syscall API)
udptest.elf: apps/udptest.c
	python3 scripts/build_elf.py apps/udptest.c udptest.elf

$(OBJ_DIR)/udptest_elf.o: udptest.elf | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 udptest.elf $(OBJ_DIR)/udptest_elf.o

$(OBJ_DIR)/wallpaper.o: $(OBJ_DIR)/wallpaper.bin | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 $< $@

$(OBJ_DIR)/wallpaper.bin: assets/wallpaper.png
	python3 scripts/build_wallpaper.py assets/wallpaper.png $@

# DOOM WAD file embedded as object
$(OBJ_DIR)/doom1_wad.o: doom1.wad | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 doom1.wad $(OBJ_DIR)/doom1_wad.o

# DOOM source compilation rule
$(OBJ_DIR)/doom/%.o: doom/%.c | $(OBJ_DIR)
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

# Kernel source compilation rule
$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

myos.bin: $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o myos.bin

clean:
	rm -rf $(OBJ_DIR) myos.bin

clean_all: clean
	rm -f *.mct *.elf

.PHONY: all clean clean_all
