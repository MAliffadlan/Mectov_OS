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
// v38.159: write errors stopped being swallowed — finding F1 of the audit
// report (docs/audit/architecture-review-2026-10-04.md). The root cause sat one
// layer below where the report placed it: ata_write_sectors_drive_io() and
// ata_write_sector_drive_io() polled only BSY/DRQ and never read ERR/DF in the
// status register, so a write command that completed WITH AN ERROR still looked
// like success. ata_status_error() now checks the status register after the
// command completes on all three write paths (multi-sector PIO, single-sector
// PIO, DMA) and returns -1; ext2/fat32 write helpers became int and every
// internal caller (bitmaps, dirents, FAT copies, superblock/inode sync) checks
// and rolls back; vfs_save() and the VFS data path propagate the refusal to the
// syscall's return value instead of storing a size for bytes that never landed.
// Release-only paths (free_block/free_chain/...) still log and continue by
// design — refusing to free during recovery would leak worse.
// Proof: scripts/wfail_test.py + apps/wfiledemo.c — with QEMU blkdebug refusing
// every write_aio (errno EIO) the pre-fix guest printed "WFILE write ok" while
// the medium held nothing; the post-fix guest reports the failure (write
// ok=False, WFILE create FAILED) and mtools confirms no file on the medium. The
// no-injection control boot stays green. `make check-wfail` runs both.
// 150-158 are spoken for (see above); 159 is the next free number.
// v38.160: audit finding F0 is closed — v38.92's single "pre-existing
// cross-core race" was two independent defects.
//  1. FIXED (app) — the recorded symptoms (items lost/duped, ~1 run in 4-6 on
//     2+ cores; the `FAIL consumed count` / `FAIL missing/duplicate items` the
//     audit reproduced on 2026-10-04) were an app-level mutual-exclusion hole:
//     apps/conddemo.c's consumer loop carried a SECOND mct_mutex_unlock(&c_mu),
//     added by that same v38.92 commit. The first unlock's lock=0 store + futex
//     wake lets a peer acquire the mutex; the stray second store then releases
//     it again while that peer is inside — two threads in the critical section.
//     On one core the stray store is harmless (its owner reacquires before any
//     peer runs), which is exactly why SMP1 stress stayed green. One unlock now.
//  2. FIXED (kernel) — the intermittent 4-core stall (2nd-or-later demo run in
//     one boot, ~1 in 20 rounds) was scheduler slot-reuse corruption, and the
//     v38.160 post-mortem pinned all of it rather than inferring it: the stuck
//     consumer (tasks[8]) was READY and queued in rq[0] while its saved frame
//     was the kernel EXIT PARK LOOP (EIP=task_exit_with_code+0x71, the
//     `for(;;) hlt` a task reaches after SYS_EXIT), its parent was parked in
//     waitpid(8) with waiting=8, zombie_since=0 (that thread never exited
//     itself) and the futex table was empty. Root cause: a task that exits
//     stays current on its core until that core's NEXT tick —
//     task_exit_with_code can only park in a hlt loop, and the switch happens
//     in schedule(), i.e. <=10 ms at 100 Hz. When the slot is reclaimed inside
//     that window the stale core's tick wrote its dead frame into the RECYCLED
//     slot (`tasks[cur].esp = esp`), overwriting the new thread's freshly built
//     frame; the next pick then resumed the new thread into the dead hlt loop,
//     so it never ran a single instruction, its parent's waitpid never
//     completed and the round died with no verdict. conddemo hits that window
//     by construction: the last mutex worker exits, join() returns microseconds
//     later and the producer/consumer phase clones straight back into the same
//     slots. Round 1 of a boot is always safe (no slot has a predecessor yet),
//     which is why every single-run CI step and every first run stayed green.
//     Fix (src/sys/task.c): schedule() skips every write to a dead current task
//     (esp, watermark, state, runqueue, FPU image, tick accounting) and, with
//     nothing runnable, parks the core on its own idle task instead of iret'ing
//     a dead frame; rq_enqueue refuses dead tids and the commit path refuses a
//     dead `next`; task_set_state refuses to revive FREE/ZOMBIE; sem_destroy
//     filters dead/idle waiters (the last unguarded READY write in sync.c); and
//     every slot-claiming path (clone/fork/fork_exec/idle) skips a FREE slot
//     that a core still names as its current task (slot_still_current).
//     Diagnostics: throttled `[WATCH]` lines (dead-cur, create-skip,
//     revive-refused, dead-in-rq, parked-frame) plus a 1 Hz integrity sweep on
//     the BSP; scripts/cond_test.py FAILS the run if a parked-frame line ever
//     appears (a task resumed into dead code) and reports create-skip as
//     survived evidence, not a failure.
//     Verified: `--repeat 60` on 4 vCPUs (TCG) = 60/60 rounds ALL PASS — the
//     same shape that stalled at round 36 before the fix — with a dead-cur
//     event in every round and one `create-skip tid=8` (the clobber window
//     demonstrably fires and is now survived). A second run, `--repeat 30`, is
//     green too. Regressions on this tree: make check-quick 12/12, boot_test
//     rc=0.
// scripts/cond_test.py gained --repeat N (N rounds in one boot; per-round
// serial offsets so a later round cannot consume an earlier round's markers,
// per-round terminal re-focus), --smp N, the stall-signature assertion above,
// a full QEMU-monitor post-mortem on a stall (registers, current_task, rq[0..3],
// tasks[], both kstacks, the futex table) and a final
// "[OK] cond_test: smp=4 runs=N/N ALL PASS" gate-shape line. CI's cond step is
// unpinned (MCTOV_SMP=4) now that both bugs are fixed; the deep multi-round
// stress stays a local command, documented in README.
// 159 is spoken for; 160 is the next free number.
// v38.161: the audit's remaining findings
// (docs/audit/architecture-review-2026-10-04.md, F2-F11) are closed in one
// pass — and one of them turned out to be wrong.
//   F2  docs/architecture/scheduler.md said MAX_TASKS = 32; src/sys/task.c runs
//       64 (MAX_CPUS is 16). Fixed.
//   F3  The VFS node limit was quoted as 64 (memory.md) and 256 (README's
//       v38.44 row). Both stale: src/include/vfs.h is 2048, layout v5 since
//       v38.141. The README's current-capacity bullet says 2048 and points at
//       the code now.
//   F4  memory.md's intro promised "two-level x86 paging" while the same file
//       documents the v38.49 PAE 3-level migration. Intro fixed.
//   F5  memory.md said init_mem() walks the Multiboot mmap and rounds the page
//       count up; the code takes the scalar mem_size from the Multiboot header
//       and floors it (mem_size / PAGE_SIZE). Rewritten, with the v38.156
//       RAM-vs-mapped-span lesson attached — that gap was a real boot bug.
//   F6  scripts/doom_test.py's seed pre-flight printed [skip] and returned when
//       debugfs was missing or failed, silently removing the discrimination it
//       exists for (a missing/truncated doom1.wad was historically the #1
//       "never entered game loop" confounder). It now FAILS (exit 2) with an
//       install hint, and debugfs joined check.py's TOOLS preflight.
//   F7  scripts/check.py was the last copy of the KVM probe that decided with
//       exists()+os.access(); /dev/kvm can exist and still fail to open (the
//       reason a969f84 migrated kbd/heap/cons/gui). It opens the node now.
//   F8  src/sys/entropy.c's CHACHA_ROUNDS counter actually counted DOUBLE
//       rounds (8 -> the full 16-round permutation), so the box was stronger
//       than its name; the macro is CHACHA_DOUBLE_ROUNDS now. get_random_u32()
//       also returned a hard 0 while the pool was unseeded — a constant where
//       ASLR and the first TCP ISN asked for randomness; it retries, then
//       falls back to a TSC-mixed value that is never constant and never zero.
//   F9  src/gui/wm.c's wm_lock_release() decremented to 0 and cleared the lock
//       unconditionally, so an unbalanced release could silently drop another
//       task's critical section. Depth 0 now logs a throttled
//       [WATCH] wm-unbalanced-release and changes nothing.
//   F10 A "0 warning" clean build still printed 36 `ld: ... RWX permissions`
//       notes (one per Ring 3 payload) plus two rustc warnings. The notes are
//       intentional (W^X covers user heap/stack/mmap; a loaded .mct keeps its
//       text executable) and the 64-bit linkers already passed
//       --no-warn-rwx-segments — the 32-bit app linkers do too now. The rustc
//       step no longer inherits MAKEFLAGS (rustc cannot use make's jobserver,
//       which printed a "Bad file descriptor" note on every incremental build)
//       and no longer names the unknown/unstable `mmx` target-feature.
//   F11 CORRECTED, not fixed: the claim that task_reap_zombies() is dead code
//       came from a grep of src/ only — the callers are in kernel.c, at both
//       1 Hz BSP ticks (lines 650 and 1005). The reaper runs, so
//       zombie_reap_ms and /proc/sys/zombie_reap_ms are live, and that also
//       explains F11's "unexplained" dump detail: the FREE slot with a stale
//       zombie_since was the reaper freeing a zombie whose parent never called
//       waitpid(), after the 15 s timeout. A throttled [WATCH] free-zombie
//       line (watch kind 8) now records it when it happens.
// Also in this release: three README version rows carried unescaped `|` inside
// code spans (v38.128's map|clampmap|animMap and tcMod scroll|scale, v38.50's
// PCD|PWT, v38.41's OSFXSR|OSXMMEXCPT) — escaped now, so the table renders.
// The "12 rows" the audit published was a scan artefact that counted already
// escaped \| as separators.
// 160 is spoken for; 161 is the next free number.
#define OS_VERSION "38.161"

#endif
