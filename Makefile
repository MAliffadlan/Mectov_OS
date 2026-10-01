CC = gcc
AS = nasm
LD = ld

CFLAGS = -m32 -std=gnu99 -ffreestanding -O2 -Wall -Wextra -g -msoft-float -mno-80387 -mno-sse -mno-mmx -march=i686 -fno-pie -fno-pic -MMD -MP
# -march=i686: this GCC's -m32 default is already i686 (Ubuntu builds with
# --with-arch-32=i686), but pin it so a toolchain whose default is plain
# i386 (cmov-less, no scheduling for P6) can't silently downgrade the
# kernel. Requires CMOV — every CPU this OS boots on (PAE+NX era, qemu32)
# has it.
# -fno-pie -fno-pic: the kernel is linked at fixed 1M by linker.ld and
# never relocated, yet GCC's default-PIE distro builds add -fPIE even to
# -c compiles, forcing every global access through the GOT via
# __x86.get_pc_thunk stubs (myos.bin carried a .got/.got.plt). Explicitly
# disabling PIC drops all that indirection.
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
LDFLAGS = -m elf_i386 -T linker.ld -z noexecstack
ASFLAGS = -f elf32

# DOOM compile flags: redirect standard headers to our mini libc
# v38.29: windowed DOOM — DG_ScreenBuffer is a 2x upscale of the 320x200
# internal screen (640x400) and gets scaled into a WM window by the
# compositor. (Was 1024x768 when DOOM owned the whole framebuffer.)
DOOM_CFLAGS = -m32 -std=gnu99 -ffreestanding -O2 -MMD -MP \
              -isystem doom/include_override -Idoom \
              -fno-builtin -march=i686 -fno-pie -fno-pic \
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
       $(OBJ_DIR)/gcalc_mct.o \
       $(OBJ_DIR)/hello_mct.o \
       $(OBJ_DIR)/keyshow_mct.o \
       $(OBJ_DIR)/clock_mct.o \
       $(OBJ_DIR)/snake_mct.o \
       $(OBJ_DIR)/sysinfo_mct.o \
       $(OBJ_DIR)/pci_mct.o \
       $(OBJ_DIR)/explorer_mct.o \
       $(OBJ_DIR)/browser_mct.o \
       $(OBJ_DIR)/paint_mct.o \
       $(OBJ_DIR)/terminal_mct.o \
       $(OBJ_DIR)/taskmgr_mct.o \
       $(OBJ_DIR)/notepad_mct.o \
       $(OBJ_DIR)/flappy_mct.o \
       $(OBJ_DIR)/forkdemo_mct.o \
       $(OBJ_DIR)/procfsdemo_mct.o \
       $(OBJ_DIR)/fputest_mct.o \
       $(OBJ_DIR)/hardening_test_mct.o \
       $(OBJ_DIR)/nxtest_mct.o \
       $(OBJ_DIR)/fbmap_mct.o \
       $(OBJ_DIR)/execdemo_mct.o \
       $(OBJ_DIR)/execchild_mct.o \
       $(OBJ_DIR)/tcpserver_mct.o \
       $(OBJ_DIR)/shmdemo_mct.o \
       $(OBJ_DIR)/mmapdemo_mct.o \
       $(OBJ_DIR)/mmapfiledemo_mct.o \
       $(OBJ_DIR)/syncfiledemo_mct.o \
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
       $(OBJ_DIR)/elfdemo_elf.o \
       $(OBJ_DIR)/syncdemo_elf.o \
       $(OBJ_DIR)/udptest_elf.o \
       $(DOOM_OBJS)

# --- Default build: the 64-bit kernel (mainline since 2026-09-18) ---
# Bare `make` builds the 64-bit kernel + ISO. The previous 32-bit line is kept
# fully buildable, just explicit:
#   make all32              myos.bin            (32-bit kernel)
#   make all32 && make iso  mectov.iso          (32-bit ISO, ./run.sh does this)
#   make all64 | make       myos64.bin + mectov64.iso (64-bit, the default)
#   make check64            64-bit gate battery (run64.sh + the scripts/ tests)
#   make check              32-bit battery (scripts/check.py, ~25 min)
.DEFAULT_GOAL := all64
all: all64
all64: myos64.bin iso64
all32: $(OBJ_DIR) myos.bin

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

paint.mct: apps/paint.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/paint.c paint.mct

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

procfsdemo.mct: apps/procfsdemo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/procfsdemo.c procfsdemo.mct

