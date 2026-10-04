#ifndef MECTOV_VERSION_H
#define MECTOV_VERSION_H

// Single source of truth for the release string.
//
// Kernel-side code reaches it through utils.h (which includes this file). Ring 3
// apps include it directly: apps/terminal.c prints the *running* version in its
// banner, and it used to carry a hardcoded copy that went stale for many
// releases — a current ISO still introduced itself as "v36.3", which is exactly
// the kind of thing that makes someone doubt the build they are looking at.
// v38.148: the number jumps from 138 because the working tree already carries
// change tags v38.139 through v38.147 (ext2 doubly-indirect, the vfs node table
// widening, the 40 Hz pacer, the sound-tap removal, the sky drawn last, the
// two-buffer present in q3cl_render, the pose grid). Those changes shipped in
// this same tree without ever getting a release row, so 139-147 are spoken for
// in the source and reusing one of them would make "which release introduced
// this" unanswerable. 148 is the first number that is free; 149 follows it.
// v38.150/151 (source tags only, same batch): game-task present + present lock,
// adaptive {25,50}ms pacer, atomic mouse drain, default sens 0.10, deferred
// 1 Hz compose, raise-skip, MAIN-after-quit. Bumped so a session can prove it
// runs this batch: if the terminal still says 38.149, the ISO is stale.
// v38.153: q3_memcpy/q3_memmove/q3_memset were byte-at-a-time loops, and the
// whole TinyGL tree's <string.h> aliases to them (stubs/string.h), so every
// per-frame framebuffer copy and clear ran one byte per iteration at -O1 with
// -mno-sse -mno-mmx (nothing for GCC to vectorize with). Now word-wide with
// `rep movsl`/`rep stosl`, matching src/sys/utils.c in this same tree. Measured
// at SCALE=2: gl_ms 1789 -> ~350 and other_ms 1038 -> ~330 per 100 frames.
// Bumped so a session can prove it runs this batch: terminal must say 38.152.
//
// v38.156: ACPI table pointers are now validated against the span paging_init()
// actually mapped, not a stale 256MB constant. Every `-m 512` guest (the one
// every Q3 session runs) puts its RSDT in the reserved strip at the top of RAM
// (0x1FFE1D6F), which the old bound rejected — so no MADT, no APs, and the game
// task, the compositor, the shell and the busy-wait serial writes all shared
// one core. That is the `/quake` session's "selalu patah patah" with `slack`
// ~22 ms in every [Q3HIT] line. 150-155 are source tags already spoken for in
// this tree (see the block above); 156 is the next free number.
// v38.157: two changes that came out of the first 4-CPU KVM session on the
// v38.156 ISO (the ACPI/SMP fix turned the APs on, and the q3dm1 launch then
// exposed what one core had been hiding):
//  1. The VFS node table (2048 sectors = 1 MB) is written in batched
//     multi-sector commands instead of one single-sector PIO command with a
//     CACHE FLUSH each. vfs_create_node -> vfs_save on the game's own startup
//     mkdirs held vfs_lock with IRQs off for seconds; the SMP watchdog caught
//     CPU 1 mid `rep outsw` in ata_write_sector_drive_io and panicked the
//     boot. Now 16 transfers (DMA when present), tens of ms.
//  2. Serial output is an asynchronous transmit ring drained from every core's
//     timer tick. The old path busy-waited THRE per byte with IRQs off, so a
//     550-byte diagnostics line froze the frame around it (measured ~2.4 ms/
//     frame and a ~47 ms clump every 20 frames; the mouse queue emptied in one
//     burst after each clump). Every byte still reaches the log, panic paths
//     flush the ring first.
// 150-156 are spoken for (see above); 157 is the next free number.
//
// v38.158: a CI repair release; the kernel itself is unchanged except that the
// Q3 diagnostics the suites assert on exist in the binary again. The GitHub
// Actions run on the merge commit (37189377311, 4 Oct) was red in two jobs:
//  1. kernel64: `make iso64` died at `grub-mkrescue: error: `mformat`
//     invocation failed` because that job never installed mtools — it landed
//     with a comment claiming mtools was not needed while the 32-bit build job
//     installs it and proves the opposite. It had been red since 1 Oct.
//  2. quake3: the HUD step failed at "[FAIL] the status bar was never
//     sampled" — v38.145 folded the play/hud/sky/viewmodel lines behind
//     `#ifdef Q3_DEEP_DIAG` on the belief that no suite parsed them, while
//     q3hud/q3sky/q3jump/q3viewmodel/q3cull all do, and no build anywhere
//     defines that macro: the evidence existed in no binary at all. The block
//     is compiled again; the lines nothing parses stay gated.
// Two harness repairs rode along: the stale `Q3 sounds` CI step and the
// `check-q3snd` target (the script was retired in v38.142), and two suite
// races (q3viewmodel waiting for a line's head under v38.157's asynchronous
// serial ring, and q3hud's unpinned frame-20 histogram A/B).
//
// The q3hud flake turned out to be an OPEN renderer defect, not a HUD leak,
// and the suite now measures it instead of tripping over it: at the same
// pinned eye one boot in two draws 12 extra faces — `drawn=38 tris=76
// back=34 untrusted=32` where a healthy boot reads `drawn=26 tris=52 back=46
// untrusted=8`, constant from frame 20 to frame 100 — the backface
// reject's "the file's normals disagree" escape hatch firing on 24 extra
// triangles — and those pixels are exactly the ones that move between the
// `patch` and `wall` histogram buckets while cyan/stepgreen/violet/bright do
// not move at all. That defect is named here and in the README row, not
// hidden: the check asserts the level's palette exactly and its covered area
// to within 1%, and reports the warm-bucket split it sees.
// 150-157 are spoken for (see above); 158 is the next free number.
#define OS_VERSION "38.158"

#endif
