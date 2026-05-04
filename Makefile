CC = gcc
AS = nasm
LD = ld

CFLAGS = -m32 -std=gnu99 -ffreestanding -O2 -Wall -Wextra
LDFLAGS = -m elf_i386 -T linker.ld
ASFLAGS = -f elf32

# DOOM compile flags: redirect standard headers to our mini libc
DOOM_CFLAGS = -m32 -std=gnu99 -ffreestanding -O2 \
              -isystem doom/include_override -Idoom \
              -fno-builtin \
              -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
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
       $(OBJ_DIR)/edit_mct.o \
       $(OBJ_DIR)/flappy_mct.o \
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

$(OBJ_DIR)/gcalc_mct.o: gcalc.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 $< $@

$(OBJ_DIR)/hello_mct.o: hello.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 $< $@

clock.mct: clock.c
	python3 build_mct.py clock.c clock.mct

snake.mct: snake_ring3.c
	python3 build_mct.py snake_ring3.c snake.mct

sysinfo.mct: sysinfo.c
	python3 build_mct.py sysinfo.c sysinfo.mct

explorer.mct: explorer.c
	python3 build_mct.py explorer.c explorer.mct

pci.mct: pci.c
	python3 build_mct.py pci.c pci.mct

browser.mct: browser.c
	python3 build_mct.py browser.c browser.mct

terminal.mct: terminal.c
	python3 build_mct.py terminal.c terminal.mct

taskmgr.mct: src/apps/taskmgr_app.c
	python3 build_mct.py src/apps/taskmgr_app.c taskmgr.mct

edit.mct: edit.c
	python3 build_mct.py edit.c edit.mct

flappy.mct: flappy_ring3.c
	python3 build_mct.py flappy_ring3.c flappy.mct

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

$(OBJ_DIR)/edit_mct.o: edit.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 edit.mct $(OBJ_DIR)/edit_mct.o

$(OBJ_DIR)/flappy_mct.o: flappy.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 flappy.mct $(OBJ_DIR)/flappy_mct.o

$(OBJ_DIR)/wallpaper.o: $(OBJ_DIR)/wallpaper.bin | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 $< $@

$(OBJ_DIR)/wallpaper.bin:
	python3 scratch/build_wallpaper.py /home/mectov/.gemini/antigravity/brain/1701ced2-e485-4124-afac-9fa8400a902f/media__1777482843135.png $@

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

.PHONY: all clean