fputest.mct: apps/fputest.c $(MCT_LIBC_H)
	MCT_CFLAGS_EXTRA="-msse -msse2" python3 scripts/build_mct.py apps/fputest.c fputest.mct

hardening_test.mct: apps/hardening_test.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/hardening_test.c hardening_test.mct

nxtest.mct: apps/nxtest.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/nxtest.c nxtest.mct

fbmap.mct: apps/fbmap.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/fbmap.c fbmap.mct

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

syncfiledemo.mct: apps/syncfiledemo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/syncfiledemo.c syncfiledemo.mct

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

$(OBJ_DIR)/paint_mct.o: paint.mct | $(OBJ_DIR)
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

$(OBJ_DIR)/procfsdemo_mct.o: procfsdemo.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 procfsdemo.mct $(OBJ_DIR)/procfsdemo_mct.o

$(OBJ_DIR)/fputest_mct.o: fputest.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 fputest.mct $(OBJ_DIR)/fputest_mct.o

$(OBJ_DIR)/hardening_test_mct.o: hardening_test.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 hardening_test.mct $(OBJ_DIR)/hardening_test_mct.o

$(OBJ_DIR)/nxtest_mct.o: nxtest.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 nxtest.mct $(OBJ_DIR)/nxtest_mct.o

$(OBJ_DIR)/fbmap_mct.o: fbmap.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 fbmap.mct $(OBJ_DIR)/fbmap_mct.o

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

$(OBJ_DIR)/syncfiledemo_mct.o: syncfiledemo.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 syncfiledemo.mct $(OBJ_DIR)/syncfiledemo_mct.o

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

# (System-blob objects removed in v38.81: doom1.wad / wallpaper.bin /
# music.wav now live on /ext2, loaded on demand — see scripts/seed_ext2.sh
# and src/sys/assets.c. The wallpaper.bin rule below stays: it is the seed
# source for fresh images.)

# ELF demo app: built as a real ELF32 binary (v38.63: ET_DYN PIE at offset
# 0 — the kernel loader applies an ASLR bias) and embedded for VFS injection.
# Depends on the builder script too, so a toolchain change (e.g. ET_EXEC ->
# PIE) forces the app binaries to rebuild.
elfdemo.elf: apps/elfdemo.c scripts/build_elf.py
	python3 scripts/build_elf.py apps/elfdemo.c elfdemo.elf

$(OBJ_DIR)/elfdemo_elf.o: elfdemo.elf | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 elfdemo.elf $(OBJ_DIR)/elfdemo_elf.o

# Sync demo (semaphore + futex test, built as ELF PIE)
syncdemo.elf: apps/syncdemo.c scripts/build_elf.py
	python3 scripts/build_elf.py apps/syncdemo.c syncdemo.elf

$(OBJ_DIR)/syncdemo_elf.o: syncdemo.elf | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 syncdemo.elf $(OBJ_DIR)/syncdemo_elf.o

# UDP test app (validates the UDP syscall API, ELF PIE)
udptest.elf: apps/udptest.c scripts/build_elf.py
	python3 scripts/build_elf.py apps/udptest.c udptest.elf

$(OBJ_DIR)/udptest_elf.o: udptest.elf | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 udptest.elf $(OBJ_DIR)/udptest_elf.o

$(OBJ_DIR)/wallpaper.bin: assets/wallpaper.png
	python3 scripts/build_wallpaper.py assets/wallpaper.png $@

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
	# Debloated shipping binary (v38.81): debug info lives in
	# myos.bin.debug (for gdb + the COM2 stub symbols); the multiboot
	# image GRUB loads stays lean. GDB: `gdb myos.bin.debug`.
	objcopy --only-keep-debug myos.bin myos.bin.debug
	objcopy --strip-debug myos.bin

clean:
	rm -rf $(OBJ_DIR) myos.bin myos.bin.debug

# Full clean: both lines (run.sh calls this before its 32-bit rebuild).
clean_all: clean clean64
	rm -f *.mct *.elf

