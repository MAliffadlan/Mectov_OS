# x86_64 Kernel — Audit and Roadmap

Status: living document, written 2026-09-18 on branch `x86_64-port`.

**The 64-bit kernel (`kernel64.c` + `k64/`) is the mainline.** The 32-bit tree
(`src/`) is the previous line: it is kept as *reference* — where it already
solved a problem (allocator hardening, ext2 on-disk formats, fonts, the visual
language, the scheduler's corner cases) that solution is prior art to reuse or
to improve on, but new subsystems are designed for 64-bit and live in `k64/`.
This document therefore reads as: what the 64-bit kernel owns today, what it
still needs, in which order, and what the 32-bit line contributes at each step.

Build/run as of this document: bare `make` builds `myos64.bin` + `mectov64.iso`
(`.DEFAULT_GOAL := all64`), `./run64.sh` boots it, `make check64` runs the gate
battery. The 32-bit line is explicit: `make all32`, `./run.sh`, `make check`.
`run.sh` and the CI boot test call `make all32` for exactly that reason.

Machine: `./run64.sh` boots `-machine pc` (since M11). QEMU's q35 puts every
`-cdrom`/`-drive` behind the ICH9 *AHCI* controller — MMIO, and no legacy
0x1F0/0x170 channel at all — so a legacy ATA block driver has nothing to talk
to there. pc is also the machine the 32-bit line has always used, and every
M1-M10 gate passes on it unchanged (verified: same window geometry, same
selftests). AHCI becomes a second backend behind `blk64_read()` when a q35 disk
is actually needed.

Context for the audit: the 32-bit desktop (the thing that looks "done") is not
a self-contained drawing library, it is the top of a stack — task/window model,
heap, IPC, VFS, RTC. The 64-bit kernel currently ends at the layer *below* that
stack, which is why its desktop is chrome drawn on the primitives that exist
(kernel framebuffer + console + mouse + one window) rather than a second copy
of `src/gui`.

---

## 1. What the 64-bit kernel already owns

| Milestone | Content | Files |
|---|---|---|
| M1 | long mode, boot, serial | `boot64.asm`, `kernel64.c` |
| M2 | GDT/IDT/TSS, exceptions | `k64/gdt64.c`, `k64/idt64.c`, `k64/isr64.c`, `k64/entry64.asm` |
| M3 | PMM + 4-level paging + COW/teardown capable VMM | `k64/mem64.c` |
| M4 | Ring-3 tasks, `int $0x80` ABI0 | `k64/task64.c`, `k64/syscall64.c`, `k64/loader64.c`, `demos/` |
| M5 | private PML4 per task, fork/exec, waitpid | `k64/task64.c`, `k64/mem64.c` |
| M6 | SMP (AP bring-up, IPIs, shootdowns) | `k64/smp64.c` |
| M7 | scheduler, sleep/block, brk/demand heap, ps/argv/spawn | `k64/task64.c`, `k64/syscall64.c` |
| M8 | text console on the Multiboot2 framebuffer | `k64/console64.c` |
| M9 | desktop chrome: gfx core, mouse, cursor compositing, one window | `k64/gfx64.c`, `k64/gui64.c`, `k64/mouse64.c` |
| M10 | kernel heap (`kmalloc`/`kfree`/`krealloc`/`kcalloc` + stats) | `k64/heap64.c` |

Gaps that matter for a real desktop:

* no block device (ATA/AHCI/virtio) — nothing can be read off disk;
* no VFS/FS layer — no `/ext2/wallpaper.bin`, no icons, no config files;
* no IPC/sync primitives — the 32-bit console→Terminal pipeline is IPC;
* no RTC — the lock screen clock has no source;
* no window model in the kernel (one static window, no z-order, no drag);
* no user-space app loader for `src/apps/*` (the 64-bit loader only runs the
  freestanding `demos/*64` images).

## 2. Is `-m64` itself the blocker? (measured, not assumed)

Every `src/**/*.c` file (149 of them) was compiled with the *exact* 64-bit flag
set the port uses (`-m64 -mcmodel=kernel -mno-red-zone -ffreestanding -O2
-march=x86-64`), stopping at the assembler stage:

| Result | Files |
|---|---|
| compiles clean under `-m64` today | **119** |
| fails *only* because of `src/include/spinlock.h` (32-bit `pushf/popf` asm) | **15** |
| fails on 32-bit asm inside the file itself (`%eax`-width `mov`, `pushfl`) | **15** |
| silent 32-bit pointer truncation (`-Wpointer-to-int-cast`, 301 sites) | 9 |

The 15 "include-swap-only" files: `drivers/ahci.c`, `drivers/ata.c`,
`drivers/keyboard.c`, `drivers/virtio_blk.c`, `drivers/xhci.c`, `gui/wm.c`,
`sys/blkcache.c`, `sys/clipboard.c`, `sys/entropy.c`, `sys/fd.c`, `sys/ipc.c`,
`sys/pcache.c`, `sys/shell/shell_core.c`, `sys/vfs.c`, `apps/terminal_stubs.c`.
The port already has the 64-bit equivalent of that header: `k64/spin64.h`.

The 15 files with their own 32-bit asm are the real mechanical work, and they
are exactly the ones the plan below touches first:
`sys/mem.c`, `sys/vmm.c`, `sys/task.c`, `sys/syscall.c`, `sys/idt.c`,
`sys/loader.c`, `sys/smp.c`, `sys/fpu.c`, `sys/panic.c`, `sys/klog.c`,
`sys/sync.c`, `drivers/serial.c`, `drivers/timer.c`, `drivers/mouse.c`,
`drivers/rtl8139.c`.

Truncation site concentration (this is where a naive `-m64` build would
"compile" and then corrupt memory at runtime):

| file | `-Wpointer-to-int-cast` sites |
|---|---|
| `sys/task.c` | 67 |
| `sys/syscall.c` | 66 |
| `sys/fd.c` | 61 |
| `sys/vfs.c` | 61 |
| `sys/idt.c` | 35 |
| `sys/mem.c` | 6 |
| `gui/wm.c`, `sys/smp.c` | 2 each |

So the old line is not blocked by the compiler: it is blocked by layers the
64-bit kernel has not built yet, and by latent 32-bit assumptions. A pointer
stored in a `uint32_t` field (`task_t`'s CR3/PD fields, fd buffers) survives
compilation and dies at runtime. That is exactly why the 64-bit kernel keeps
its own widened types in `k64/cpu64.h`, and why the roadmap below builds its
own subsystems instead of flipping `-m32` to `-m64` on the old tree. The
measurement is still useful the other way round: when a *specific algorithm*
from `src/` is worth lifting verbatim, this table says which files are cheap
to lift and which drag 32-bit assumptions with them.

## 3. What the 32-bit desktop is actually built on (reference)

Not a porting checklist — a map of the prior art, so the 64-bit line knows
what each piece of UI work implicitly depends on. Symbol references inside the
32-bit GUI and its immediate dependencies (grep counts, not estimates):

| subsystem | lines | who needs it | why |
|---|---|---|---|
| `sys/task.c` (`task_*`, window/task model) | 3544 | `gui/wm.c` (52), `gui/taskbar.c` (22) | window ownership, focus, z-order, per-task framebuffer |
| `sys/mem.c` (heap) | 537 | `gui/login.c` (3 buffers ≈ 12 MB), `gui/wm.c`, `drivers/vga.c` | double buffers, window backing store |
| `drivers/rtc.c` | 98 | `gui/login.c` (26) | lock-screen clock |
| `sys/ipc.c` + `sys/shm.c` + `sys/sync.c` | 970 | `drivers/vga.c` (13+45+8) | console output reaches the Ring-3 Terminal via IPC |
| `sys/vfs.c` + `sys/ext2.c` + `sys/fat32.c` + `drivers/{ata,ahci}.c` | 5774 | `gui/desktop.c`, `lib/assets` | icons.cfg, wallpaper, fonts, apps |
| `drivers/vga.c` (double-buffered console) | 1011 | all of `src/gui` | the compositor target |
| `gui/wm.c` + `gui/taskbar.c` + `gui/desktop.c` + `gui/login.c` | 3218 | — | the desktop itself (~15% of `src/`) |

The port already matches the *visual* contracts one-for-one: the same
`src/drivers/font8x16.c` is compiled into the 64-bit kernel, the `IC_*` palette
comes from `src/gui/login.c`, the cursor sprite is copied from `cursor_mask` /
`cursor_inner` in `drivers/vga.c`, and MB2 mode is the same 1024x768x32.

## 4. Roadmap

Steps are ordered by dependency, not by "how much 32-bit code they reuse".
Each one is independently bootable and gated by a marker in `run64.sh`, so a
regression can never hide behind a later step. The "prior art" column says what
the 32-bit line contributes: an algorithm worth lifting, a spec implementation
worth trusting, or nothing but a shape to improve on.

| step | deliverable | depends on | prior art in `src/` | gate |
|---|---|---|---|---|
| **M10** | kernel heap: `kmalloc/kfree/krealloc/kcalloc` + stats, frames mapped into a contiguous kernel VA window | M3 PMM/VMM | `sys/mem.c` heap ≈ 300 lines (list + magic/canary design) | `M10 HEAP SELFTEST OK` + `kmem` probe from Ring-3 |
| **M11** | block layer: legacy ATA PIO (IDENTIFY, LBA28 `0x20`, LBA48 `0x24`) + ATAPI `READ(12)` for the boot CD, 512B/2048B sectors | M4, M10 | `drivers/ata.c` 562 (command sequences, poll/timeout); it has no ATAPI path at all | `M11 BLK SELFTEST OK` + `scripts/blk_test.py` reproduces the guest's hashes host-side |
| **M12** | read-only filesystem layer: `fs64` core (mount table, lookup, readdir, open/read/close, path walk) + an ISO9660 backend for the boot CD and an ext2 backend for the ATA device, plus `SYS64_FSOP` so Ring-3 can `fo ls/cat/stat/hash` | M11, M10 | `ext2.c` 1065 (on-disk formats are facts — the parsing is worth lifting), `vfs.c` 3314 (shape only) | `M12 FS SELFTEST OK` + `scripts/fs_test.py` compares Ring-3 and kernel hashes against host-side hashes of both images |
| M13 | IPC + sync, sized for what the 64-bit design needs: message queue, shared memory, semaphores | M5, M10 | `ipc.c` + `shm.c` + `sync.c` 970 (prior art; the 32-bit shapes here are the weakest part of the old line) | producer/consumer across two tasks |
| **M14** (done) | window manager in the kernel: N windows, z-order, focus, damage-rect compositor on a screen back buffer, drag, title-bar buttons, taskbar buttons, cursor as a draw step | M9, M10 | `wm.c` 1191 (feature list: what a user expects; not an implementation to copy) | `scripts/gui_test.py`: raise, drag, uncovered-repaint, taskbar minimize/restore, close, single cursor |
| M15 | the 64-bit shell/desktop grows into the real UI on M14: multi-window desktop, taskbar, app hosting | M12–M14 | `gui/*` 3218 (visual language: `IC_*` palette, cursor sprite, layout) | driven by `scripts/gui_test.py`-style pixel checks |
| M16 | RTC + assets + persisted settings | M12 | `rtc.c` 98 (CMOS sequence) | clock ticks, settings survive reboot |

Note that only M12's *format* work (ext2/fat32 on-disk layout) is really "reuse
the 32-bit code": it is a spec implementation, and re-deriving it would be pure
waste. Everything else is a fresh 64-bit design that happens to know what the
old line learned. Non-goals for now: `drivers/xhci.c` (1619), `drivers/net.c`
(1449), `sys/watchdog.c`, the `apps/*` GUI apps.

## 5. M10 design notes (the kernel heap)

The allocator reuses the 32-bit line's proven design — `block_meta` free list,
magic + canary hardening, same API surface and counters — because none of that
is 32-bit specific; the structural change is that the arena is no longer a
fixed 24 MB window at `0x1800000`.

```
HEAP_VA_BASE = 0x90000000   (2.25 GB, supervisor RW+NX, 64 MB ceiling)
  ^ above the user window (1-2 GB, demo images / per-task stacks) and above
    the M3 selftest scratch at 0x80000000; inside a PDPT slot the M3 selftest
    already populated.
```

*Pages are backed lazily.* The first block that needs VA past the mapped tail
maps a fresh frame from the PMM there. The VA stays **contiguous**, which is
what keeps the block list a straight port: a block may span two pages and both
sides are mapped.

*All mapping is done against `pml4_boot`, never the current CR3* (the root is
saved/restored around the map). `kmalloc` is reachable from a task context, and
a PTE created in that task's private tables would be invisible to every other
task. Boot's supervisor tables are shared by pointer into every clone (M5), so
a heap mapping made once is visible from every address space — and `teardown`
correctly skips it because it compares against `pml4_boot`.

*16-byte payload alignment* (the 32-bit heap aligns to 4): the 64-bit kernel
saves 512-byte FPU images and will hold `u64`/SSE-friendly buffers.

Hardening is kept as-is, with the fatal path using lock-free serial
(`s_raws`/`s_rawx`) because the heap lock may be held when it fires.

### M10 as built (and the trap it exposed)

`k64/heap64.c` (~460 lines) + `vmm_map_page_in()` (map into an explicit root
while holding `mem_lock`, so a heap growth can never wire a PTE into "the
current root" — `vmm_root` is shared by every CPU).

The first boot of the Ring-3 side caught a real bug in the "kernel tables are
shared with clones" assumption: `clone_level` always fresh-copies *PML4*
entries, so only tables that already existed at clone time are shared below
the PDPT level. Mapping the heap lazily (first allocation, i.e. after the demo
tasks were spawned) left each task with a fresh, empty `PDPT[2]` and the first
`kmem` died on `#PF 0x90000010` on the shell's own CR3. Fix: back the arena's
first page in `heap64_init()`, before any address space is cloned — later
growth only adds PTEs / new PTs under frames that are then genuinely shared.

Gate: `run64.sh` requires `heap: arena 0x90000000 max 64MB frame-backed` and
`M10 HEAP SELFTEST OK` (pre-STI: 16-byte alignment, block packing, split,
first-fit reuse, both coalescing directions down to a single free block,
multi-page block with three distinct frames, VA→identity-alias check, kcalloc
zeroing, krealloc preservation, clean OOM, page accounting).
`scripts/heap_test.py` then proves the part the kernel-side selftest cannot:
from Ring-3 the shell runs `kmem`, which performs a real
kmalloc/write/read/kfree round trip on *its own* CR3 and prints live counters
(`allocs=10 frees=10 live=0 pages=4 bad=0`, pages constant across calls).
## 6. M11 design notes (block layer)

`k64/blk64.c` is a legacy ATA PIO driver, read-only, polling only: no DMA, no
IRQ14/15, no writes (nothing needs to write a disk yet), no partition parsing
(M12's job). Slots 0-3 are ide0 master/slave + ide1 master/slave, and the
device table (model, serial, sector size, capacity, LBA48 capability) is built
once by IDENTIFY / IDENTIFY PACKET DEVICE.

Two sector sizes, and callers must not assume: an ATA disk reports 512, the
boot CD 2048. `blk64_read()` therefore returns into the caller's buffer with
`sector_size` from the device, and the CD's size is left to the filesystem in
M12 (IDENTIFY cannot report it).

Lessons that cost a boot each:

* `rep insw` counts **words**: 512B = 256, 2048B = 1024. Getting this wrong
  reads half a sector and leaves the rest of the caller's buffer stale — which
  is exactly the kind of bug a hash gate catches and a "did it return 0?"
  check does not.
* ATAPI has three phases, and the interrupt reason register says which one you
  are in: after `PACKET` it must read `0x01` (command, host→device: the device
  wants the 12-byte CDB), and only after the CDB must it read `0x02` (data,
  device→host). Checking for "data" one step too early fails every read.
* The byte-count-limit registers must carry the real transfer size; zero is
  not "unlimited" on all implementations.
* An absent unit leaves the status port floating at `0x00`/`0xFF` — fail fast
  instead of burning the DRQ timeout (the 32-bit driver learned this the hard
  way on a missing drive).

Gate: `run64.sh` requires `M11 BLK SELFTEST OK`, the ISO PVD line and the
`lba28 fnv=... lba48 fnv=... same=1` line. The selftest proves internal
consistency (both command paths agree, a re-read is stable, the ISO's PVD says
`CD001`), and `scripts/blk_test.py` proves the bytes are the *right* bytes: it
computes FNV-1a 64 of ISO sector 16 and of disk sector 40 from the host's own
files and requires the guest's hashes to match. The fixture is generated by
`scripts/mk_blkdisk.py` (a pure function of the sector index), so no binary
blob enters the repository.

Not yet gated, and honestly noted as such: addressing beyond LBA28's 128 GiB
(the LBA48 *command path* is exercised, the 48-bit *address range* is not —
that needs a >128 GiB sparse image), multi-sector transfers, and writes.

## 7. M12 design notes (read-only filesystems)

Shape: one filesystem-agnostic core, two backends, one syscall.

* `k64/fs64.c` owns the mount table and every *policy* decision the callers see:
  `fs64_lookup` / `fs64_readdir` / `fs64_read` / `fs64_open`+`fs64_read_fd` /
  `fs64_mount(idx)`, path splitting with `.`/`..`, per-mount roots, and one
  error vocabulary (`-E64_ENOENT`, `-E64_ENOTDIR`, `-E64_EISDIR`, ...).
  Backends register themselves (`fs64_register_backend`) and only implement
  "read this block, list this directory, resolve this name" — so a fat32
  backend later is a new file, not a change to the core.
* `k64/ext64.c` is the ext2 backend: superblock-driven block size (the fixture
  is 4 KiB, not the common 1 KiB), direct + single/double/triple indirect
  blocks, directories as `ext2_dir_entry` runs, read-only. The on-disk formats
  are facts, so the parsing is the one place where the 32-bit `ext2.c` line is
  legitimately worth reusing rather than re-deriving.
* `k64/fs64.c` also carries the ISO9660 backend (mount from sector 16's PVD,
  directory records, `;1` version stripping, extents rounded to 2048B sectors).
* Ring-3 gets `SYS64_FSOP` (139) with a mirrored request struct
  (`fs64_req_t` in `k64/cpu64.h` ↔ `d_fsreq_t` in `demos/sys64.h`) and an
  op-selected stream: `LS`, `CAT`, `STAT`, `HASH`, `MOUNT`. `HASH` exists so a
  filesystem read can be checked against a host-side hash without pushing the
  bytes through a serial console at 115200 baud.

The ISO the build produces has **no Rock Ridge or Joliet**: names are the
level-1 `NAME.EXT;1` form, so `ls /boot` shows `GRUB` and `MYOS64.BIN` with the
version suffix stripped. That is a property of the ISO (GRUB's `xorriso` line
in `Makefile` sets neither), not of the reader.

Three bugs this step found, and why the gate found them:

1. **ATAPI `READ(12)` transfer length.** The block count belongs in the 4-byte
   big-endian field CDB[6..9]; the M11 code wrote it at byte 8 only. Byte 0 was
   correct, so single-read hashes matched and nothing noticed 255 blocks left
   pending — until M12 issued a *second* packet command in one boot and saw a
   stale data phase. A hash proves *content*, not *protocol state*.
2. **The kernel image was not a reserved region.** `mem64` hardcoded
   `[0, 2MB)`, but the 64-bit image carries the embedded demo images, the font
   and the console/GUI buffers, so `.bss` ends past 4 MB. The free-frame pool
   therefore started *inside* the kernel's own `.bss`: a task's stack page and
   the console's shadow grid were the same physical frame. `linker64.ld` now
   exports `_kernel_end` and `mem64_init()` reserves up to it (rounded to 2 MB).
   The tell was that the corruption *moved* when the crashing function's stack
   frame changed.
3. **Blocking syscalls dropped the task's vector state.** The scheduler's
   switch-out save trusts `cur_cpu[]` only while that entry is still `T_RUNNING`
   (the guard that keeps a recycled slot from being clobbered), but `sleep` and
   `waitpid` demote themselves to `T_BLOCKED` *before* scheduling, so their live
   x87/xmm state was never saved and the next `fxrstor` restored the zeros left
   by `exec`. GCC had hoisted the shell's `{magic, op}` constant into `xmm1` at
   startup and reused it at the store site much later, so the first `fs` after a
   `d_sleep(2)` in the key-poll loop sent `magic = 0` and got EINVAL. Fixed with
   `fx_save_self()` at both blocking sites; `fpu64` never caught it because
   `d_yield()` reschedules without demoting. Invariant to keep: **kernel code
   must not touch vector registers** (`CFLAGS64` has `-mgeneral-regs-only`,
   which is what makes "the kernel preserves all registers" true again).

Gate: `run64.sh` requires `M12 FS SELFTEST OK` (mount/readdir/lookup/descend/
read/eof/errors on both images), and `scripts/fs_test.py` compares hashes
computed *host-side* (Python walk of the ISO and of `ext2test.img`) with what
the kernel and Ring-3 report, including a 3-level nested path and a file large
enough to need indirect blocks. The ext2 fixture is built by
`scripts/mk_ext2disk.py` with `mkfs.ext2`/`debugfs`, so it is a real
filesystem, not a hand-rolled approximation.

Not gated, and not implemented: writes, more than one mount per device, ISO9660
long names (Rock Ridge/Joliet), ext2 attributes/htree directories, a page cache
(every `cat` re-reads the device), and a real fd table — the ops are path-based
and serialized by one lock, so a large `cat` stalls that CPU for the transfer.

## 8. M14 design notes (kernel window manager)

M9's desktop was a fixed layout drawn straight into the framebuffer, with a
save-under buffer for the cursor and a documented rule that every bulk pixel
move had to lift the cursor first. That cannot host a second window, so M14
replaces both halves with one mechanism: **the compositor**.

* `k64/gfx64.c` gained a screen back buffer (`gfx_backbuf_alloc`, unpacked
  24-bit RGB words, so a 16bpp panel simply packs on the way out) and
  `gfx_present(rect)`. While the buffer is live every primitive draws into it
  and coordinates stay screen pixels, so the console, the chrome and the text
  code needed no changes at all. `gfx_vgrad_color()` exposes one row of a ramp
  so a partial repaint of the wallpaper lands on exactly the colour the
  full-height gradient would have produced.
* `k64/wm64.c` owns the window table (6 slots), z-order, focus, the desktop
  chrome (wallpaper, top bar, taskbar with one button per window), the
  composited cursor, and a damage list: up to 24 rects, merged on insert.
  `wm64_flush()` re-composites the **whole stack** inside each damaged rect —
  wallpaper, bars, the windows bottom-to-top, then the cursor — and presents it.
  "What is behind this rect" is a function, not a saved copy: that is why
  overlapping windows work, why a drag uncovers the window below correctly, and
  why the cursor can no longer ghost or be overwritten by text (the M9
  hide-before-scroll protocol is gone; `cons_set_hide_hook` is simply not wired).
* A window is a rect plus a contract: a non-console window must be able to paint
  its client **inside the clip it is handed**. The terminal's client needs no
  callback at all — its shadow grid *is* its canvas, so `cons_repaint_rect()`
  re-renders exactly the cells a damaged rect asks for (spaces included, which
  is what erases a scrolled-away line). Ring-3 clients will need a real canvas
  instead (M15): a callback cannot be asked to run from an IRQ-driven composite.
* Mouse input is edge-driven: `mouse64_set_button_hook` reports button changes
  with the position at the change, so a press starts a drag and the motion
  packets that follow continue it — one decision, one repaint.
* `gui64.c` shrank to a shell: open the terminal window and re-home the console
  into it, open the live system window (cpu/tick/heap/fs/frames counters, tied
  to the 1 Hz BSP tick through `gui64_tick()`), wire the mouse and print the
  geometry markers the screenshot gates grep for.

Five bugs this step found. The first two only reproduce under KVM, which is why
they are worth writing down:

1. **`gfx_cell()` trusted its caller.** Every other primitive clips itself, but
   cell drawing did not, and the M14 back buffer is heap VA living at the end of
   the live arena: one coordinate below the screen became an unmapped page and a
   #PF inside the compositor. Stores are now range-checked and the first
   offender is reported by the tick (`wm: gfx rejected N pixel store(s), at
   x,y`) instead of dying silently.
2. **`cons_rehome()` raced a kernel print on another CPU.** It updates
   `c_ox/c_oy` and `c_cols/c_rows` as separate stores while the print path walks
   the same fields; a print landing between them draws with the NEW origin and
   the OLD row count — cells at row 47 of a 36-row view, i.e. below the screen,
   which under M14 is bug 1. Fixed by holding the console lock across the
   transition only (M9 held it across the whole desktop draw) and by bounds-
   checking the cell path. Symptom to remember: passes under TCG, dies under
   KVM at 4 vCPUs.
3. **The fatal dump re-entered the desktop.** The console's dirty hook
   composites on every printed character, so a dump printed with the hook still
   attached re-ran the faulting code: the log filled with the dump's *first
   character* (`[`, or spaces) instead of the dump. The fault paths now detach
   the hooks, freeze the console and park on `pml4_boot` before printing, and
   `vmm_dump_walk()` prints the L4..L1 entries of *both* the faulting root and
   boot's — one walk only ever says "not present", two walks say whether the
   page is missing from one address space or from the pointer itself.
4. **The cursor underline has to be state, not a side effect.** The compositor
   repaints console cells on demand, and `cursor_paint()` notified *before*
   recording that the underline was up, so the repaint it triggered erased the
   underline it had just drawn. `c_cur_on` now says "the cursor cell's pixels
   include the underline"; a repaint of that cell puts it back.
5. **Damage that cuts a console cell in half.** Cell-granular repaint must stay
   inside the rect it was given or it lands on the neighbouring window's frame
   (the M9 gate samples exactly that hairline). Damage touching the terminal's
   client is snapped to the console's cell grid before compositing, and the
   snapped pixels are presented too.

Gate: `scripts/gui_test.py` kept every M9 pixel check (chrome bands, frame
hairline, gradient, single cursor, glyph-exact text) and grew the M14 ones: click
the visible corner of the unfocused window to raise it, drag its title bar and
require the reported position to match, require the rectangle it vacated to be
glyph-exact terminal text again, minimize and restore from its taskbar button,
close it from its title bar and require the terminal to be back underneath — with
exactly one cursor on screen after five clicks. `scripts/heap_test.py` had to
change one assertion: `live == 0` is no longer true now that a screen-sized back
buffer is a legitimate standing allocation, so it checks that live is *constant*
across probes (nothing leaked) and a fraction of the arena.

Not implemented (deliberately): resize by grabbing an edge, window resize
handles, a keyboard input queue per window (keystrokes still only reach
`SYS64_GETCHAR`), per-window canvases for Ring-3 clients, and alt-tab. A window
opened behind the focused one is raised by clicking a visible part of it or its
taskbar button — there is no "click through" policy yet.
