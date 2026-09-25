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

# Quake III (v38.98): the engine core built into the kernel behind
# MECTOV_Q3=1 (default off). Stub headers under third_party/q3mectov/stubs
# alias every libc name to q3_* implementations (q3_kernel.c), so the engine
# objects reference nothing from utils.c/doom_libc.
#
# v38.105: the sources are the OFFICIAL id Software Quake III Arena release
# (third_party/q3a — id-Software/Quake-III-Arena, see its UPSTREAM.md), not an
# engine fork. vm.c + vm_interpreted.c are id's own QVM loader and portable
# bytecode interpreter, so the kernel can execute qagame.qvm — real Quake III
# Arena game bytecode built from that same source by id's own lcc + q3asm
# (scripts/build_qvm.sh).
Q3_ENABLED := $(shell [ "$(MECTOV_Q3)" = "1" ] && echo 1 || echo 0)
ifeq ($(Q3_ENABLED),1)
CFLAGS += -DMECTOV_Q3=1
endif
# v38.99: flipping MECTOV_Q3 changes CFLAGS, which make's dependency check
# cannot see — cmd_q3.o built in one mode was reused as-is in the other and
# the Q3 ISO once shipped the "engine not compiled in" stub. Stamp the mode;
# on mismatch wipe the object tree so every variant starts clean.
Q3_STAMP := $(OBJ_DIR)/.q3mode
ifneq ($(shell cat $(Q3_STAMP) 2>/dev/null),$(Q3_ENABLED))
$(shell mkdir -p $(OBJ_DIR); rm -rf $(OBJ_DIR)/*; echo "$(Q3_ENABLED)" > "$(Q3_STAMP)")
endif
Q3_DIR = third_party/q3a/code
Q3_OUR = third_party/q3mectov
# q_shared.c/q_math.c live in code/game in the official layout (id moved them
# out of qcommon after this release), and the official build has no md5.c /
# ioapi.c / net_ip.c — those are fork additions.
Q3_SRCS = $(Q3_DIR)/game/q_math.c $(Q3_DIR)/game/q_shared.c \
          $(Q3_DIR)/qcommon/common.c $(Q3_DIR)/qcommon/cvar.c $(Q3_DIR)/qcommon/cmd.c \
          $(Q3_DIR)/qcommon/files.c $(Q3_DIR)/qcommon/msg.c \
          $(Q3_DIR)/qcommon/huffman.c $(Q3_DIR)/qcommon/md4.c \
          $(Q3_DIR)/qcommon/net_chan.c $(Q3_DIR)/qcommon/unzip.c \
          $(Q3_DIR)/qcommon/vm.c $(Q3_DIR)/qcommon/vm_interpreted.c \
          $(Q3_DIR)/qcommon/cm_load.c $(Q3_DIR)/qcommon/cm_trace.c \
          $(Q3_DIR)/qcommon/cm_test.c $(Q3_DIR)/qcommon/cm_patch.c \
          $(Q3_DIR)/qcommon/cm_polylib.c \
          $(Q3_DIR)/null/null_input.c $(Q3_DIR)/null/null_snddma.c \
          $(Q3_OUR)/q3_kernel.c $(Q3_OUR)/q3_printf.c \
          $(Q3_OUR)/q3_platform.c $(Q3_OUR)/q3_client.c \
          $(Q3_OUR)/q3_map.c $(Q3_OUR)/q3_vm.c
# null/null_client.c is deliberately NOT built any more (v38.103): q3_client.c
# replaces the upstream null client with the Mectov client layer — CL_Init,
# CL_Frame, CL_KeyEvent/CL_CharEvent/CL_MouseEvent, the bind commands and the
# key-name tables all live there, wired to TinyGL and the WM input paths.
# TinyGL (v38.102, Q3 phase 2): software OpenGL 1.1 rasterizer rendering the
# gears scene into a WM window. Vendored from jserv/tinygl (MIT); core only —
# ztext/glDrawText dropped, allocator shim routes gl_malloc to kmalloc.
TGL_DIR = third_party/tinygl
TGL_SRCS = $(TGL_DIR)/src/api.c $(TGL_DIR)/src/arrays.c $(TGL_DIR)/src/clear.c \
           $(TGL_DIR)/src/clip.c $(TGL_DIR)/src/get.c $(TGL_DIR)/src/image_util.c \
           $(TGL_DIR)/src/init.c $(TGL_DIR)/src/light.c $(TGL_DIR)/src/list.c \
           $(TGL_DIR)/src/matrix.c \
           $(TGL_DIR)/src/misc.c \
           $(TGL_DIR)/src/msghandling.c $(TGL_DIR)/src/ztext.c \
           $(TGL_DIR)/src/select.c $(TGL_DIR)/src/specbuf.c $(TGL_DIR)/src/texture.c \
           $(TGL_DIR)/src/vertex.c $(TGL_DIR)/src/zbuffer.c $(TGL_DIR)/src/zline.c \
           $(TGL_DIR)/src/zmath.c $(TGL_DIR)/src/zpostprocess.c $(TGL_DIR)/src/zraster.c \
           $(TGL_DIR)/src/ztriangle.c \
           $(TGL_DIR)/kernel_shim.c $(TGL_DIR)/q3gl_window.c \
           $(TGL_DIR)/q3cl_render.c
ifeq ($(Q3_ENABLED),1)
Q3_OBJS = $(patsubst $(Q3_DIR)/%,$(OBJ_DIR)/q3/%,$(Q3_SRCS:.c=.o))
Q3_OBJS := $(patsubst $(Q3_OUR)/%,$(OBJ_DIR)/q3plat/%,$(Q3_OBJS))
TGL_OBJS = $(patsubst $(TGL_DIR)/%,$(OBJ_DIR)/tgl/%,$(TGL_SRCS:.c=.o))
else
Q3_OBJS =
TGL_OBJS =
endif
# -DSTANDALONE: no CD-key write / no client-only branches (we ship no id
#   game data); -DDEDICATED would also work but keeps the client light off.
# id's engine core and DOOM each ship a zone allocator, and id's common.c
# exports Z_Malloc/Z_Free/Z_FreeTags/Z_CheckHeap/Z_ClearZone plus the mainzone
# global (its own build never linked DOOM, so the clash could not happen there).
# obj/doom is not ours to rewrite and third_party/q3a must stay verbatim, so the
# rename is done in the preprocessor — a build-level alias, not an edit.
Q3_ZONE_RENAME = -Dmainzone=Q3Z_mainzone -DZ_Malloc=Q3Z_Malloc \
                 -DZ_Free=Q3Z_Free -DZ_FreeTags=Q3Z_FreeTags \
                 -DZ_CheckHeap=Q3Z_CheckHeap -DZ_ClearZone=Q3Z_ClearZone

# -DC_ONLY selects id's portable C implementations. One of them is
#   BoxOnPlaneSide in game/q_math.c, whose ONLY definition on Linux/i386 sits
#   behind `#if !((__linux__||__FreeBSD__) && __i386__ && !C_ONLY)` — upstream
#   expected that combination to come from an assembly object the kernel does
#   not build, so without this the collision tree has nothing to call (v38.107
#   hit exactly that as `undefined reference to BoxOnPlaneSide` the moment
#   cm_test.c joined the link). C_ONLY appears in only three places in the whole
#   tree: that guard, the branch inside it, and a PowerPC path we never take.
Q3_CFLAGS = -m32 -std=gnu99 -ffreestanding -O1 -MMD -MP \
              -I$(Q3_OUR)/stubs -I$(Q3_DIR)/qcommon -I$(Q3_DIR)/game \
              -I$(Q3_DIR)/null \
              -fno-builtin -fno-pie -fno-pic -march=i686 \
              -DMECTOV_Q3=1 -DSTANDALONE -DC_ONLY $(Q3_ZONE_RENAME) -w

# v38.107 (Q3 phase 7): id's OWN collision model — cm_load.c, cm_trace.c,
# cm_test.c, cm_patch.c, cm_polylib.c — is compiled into the kernel above.
# Until now the port answered G_TRACE from a hand-written world (a floor plane
# at z=0); CM_LoadMap + CM_BoxTrace replace it with the real .bsp, which is what
# makes id's own entity string, brush tracing and player collision available to
# the official game module. They are host-side (not VM) code and need nothing
# from the QVM loader, which is why they could stay unbuilt until a .bsp existed
# to load.

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
       $(OBJ_DIR)/procsysdemo_mct.o \
       $(OBJ_DIR)/tmpfsdemo_mct.o \
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
       $(DOOM_OBJS) \
       $(Q3_OBJS) \
       $(TGL_OBJS)

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

paint.mct: apps/paint.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/paint.c paint.mct

# v38.107: apps/terminal.c prints the release string from src/include/version.h,
# so that header is a prerequisite here even though it is not a source file of
# this target. scripts/build_mct.py emits no dependency files, which means make
# cannot see a changed header on its own — and the failure is silent: after
# OS_VERSION moved into version.h, the Terminal still shipped "v38.106" while
# the kernel reported v38.107, which is the exact class of stale-number bug
# this release set out to remove. Any app that starts printing the version needs
# this prerequisite too.
terminal.mct: apps/terminal.c $(MCT_LIBC_H) src/include/version.h
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

procsysdemo.mct: apps/procsysdemo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/procsysdemo.c procsysdemo.mct

tmpfsdemo.mct: apps/tmpfsdemo.c $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/tmpfsdemo.c tmpfsdemo.mct

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

$(OBJ_DIR)/procsysdemo_mct.o: procsysdemo.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 procsysdemo.mct $(OBJ_DIR)/procsysdemo_mct.o

$(OBJ_DIR)/tmpfsdemo_mct.o: tmpfsdemo.mct | $(OBJ_DIR)
	objcopy -I binary -O elf32-i386 -B i386 tmpfsdemo.mct $(OBJ_DIR)/tmpfsdemo_mct.o

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

# Q3 source compilation rule (objects only enter the link when MECTOV_Q3=1).
# The Makefile is a prerequisite on purpose: Q3_CFLAGS is not something make's
# dependency tracking can see, so an edited flag would otherwise silently reuse
# objects built with the old one (the v38.99 class of bug).
$(OBJ_DIR)/q3/%.o: $(Q3_DIR)/%.c Makefile | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(Q3_CFLAGS) -c $< -o $@

$(OBJ_DIR)/q3plat/%.o: $(Q3_OUR)/%.c Makefile | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(Q3_CFLAGS) -c $< -o $@

# TinyGL (v38.102): compiled like the Q3 tree — libc names resolve through
# the q3 stub headers (-I$(Q3_OUR)/stubs) to kmalloc-backed shims, plus the
# TinyGL include dirs. TGL_FEATURE_* stay at their defaults (32-bit render).
TGL_CFLAGS = -m32 -std=gnu99 -ffreestanding -O1 -MMD -MP \
              -I$(Q3_OUR)/stubs -I$(TGL_DIR)/include -I$(TGL_DIR)/src \
              -fno-builtin -fno-pie -fno-pic -march=i686 \
              -DMECTOV_Q3=1 -DNO_DEBUG_OUTPUT -w
# memory.c is EXCLUDED from TGL_SRCS: it defines gl_malloc/gl_free over the
# host malloc, and kernel_shim.c provides those symbols over kmalloc instead.

$(OBJ_DIR)/tgl/src/%.o: $(TGL_DIR)/src/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(TGL_CFLAGS) -c $< -o $@

$(OBJ_DIR)/tgl/kernel_shim.o: $(TGL_DIR)/kernel_shim.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(TGL_CFLAGS) -c $< -o $@

$(OBJ_DIR)/tgl/q3gl_window.o: $(TGL_DIR)/q3gl_window.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(TGL_CFLAGS) -Isrc/include -c $< -o $@

# q3cl_render.c (v38.103): TinyGL backend for the Q3 client's first-person
# arena. Same include shape as q3gl_window.c — it includes only <TGL/gl.h>,
# zbuffer.h and its own header, which the client TU shares.
$(OBJ_DIR)/tgl/q3cl_render.o: $(TGL_DIR)/q3cl_render.c $(TGL_DIR)/q3cl_render.h | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(TGL_CFLAGS) -c $< -o $@

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

clean_all: clean
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
ifeq ($(Q3_ENABLED),1)
# v38.105: the Q3 ISO ships real Quake III Arena bytecode, so building it
# implies building qagame.qvm from third_party/q3a with id's own lcc + q3asm.
QVM_PREREQ = qvm
endif

iso: myos.bin $(QVM_PREREQ)
	@mkdir -p iso/boot/grub
	cp myos.bin iso/boot/
	@printf 'set timeout=0\nset default=0\nmenuentry "Mectov OS" {\n    multiboot /boot/myos.bin\n    boot\n}\n' > iso/boot/grub/grub.cfg
	grub-mkrescue -o mectov.iso iso

check: iso
	python3 scripts/check.py $(CHECK_ARGS)

check-quick: iso
	python3 scripts/check.py --quick $(CHECK_ARGS)

# ioquake3 engine-core suite (v38.99): builds the MECTOV_Q3=1 kernel/ISO
# variant and runs only the q3 regression against it.
check-q3:
	MECTOV_Q3=1 $(MAKE) iso
	python3 scripts/check.py --keep-images --only q3 $(CHECK_ARGS)

# TinyGL gears window (v38.102, Q3 phase 2): same MECTOV_Q3=1 ISO, visual
# test only. `make check-q3tgl` runs both Q3-phase suites.
check-q3tgl:
	MECTOV_Q3=1 $(MAKE) iso
	python3 scripts/check.py --keep-images --only q3gl $(CHECK_ARGS)

# Q3 client loop (v38.103, Q3 phase 3): the same MECTOV_Q3=1 ISO again, this
# time driving the engine's CL_Init/CL_Frame with WM keyboard + captured mouse
# input. `make check-q3play` runs all three Q3-phase suites.
check-q3play:
	MECTOV_Q3=1 $(MAKE) iso
	python3 scripts/check.py --keep-images --only q3play $(CHECK_ARGS)

# Official QVM (v38.105, Q3 phase 5): builds qagame.qvm out of id's own source
# with id's own lcc + q3asm. Standalone — build_qvm.sh is safe to call directly
# and is a no-op when the bytecode is already newer than its inputs.
qvm:
	@bash scripts/build_qvm.sh

# The phase-5 suite: the same MECTOV_Q3=1 ISO, running the official game
# module as Quake VM bytecode through id's own interpreter.
check-q3vm:
	MECTOV_Q3=1 $(MAKE) iso
	python3 scripts/check.py --keep-images --only q3vm $(CHECK_ARGS)

.PHONY: all clean clean_all check check-quick qvm iso \
        check-q3 check-q3tgl check-q3play check-q3vm
