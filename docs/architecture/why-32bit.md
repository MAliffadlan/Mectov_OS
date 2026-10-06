# Why 32-bit only

Status: written 2026-10-06, when the 64-bit kernel was removed from the tree.

This document replaces `x86_64_port.md`. That file was the audit and roadmap for
the 64-bit kernel; the roadmap is dead, but the audit's central measurement is
not — it is a live to-do list for the line that stayed.

## What the 64-bit kernel was

`kernel64.c` + `k64/` — a from-scratch x86-64 kernel, started 2026-09-18 on the
`x86_64-port` branch and promoted to the default build on 2026-09-24 by commit
`5087531`, which set `.DEFAULT_GOAL := all64`.

It reached M14 and worked: long-mode boot, GDT/IDT/TSS, 4-level paging with COW
and NX, SMP on four cores, a preemptive scheduler with fork/exec/waitpid, a
kernel heap with magic and canary, legacy ATA + ATAPI reads, read-only ISO9660
and ext2, a framebuffer console, and a damage-rect window manager. Roughly
12,800 lines across four commits:

| commit | milestone | content |
|---|---|---|
| `5087531` | M1–M10 | bring-up, paging, Ring 3, SMP, scheduler, heap |
| `679e545` | M11 | block layer — legacy ATA PIO + ATAPI, read-only |
| `0567275` | M12 | read-only ISO9660 + ext2 on the M11 layer |
| `0f39b4b` | M14 | kernel window manager — damage-rect compositor |

All four are ancestors of `main`, so the whole port is recoverable from git
history with `git show <commit>:<path>`. The `x86_64-port` branch is fully merged
and 46 commits behind `main`.

## Why it was removed

The promotion was a bet that the compiler was the bottleneck. It was not. The
evidence was in the tree by the time M14 shipped:

| | since `5087531` (51 commits) |
|---|---|
| commits touching `src/` + `kernel.c` | **36** |
| commits touching `k64/` + `kernel64.c` | **4** |

Every feature that made this OS worth using was being built in 32-bit and none
of it existed in 64-bit: the TLS 1.3 engine, the Mini Browser's HTTPS, the
writable FAT32/ext2 paths, xHCI, AHCI, virtio, and Quake III Arena playable end
to end. A second kernel that could boot and draw a window, while the real one
gained a working internet, was not a second kernel worth having — it was a
second kernel to maintain and to document, and it already had visible drift
(`Makefile` claiming `clean_all` covered "both lines" when one of them was a
dead end).

M13, M15 and M16 — IPC/sync, the real UI and app hosting, RTC and persisted
settings — were never started. Reaching them would have meant rebuilding
networking, audio, USB, SATA, PCI enumeration, a writable filesystem, an fd
table and a page cache from nothing, while Quake III and the browser waited.

The 32-bit kernel is the one that runs, and it is the one that continues.

## What the port measured that still matters

The port compiled every `src/**/*.c` under the 64-bit flag set and counted the
warnings that only a 64-bit compiler can emit. Those warnings are still real on
the 32-bit line — a cast that truncates today is a cast that corrupts memory the
day anyone widens the type it feeds.

Reproduce it (no build needed, no output written):

```bash
FLAGS="-m64 -std=gnu99 -ffreestanding -O2 -march=x86-64 -mcmodel=kernel \
       -mno-red-zone -fno-pie -fno-pic -mgeneral-regs-only"
for f in $(git ls-files 'src/*.c'); do
  gcc $FLAGS -Wpointer-to-int-cast -fdiagnostics-plain-output -c "$f" -o /dev/null 2>&1 |
    grep pointer-to-int-cast
done
```

Measured on 2026-10-06, over 159 files: **632 warnings emitted, but only 112
distinct source locations.** The gap is the inline `sys_*` wrappers in
`src/include/syscall.h` — each one is re-reported by every translation unit that
includes the header, so the per-file counts in a naive tally are inflated
several times over. Count distinct `path:line`, not warnings.

| file | distinct `-Wpointer-to-int-cast` sites |
|---|---|
| `src/include/syscall.h` (inline `sys_*` wrappers) | 49 |
| `src/sys/idt.c` | 35 |
| `src/sys/task.c` | 6 |
| `src/sys/syscall.c` | 5 |
| `src/sys/mem.c` | 4 |
| `src/sys/gdt.c` | 3 |
| `src/sys/smp.c`, `src/gui/wm.c`, `src/drivers/gdb_stub.c` | 2 each |
| `src/sys/syscall_proc.c`, `panic.c`, `acpi.c`, `src/drivers/sb16.c` | 1 each |

The original audit recorded **301** on 2026-09-18, when `src/` was 149 files and
predated the TLS engine, the Mini Browser, the FAT32 write path and the Quake III
port. The per-file figures for `idt.c`, `mem.c`, `wm.c` and `smp.c` reproduce
exactly; the totals grew because the code did.

At 32 bits a truncating store is usually a no-op, so these are latent rather than
live bugs — but they concentrate in exactly the structures where a truncation
would silently become a wild pointer: `task_t`'s CR3 and page-directory fields,
`idt`'s gate tables, and the fd and syscall argument paths. Worth an audit; not
worth a second kernel.

The port also left two pieces worth keeping as reference even without a 64-bit
kernel: a kernel heap with magic + canary (`k64/heap64.c`) and a damage-rect
compositor over a screen back buffer (`k64/wm64.c`). Both exist in the 32-bit line
in other shapes — `sys/mem.c`'s block list and `gui/wm.c`'s full recomposite per
frame respectively.

## Hardening still missing in 32-bit

Recorded here because the port was the thing that surfaced them:

| protection | 32-bit state |
|---|---|
| SMEP | yes — `sys/mem.c` `paging_enable_smep()`, CR4 bit 20 |
| SMAP | **no** |
| KASLR (user PIE) | yes — ASLR window in `sys/loader.c` |
| KASLR (the kernel itself) | **no** — linked at a fixed 1 MB (`linker.ld`), never relocated |
| stack canary | heap only (`sys/mem.c`), not kernel stacks |
| swap / page-out | **none** — exhaustion is an OOM, never a page-out |
