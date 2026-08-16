CC = gcc
AS = nasm
LD = ld

CFLAGS = -m32 -std=gnu99 -ffreestanding -O2 -Wall -Wextra -g -msoft-float -mno-80387 -mno-sse -mno-mmx -MMD -MP
# -MMD -MP: emit per-object .d dependency files so a header change (e.g.
# MAX_WINDOWS in wm.h) rebuilds every object that includes it. Without this,
# touching a header left stale .o files and the change silently never
# reached the binary.
# -msoft-float: the kernel core never emits x87/SSE instructions of its
# own — all float math in src/ + kernel.c goes through soft-float libgcc
# calls. Ring 3 apps and DOOM (which runs inside the shell's task) DO use
# the real FPU; since v38.41 the scheduler eagerly swaps the full
# x87+MMX+SSE image on every context switch (fxsave/fxrstor in fpu.c +
# schedule()), so multiple FPU users can be preempted against each other
# safely.
# -g keeps DWARF debug info in myos.bin so GDB can resolve kernel symbols
# (break kernel_main, bt, list, etc.) when debugging via the in-kernel stub.
LDFLAGS = -m elf_i386 -T linker.ld
ASFLAGS = -f elf32

# DOOM compile flags: redirect standard headers to our mini libc
# v38.29: windowed DOOM — DG_ScreenBuffer is a 2x upscale of the 320x200
# internal screen (640x400) and gets scaled into a WM window by the
# compositor. (Was 1024x768 when DOOM owned the whole framebuffer.)
DOOM_CFLAGS = -m32 -std=gnu99 -ffreestanding -O2 -MMD -MP \
              -isystem doom/include_override -Idoom \
              -fno-builtin \
              -DDOOMGENERIC_RESX=640 -DDOOMGENERIC_RESY=400 \
              -DFEATURE_SOUND \
              -w

SRC_DIR = src
OBJ_DIR = obj

