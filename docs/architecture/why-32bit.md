# Why 32-bit only

Status: written 2026-10-06, when the 64-bit kernel was removed from the tree.

This document replaces `x86_64_port.md`. That file was the audit and roadmap for
the 64-bit kernel; the roadmap is dead, but the audit's central measurement is
not — it still describes the line that stayed, and it was measured again below.
(Audited 2026-10-23: the pointer-truncation count is not a to-do list after all —
every site is 32-bit-correct, most are hardware-mandated, and the reason to keep
counting is as a ratchet against new ones. See "What the port measured".)

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
warnings that only a 64-bit compiler can emit.

**This section used to call them a to-do list. They are not one.** Audited
2026-10-23, every site is correct at 32 bits — and the great majority are not
oversights at all, they are what the hardware or the ABI *requires*:

| cluster | sites | why 32-bit is mandatory, not a bug |
|---|---|---|
| `src/include/syscall.h` (inline `sys_*`) | 49 | the `int 0x80` ABI passes arguments in registers |
| `src/sys/idt.c` | 35 | the x86 IDT gate format has 32-bit base fields |
| `src/sys/task.c` (`esp`/`eip`) | 6 | ESP and EIP are 32-bit registers |
| `src/sys/syscall.c` | 5 | TSS `esp0` is 32-bit; user-pointer validation |
| `src/sys/mem.c` | 4 | `heap_panic` diagnostic arguments |
| `src/sys/gdt.c` | 3 | GDT descriptor `base` is 32-bit by hardware format |
| `src/drivers/sb16.c` | 2 | ISA DMA takes a **24-bit** physical address |
| `src/drivers/smp.c` | 2 | AP trampoline parameters must live below 1 MB |
| `gdb_stub.c`, `wm.c`, `panic.c`, `acpi.c`, `syscall_proc.c` | 7 | logging / diagnostics only |

**Do not "fix" a cast in the mandated clusters by widening it to `uintptr_t`.**
The IDT gate base, the GDT descriptor base, `esp0`, `esp`/`eip`, the ISA DMA
address and the trampoline parameters are 32-bit *because the interface they
feed is*, and writing 64 bits there would corrupt the structure rather than
protect it. The remaining sites are diagnostics that print an address.

So there is no live bug here, and the claim this section used to make — "a cast
that truncates today corrupts memory the day anyone widens the type it feeds" —
only bites if someone widens a type, which the 32-bit hardware line does not
invite. What the count is actually good for is catching a *new*, careless
truncation. `scripts/ptr_trunc_audit.py` does that and nothing else:

```bash
python3 scripts/ptr_trunc_audit.py          # report, and fail if the count grew
python3 scripts/ptr_trunc_audit.py --list    # every site, path:line
python3 scripts/ptr_trunc_audit.py --update  # print a new BASELINE line
```

It is a **ratchet**: CI runs it in the `build` job, and it fails when the number
of distinct sites rises above the recorded baseline. A fall is reported but does
not fail — it is a fix, not a regression — with a note to lower the baseline so
the next growth cannot hide in the slack.

Measured 2026-10-23, over 165 files: **113 distinct sites** (696 raw warnings).
The gap is the inline `sys_*` wrappers in `src/include/syscall.h`: each is
re-reported by every translation unit that includes the header, so a per-file
tally is inflated several times over. The script counts distinct `path:line`,
never warnings, and filters the `In file included from` chains — matching those
loosely is how a first attempt counted 388.

The original audit recorded **301** on 2026-09-18, when `src/` was 149 files and
predated the TLS engine, the Mini Browser, the FAT32 write path and the Quake III
port. The 2026-10-06 pass recorded 112; the only site added since is a second one
in `src/drivers/sb16.c`. The per-file figures for `idt.c`, `mem.c`, `wm.c` and
`smp.c` reproduce exactly across all three passes.

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
| SMAP | **no** — SMAP is not available in 32-bit mode (Intel SDM); the i386 line cannot set CR4.SMAP |
| KASLR (user PIE) | yes — ASLR window in `sys/loader.c` |
| KASLR (the kernel itself) | **no** — linked at a fixed 1 MB (`linker.ld`), never relocated |
| stack canary | yes — kernel C is built with `-fstack-protector-strong -mstack-protector-guard=global`; `boot.asm` seeds `__stack_chk_guard` from RDTSC, and `__stack_chk_fail` runs the panic tail |
| swap / page-out | **none** — exhaustion is an OOM, never a page-out |
