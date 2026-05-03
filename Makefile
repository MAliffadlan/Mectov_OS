CC = gcc
AS = nasm
LD = ld

CFLAGS = -m32 -std=gnu99 -ffreestanding -O2 -Wall -Wextra
LDFLAGS = -m elf_i386 -T linker.ld
ASFLAGS = -f elf32

SRC_DIR = src
OBJ_DIR = obj

# List of source files
SRCS = $(wildcard $(SRC_DIR)/drivers/*.c) \
       $(wildcard $(SRC_DIR)/sys/*.c) \
       $(wildcard $(SRC_DIR)/apps/*.c) \
       $(wildcard $(SRC_DIR)/gui/*.c) \
       kernel.c

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
       $(OBJ_DIR)/edit_mct.o

all: $(OBJ_DIR) myos.bin

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)
	mkdir -p $(OBJ_DIR)/src/drivers
	mkdir -p $(OBJ_DIR)/src/sys
	mkdir -p $(OBJ_DIR)/src/apps
	mkdir -p $(OBJ_DIR)/src/gui

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

taskmgr.mct: taskmgr.c
	python3 build_mct.py taskmgr.c taskmgr.mct

edit.mct: edit.c
	python3 build_mct.py edit.c edit.mct

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
	objcopy -I binary -O elf32-i386 -B i386 $< $@

$(OBJ_DIR)/edit_mct.o: edit.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 $< $@

$(OBJ_DIR)/wallpaper.o: $(OBJ_DIR)/wallpaper.bin | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 $< $@

$(OBJ_DIR)/wallpaper.bin:
	python3 scratch/build_wallpaper.py /home/mectov/.gemini/antigravity/brain/1701ced2-e485-4124-afac-9fa8400a902f/media__1777482843135.png $@

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

myos.bin: $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o myos.bin

clean:
	rm -rf $(OBJ_DIR) myos.bin

.PHONY: all clean
