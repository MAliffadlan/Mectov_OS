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
       $(OBJ_DIR)/wallpaper.o

all: $(OBJ_DIR) myos.bin

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)
	mkdir -p $(OBJ_DIR)/src/drivers
	mkdir -p $(OBJ_DIR)/src/sys
	mkdir -p $(OBJ_DIR)/src/apps
	mkdir -p $(OBJ_DIR)/src/gui

$(OBJ_DIR)/boot.o: boot.asm
	$(AS) $(ASFLAGS) $< -o $@

$(OBJ_DIR)/src/sys/interrupt_entry.o: src/sys/interrupt_entry.asm
	$(AS) $(ASFLAGS) $< -o $@

$(OBJ_DIR)/wallpaper.o: $(OBJ_DIR)/wallpaper.bin
	objcopy -I binary -O elf32-i386 -B i386 $< $@

$(OBJ_DIR)/wallpaper.bin:
	python3 scratch/build_wallpaper.py /home/mectov/.gemini/antigravity/brain/1701ced2-e485-4124-afac-9fa8400a902f/media__1777224290409.png $@

$(OBJ_DIR)/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

myos.bin: $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o myos.bin

clean:
	rm -rf $(OBJ_DIR) myos.bin

.PHONY: all clean
