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
| M11 | block layer done 64-bit: ATA PIO read (LBA28/48), `blk_read(sector, count, buf)` for everything above | M4, M10 | `drivers/ata.c` 562 (protocol sequence, polling loop) | sector read hashed against the ISO |
| M12 | a filesystem layer the 64-bit kernel wants: mount, inode/name cache, ext2 read | M11, M10 | `ext2.c` 1065 (on-disk formats are facts — lift the parsing), `vfs.c` 3314 (shape only) | read a known file (e.g. a wallpaper header) with a stable hash |
| M13 | IPC + sync, sized for what the 64-bit design needs: message queue, shared memory, semaphores | M5, M10 | `ipc.c` + `shm.c` + `sync.c` 970 (prior art; the 32-bit shapes here are the weakest part of the old line) | producer/consumer across two tasks |
| M14 | window model in the kernel: N windows, z-order, focus, damage rects, input queue, backing store from the M10 heap | M9, M10 | `wm.c` 1191 (feature list: what a user expects; not an implementation to copy) | drag/resize under load, no ghost pixels (M9 already gated this) |
| M15 | the 64-bit shell/desktop grows into the real UI on M14: multi-window desktop, taskbar, app hosting | M12–M14 | `gui/*` 3218 (visual language: `IC_*` palette, cursor sprite, layout) | driven by `scripts/gui_test.py`-style pixel checks |
| M16 | RTC + assets + persisted settings | M12 | `rtc.c` 98 (CMOS sequence) | clock ticks, settings survive reboot |

Note that only M12's *format* work (ext2/fat32 on-disk layout) is really "reuse
the 32-bit code": it is a spec implementation, and re-deriving it would be pure
waste. Everything else is a fresh 64-bit design that happens to know what the
old line learned. Non-goals for now: `drivers/xhci.c` (1619), `drivers/net.c`
(1449), `sys/watchdog.c`, the `apps/*` GUI apps.

## 5. M10 design notes (kernel heap)

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