# List of source files
SRCS = $(wildcard $(SRC_DIR)/drivers/*.c) \
       $(wildcard $(SRC_DIR)/sys/*.c) \
       $(wildcard $(SRC_DIR)/sys/shell/*.c) \
       $(wildcard $(SRC_DIR)/sys/shell/builtins/*/*.c) \
       $(wildcard $(SRC_DIR)/sys/shell/job/*.c) \
       $(wildcard $(SRC_DIR)/sys/shell/script/*.c) \
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
       $(OBJ_DIR)/keyshow_mct.o \
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
       $(OBJ_DIR)/fputest_mct.o \
       $(OBJ_DIR)/nxtest_mct.o \
       $(OBJ_DIR)/execdemo_mct.o \
       $(OBJ_DIR)/execchild_mct.o \
       $(OBJ_DIR)/tcpserver_mct.o \
       $(OBJ_DIR)/shmdemo_mct.o \
       $(OBJ_DIR)/mmapdemo_mct.o \
       $(OBJ_DIR)/mmapfiledemo_mct.o \
       $(OBJ_DIR)/lseekfiledemo_mct.o \
       $(OBJ_DIR)/pollselectdemo_mct.o \
       $(OBJ_DIR)/fat32demo_mct.o \
       $(OBJ_DIR)/rusthello_mct.o \
       $(OBJ_DIR)/demandtest_mct.o \
       $(OBJ_DIR)/segvtest_mct.o \
       $(OBJ_DIR)/looper_mct.o \
       $(OBJ_DIR)/crashme_mct.o \
       $(OBJ_DIR)/winman_mct.o \
       $(OBJ_DIR)/pipegen_mct.o \
       $(OBJ_DIR)/piperead_mct.o \
       $(OBJ_DIR)/sigdemo_mct.o \
       $(OBJ_DIR)/smpstress_mct.o \
       $(OBJ_DIR)/bgread_mct.o \
       $(OBJ_DIR)/fuzz_mct.o \
       $(OBJ_DIR)/iobench_mct.o \
       $(OBJ_DIR)/permtest_mct.o \
       $(OBJ_DIR)/threaddemo_mct.o \
       $(OBJ_DIR)/conddemo_mct.o \
       $(OBJ_DIR)/rlimittest_mct.o \
       $(OBJ_DIR)/bigread_mct.o \
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

.PHONY: obj-dirs
# obj/ itself is not phony (it is a real directory), but its rule must run
# every time: once obj/ exists make would otherwise consider it up-to-date
# and skip creating newly added subdirectories.
$(OBJ_DIR): obj-dirs
obj-dirs:
	@mkdir -p $(sort $(dir $(OBJS)))

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

$(OBJ_DIR)/keyshow_mct.o: keyshow.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 $< $@

MCT_LIBC_H = apps/lib/libc.h

gcalc.mct: apps/gcalc.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/gcalc.c gcalc.mct

hello.mct: apps/hello.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/hello.c hello.mct

keyshow.mct: apps/keyshow.c
	python3 scripts/build_mct.py apps/keyshow.c keyshow.mct

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

fputest.mct: apps/fputest.c $(MCT_LIBC_H)
	MCT_CFLAGS_EXTRA="-msse -msse2" python3 scripts/build_mct.py apps/fputest.c fputest.mct

nxtest.mct: apps/nxtest.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/nxtest.c nxtest.mct

execdemo.mct: apps/execdemo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/execdemo.c execdemo.mct

execchild.mct: apps/execchild.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/execchild.c execchild.mct

shmdemo.mct: apps/shmdemo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/shmdemo.c shmdemo.mct

mmapdemo.mct: apps/mmapdemo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/mmapdemo.c mmapdemo.mct

mmapfiledemo.mct: apps/mmapfiledemo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/mmapfiledemo.c mmapfiledemo.mct

lseekfiledemo.mct: apps/lseekfiledemo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/lseekfiledemo.c lseekfiledemo.mct

pollselectdemo.mct: apps/pollselectdemo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/pollselectdemo.c pollselectdemo.mct

fat32demo.mct: apps/fat32demo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/fat32demo.c fat32demo.mct

# Rust Ring 3 app: freestanding no_std, built via rustc (build_rust_mct.py
# finds rustc in ~/.cargo/bin when it is not on PATH).
rusthello.mct: apps/rusthello.rs scripts/build_rust_mct.py
	python3 scripts/build_rust_mct.py apps/rusthello.rs rusthello.mct

demandtest.mct: apps/demandtest.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/demandtest.c demandtest.mct

segvtest.mct: apps/segvtest.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/segvtest.c segvtest.mct

looper.mct: apps/looper.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/looper.c looper.mct

crashme.mct: apps/crashme.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/crashme.c crashme.mct

winman.mct: apps/winman.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/winman.c winman.mct

pipegen.mct: apps/pipegen.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/pipegen.c pipegen.mct

piperead.mct: apps/piperead.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/piperead.c piperead.mct

sigdemo.mct: apps/sigdemo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/sigdemo.c sigdemo.mct

smpstress.mct: apps/smpstress.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/smpstress.c smpstress.mct

bgread.mct: apps/bgread.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/bgread.c bgread.mct

fuzz.mct: apps/fuzz.c
	python3 scripts/build_mct.py apps/fuzz.c fuzz.mct

iobench.mct: apps/iobench.c
	python3 scripts/build_mct.py apps/iobench.c iobench.mct

permtest.mct: apps/permtest.c
	python3 scripts/build_mct.py apps/permtest.c permtest.mct

threaddemo.mct: apps/threaddemo.c
	python3 scripts/build_mct.py apps/threaddemo.c threaddemo.mct

conddemo.mct: apps/conddemo.c
	python3 scripts/build_mct.py apps/conddemo.c conddemo.mct

rlimittest.mct: apps/rlimittest.c
	python3 scripts/build_mct.py apps/rlimittest.c rlimittest.mct

bigread.mct: apps/bigread.c
	python3 scripts/build_mct.py apps/bigread.c bigread.mct

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

$(OBJ_DIR)/fputest_mct.o: fputest.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 fputest.mct $(OBJ_DIR)/fputest_mct.o

$(OBJ_DIR)/nxtest_mct.o: nxtest.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 nxtest.mct $(OBJ_DIR)/nxtest_mct.o

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

$(OBJ_DIR)/mmapfiledemo_mct.o: mmapfiledemo.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 mmapfiledemo.mct $(OBJ_DIR)/mmapfiledemo_mct.o

$(OBJ_DIR)/lseekfiledemo_mct.o: lseekfiledemo.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 lseekfiledemo.mct $(OBJ_DIR)/lseekfiledemo_mct.o

$(OBJ_DIR)/pollselectdemo_mct.o: pollselectdemo.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 pollselectdemo.mct $(OBJ_DIR)/pollselectdemo_mct.o

$(OBJ_DIR)/fat32demo_mct.o: fat32demo.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 fat32demo.mct $(OBJ_DIR)/fat32demo_mct.o

$(OBJ_DIR)/rusthello_mct.o: rusthello.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 rusthello.mct $(OBJ_DIR)/rusthello_mct.o

$(OBJ_DIR)/demandtest_mct.o: demandtest.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 demandtest.mct $(OBJ_DIR)/demandtest_mct.o

$(OBJ_DIR)/segvtest_mct.o: segvtest.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 segvtest.mct $(OBJ_DIR)/segvtest_mct.o

$(OBJ_DIR)/fuzz_mct.o: fuzz.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 fuzz.mct $(OBJ_DIR)/fuzz_mct.o

$(OBJ_DIR)/iobench_mct.o: iobench.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 iobench.mct $(OBJ_DIR)/iobench_mct.o

$(OBJ_DIR)/permtest_mct.o: permtest.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 permtest.mct $(OBJ_DIR)/permtest_mct.o

$(OBJ_DIR)/threaddemo_mct.o: threaddemo.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 threaddemo.mct $(OBJ_DIR)/threaddemo_mct.o

$(OBJ_DIR)/conddemo_mct.o: conddemo.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 conddemo.mct $(OBJ_DIR)/conddemo_mct.o

$(OBJ_DIR)/rlimittest_mct.o: rlimittest.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 rlimittest.mct $(OBJ_DIR)/rlimittest_mct.o

$(OBJ_DIR)/bigread_mct.o: bigread.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 bigread.mct $(OBJ_DIR)/bigread_mct.o

$(OBJ_DIR)/looper_mct.o: looper.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 looper.mct $(OBJ_DIR)/looper_mct.o

$(OBJ_DIR)/crashme_mct.o: crashme.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 crashme.mct $(OBJ_DIR)/crashme_mct.o

$(OBJ_DIR)/winman_mct.o: winman.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 winman.mct $(OBJ_DIR)/winman_mct.o

$(OBJ_DIR)/pipegen_mct.o: pipegen.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 pipegen.mct $(OBJ_DIR)/pipegen_mct.o

$(OBJ_DIR)/piperead_mct.o: piperead.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 piperead.mct $(OBJ_DIR)/piperead_mct.o

$(OBJ_DIR)/sigdemo_mct.o: sigdemo.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 sigdemo.mct $(OBJ_DIR)/sigdemo_mct.o

$(OBJ_DIR)/smpstress_mct.o: smpstress.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 smpstress.mct $(OBJ_DIR)/smpstress_mct.o

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

# Auto-dependencies: include the .d files -MMD -MP emitted alongside each
# object so header changes trigger rebuilds (see CFLAGS comment).
-include $(OBJS:.o=.d)

myos.bin: $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o myos.bin

clean:
	rm -rf $(OBJ_DIR) myos.bin

clean_all: clean
	rm -f *.mct *.elf

.PHONY: all clean clean_all