# --- Local test battery (CI parity with .github/workflows/build-boot-test.yml) ---
# `make check` builds kernel + ISO, recreates fresh disk/ext2/fat32 images
# (same as CI), runs every suite in CI order and prints a concise table.
#   make check                full battery (~25 min TCG)
#   make check-quick          fast high-signal subset (~6-8 min)
#   make check CHECK_ARGS=--keep-images   keep current disk images
#   make check CHECK_ARGS=--only=boot     single suite
#   make check CHECK_ARGS=--kvm          include fork/fputest KVM regressions
#                                        (opt-in: some hosts' KVM stalls the qemu32
#                                        machine at AP wake even with /dev/kvm)
iso: myos.bin
	@mkdir -p iso/boot/grub
	cp myos.bin iso/boot/
	@printf 'set timeout=0\nset default=0\nmenuentry "Mectov OS" {\n    multiboot /boot/myos.bin\n    boot\n}\n' > iso/boot/grub/grub.cfg
	grub-mkrescue -o mectov.iso iso

check: iso
	python3 scripts/check.py $(CHECK_ARGS)

check-quick: iso
	python3 scripts/check.py --quick $(CHECK_ARGS)

.PHONY: all all32 all64 clean clean_all check check-quick iso myos64 iso64 clean64 check64

# --- 64-bit bring-up (M1, x86_64-port branch) ---
# Parallel build that does NOT touch the 32-bit kernel: separate sources
# (boot64.asm + kernel64.c), separate linker script (linker64.ld), separate
# object dir (obj64/) and output (myos64.bin / mectov64.iso).
CC64 = gcc
AS64 = nasm
LD64 = ld
# -mgeneral-regs-only: the kernel must NOT touch XMM/x87. Interrupt and
# syscall entries save GPRs only (k64/entry64.asm keeps 15 regs, à la regs64_t),
# so the documented ABI ("the kernel preserves all registers except RAX") only
# holds if the kernel never uses a vector register itself. It did: -O2 happily
# emitted 16-byte XMM copies all over k64/ (heap64 alone had 225 of them), and
# an M12 shell that kept its FS request magic in xmm1 across `int $0x80` read
# back 0x20202020 — a console row of spaces left in XMM by the kernel. Cost is
# a few byte-wise struct copies; the alternative is FXSAVE/FXRSTOR on every
# entry, which the eager per-task FPU images in task64.c make unnecessary.
CFLAGS64 = -m64 -std=gnu99 -ffreestanding -O2 -Wall -Wextra -g -march=x86-64 -mcmodel=kernel -mno-red-zone -fno-pie -fno-pic -mgeneral-regs-only -MMD -MP
LDFLAGS64 = -m elf_x86_64 -T linker64.ld -z noexecstack
ASFLAGS64 = -f elf64
OBJ64_DIR = obj64
OBJS64 = $(OBJ64_DIR)/boot64.o $(OBJ64_DIR)/kernel64.o \
         $(OBJ64_DIR)/k64_gdt64.o $(OBJ64_DIR)/k64_idt64.o \
         $(OBJ64_DIR)/k64_isr64.o $(OBJ64_DIR)/k64_mem64.o \
         $(OBJ64_DIR)/k64_task64.o $(OBJ64_DIR)/k64_syscall64.o \
         $(OBJ64_DIR)/k64_loader64.o         $(OBJ64_DIR)/k64_smp64.o \
         $(OBJ64_DIR)/k64_kbd64.o $(OBJ64_DIR)/k64_console64.o \
         $(OBJ64_DIR)/k64_gfx64.o $(OBJ64_DIR)/k64_gui64.o \
         $(OBJ64_DIR)/k64_mouse64.o \
         $(OBJ64_DIR)/k64_heap64.o \
         $(OBJ64_DIR)/font8x16.o \
         $(OBJ64_DIR)/entry64.o $(OBJ64_DIR)/tramp64_bin.o \
         $(OBJ64_DIR)/hello64_mct.o $(OBJ64_DIR)/fpu64_mct.o \
         $(OBJ64_DIR)/clone64_mct.o $(OBJ64_DIR)/forkdemo64_mct.o \
         $(OBJ64_DIR)/execdemo64_mct.o $(OBJ64_DIR)/execchild64_elf.o \
         $(OBJ64_DIR)/shell64_mct.o $(OBJ64_DIR)/argdemo64_mct.o \
         $(OBJ64_DIR)/shelltest64_mct.o $(OBJ64_DIR)/brkdemo64_mct.o \
         $(OBJ64_DIR)/nxtest64_mct.o $(OBJ64_DIR)/asldemo64_mct.o \
         $(OBJ64_DIR)/smptest64_mct.o

$(OBJ64_DIR):
	@mkdir -p $(OBJ64_DIR)

$(OBJ64_DIR)/boot64.o: boot64.asm | $(OBJ64_DIR)
	$(AS64) $(ASFLAGS64) $< -o $@

$(OBJ64_DIR)/kernel64.o: kernel64.c k64/cpu64.h | $(OBJ64_DIR)
	$(CC64) $(CFLAGS64) -c $< -o $@

$(OBJ64_DIR)/k64_%.o: k64/%.c k64/cpu64.h | $(OBJ64_DIR)
	$(CC64) $(CFLAGS64) -c $< -o $@

$(OBJ64_DIR)/entry64.o: k64/entry64.asm | $(OBJ64_DIR)
	$(AS64) $(ASFLAGS64) $< -o $@

# VGA-1: the 64-bit console draws with the same glyph table as the 32-bit
# desktop (src/drivers/font8x16.c). Pure data, so the 64-bit flags are fine
# and there is only ever one font to keep in sync.
$(OBJ64_DIR)/font8x16.o: src/drivers/font8x16.c src/include/font8x16.h | $(OBJ64_DIR)
	$(CC64) $(CFLAGS64) -c $< -o $@

# --- M6 AP trampoline: 16-bit blob loaded at 0x8000 via SIPI ---
k64/tramp64.bin: k64/tramp64.asm
	nasm -f bin $< -o $@

$(OBJ64_DIR)/tramp64_bin.o: k64/tramp64.bin | $(OBJ64_DIR)
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 $< $@

# --- M5 Ring-3 images: MCT2 (flat fixed-base, build_mct64.py) + one ELF64
# PIE (build_elf64.py, exec target proving the ELF loader). Raw images are
# embedded into the kernel; the M5 loader parses them at spawn/exec time
# (no more blind-BASE raw blobs).
demos/hello64.mct: demos/hello64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/hello64.c demos/hello64.mct 0x40000000

demos/fpu64.mct: demos/fpu64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/fpu64.c demos/fpu64.mct 0x41000000

demos/clone64.mct: demos/clone64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/clone64.c demos/clone64.mct 0x42000000

demos/forkdemo64.mct: demos/forkdemo64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/forkdemo64.c demos/forkdemo64.mct 0x43000000

demos/execdemo64.mct: demos/execdemo64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/execdemo64.c demos/execdemo64.mct 0x44000000

demos/shell64.mct: demos/shell64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/shell64.c demos/shell64.mct 0x45000000

demos/argdemo64.mct: demos/argdemo64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/argdemo64.c demos/argdemo64.mct 0x46000000

demos/shelltest64.mct: demos/shelltest64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/shelltest64.c demos/shelltest64.mct 0x47000000

demos/brkdemo64.mct: demos/brkdemo64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/brkdemo64.c demos/brkdemo64.mct 0x48000000

demos/nxtest64.mct: demos/nxtest64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/nxtest64.c demos/nxtest64.mct 0x49000000

demos/asldemo64.mct: demos/asldemo64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/asldemo64.c demos/asldemo64.mct 0x4A000000

demos/smptest64.mct: demos/smptest64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/smptest64.c demos/smptest64.mct 0x4B000000

demos/execchild64.elf: demos/execchild64.c demos/sys64.h demos/entry.S scripts/build_elf64.py
	python3 scripts/build_elf64.py demos/execchild64.c demos/execchild64.elf

$(OBJ64_DIR)/%64_mct.o: demos/%64.mct | $(OBJ64_DIR)
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 $< $@

$(OBJ64_DIR)/execchild64_elf.o: demos/execchild64.elf | $(OBJ64_DIR)
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 $< $@

-include $(OBJS64:.o=.d)

myos64.bin: $(OBJS64)
	$(LD64) $(LDFLAGS64) $(OBJS64) -o myos64.bin

iso64: myos64.bin
	@mkdir -p iso64/boot/grub
	cp myos64.bin iso64/boot/
	@printf 'set timeout=0\nset default=0\nmenuentry "Mectov OS 64" {\n    multiboot2 /boot/myos64.bin $(MECTOV64_CMDLINE)\n    boot\n}\n' > iso64/boot/grub/grub.cfg
	grub-mkrescue -o mectov64.iso iso64

clean64:
	rm -rf $(OBJ64_DIR) myos64.bin mectov64.iso iso64 serial64.log
	rm -f demos/*64.o demos/*64.elf demos/*64.bin demos/*64.mct demos/entry64.o
	rm -f k64/tramp64.bin

check64: iso64
	./run64.sh --headless && python3 scripts/kbd_test.py && \
	python3 scripts/heap_test.py && python3 scripts/cons_test.py && \
	python3 scripts/gui_test.py
