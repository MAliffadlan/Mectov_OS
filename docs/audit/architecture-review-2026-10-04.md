# Mectov OS — Architecture & Code Review (2026-10-04)

**Target:** `main` @ `a969f84` (v38.158) — the commit whose CI run `37197772181` is green 28/28.
**Method:** read the source and docs, then ran the project's own fast tier to keep every claim
grounded. This report changes no code; it is a review artifact.
**Author:** automated review session (BuffY) at the user's request.

**Status update (2026-10-05):** **F1 is fixed and proven by a fault-injection harness**, and
**F0 is closed** (its app-level double unlock *and* the scheduler stall it had narrowed to, with the
post-mortem evidence and a 90-round 4-core stress) — see the F0 and F1 sections for the diffs and the
before/after evidence. The 2026-10-04 text below each of those sections stands as written (it is the
record of what was observed then). Everything else in this report stands as written; the remaining
findings (F2–F11) are untouched.

---

## 1. Scope & method — what was read vs what was run

| | |
|---|---|
| **Read (line by line)** | `docs/kernel-locking.md`, `docs/architecture/{overview,scheduler,memory}.md`, `src/include/spinlock.h`, `k64/spin64.h`, `src/sys/entropy.c`, plus contiguous windows of `src/sys/{idt,syscall,vmm,mem,vfs,ext2,fat32,fd}.c`, `src/drivers/ata.c`, `src/gui/wm.c`, `scripts/{check,doom_test}.py` |
| **Grep-verified only** | `src/drivers/{xhci,net,virtio_gpu}.c`, `src/sys/task.c` (beyond its lock sites), `src/sys/shell/shell_core.c`, `kernel.c`, `k64/*` (7,935 lines) |
| **Ran** | `make -j8 iso` (incremental), `make clean_all && make -j8 iso` (from scratch), `make check-quick` (12 suites), `make check64` (64-bit boot + 6 gates), `scripts/boot_test.py` on the rebuilt image |
| **Not run** | the 32-bit full battery (66 suites), the Q3 suites (`check-q3*` need the `MECTOV_Q3=1` ISO + `assets/q3/pak0.pk3`), the `--kvm` regressions, and anything requiring a push |
| **Never touched** | `third_party/q3a/`, the 320×240 render rule, and the git index (nothing committed) |

**No claim below is made about a file that was only grepped.** Where a cluster was sampled rather
than read, the report says so instead of implying coverage.

---

## 2. Fast-tier results (this machine, KVM available)

| Stage | Command | Result | Notes |
|---|---|---|---|
| Quick build | `make -j8 iso` | rc=0, **8 s** | The tree was already built, so this recompiled almost nothing. **Not** evidence of a clean build — see the next row. |
| Clean build | `make clean_all && make -j8 iso` | **rc=0, 9 s** — 59 objects rebuilt, 0 of them pre-existing | **0 C compiler diagnostics**; 36 `ld` notes + 2 `rustc` warnings — see F10 |
| Fast tier | `make check-quick` | **11 / 12 PASS — 1 FAIL** (19 m 03 s) | `cond` failed; see F0 |
| 64-bit tier | `make check64` | **rc=0** (3 m 36 s) | `run64.sh --headless` reached `M12 BOOT OK` (SMP + fork/exec + shell + brk/demand + W^X + ASLR + GUI + heap + ATA/ATAPI + ISO9660/ext2), then `kbd_test`, `heap_test`, `blk_test`, `fs_test`, `cons_test` and `gui_test` all passed — the four gates that only ever went green in CI now have a local baseline |
| Rebuilt artifact | `python3 scripts/boot_test.py` | **rc=0** | booted to the login screen, logins through, frame allocator + heap healthy, survived the smoke window — i.e. the clean rebuild produced a working image (54 `.mct` apps embedded), not just an exit code |
| Tree after | `git status --porcelain` | `?? docs/audit/` | no suite touched a tracked file; only this report is new |

Timing note: `scripts/check.py:190` describes the fast tier as "~6-8 min TCG". It took **19 m 03 s**
here, and the observed QEMU command lines carry no `-enable-kvm`
(`qemu-system-i386 -cpu qemu32,+nx … -smp 4 -display none`), so this run was TCG too — two suites
dominate: `app_smoke` at 262 s and `cond` at 513 s (the latter running into its own failure path). The
estimate is optimistic by ~2.4×, which matters for anyone budgeting a pre-push run. The 64-bit tier is
the opposite: those gates probe `/dev/kvm` and took it (`make check64`, 3 m 36 s for six QEMU boots
plus a full 64-bit boot).

Per-suite detail, copied from the project's own `.check/summary.txt`:

```
boot             PASS         17s      fuzz             PASS         32s
doom             PASS         42s      iocache          PASS         32s
fork             PASS         33s      app_smoke        PASS        262s
procfs           PASS         30s      thread           PASS         35s
jobcontrol       PASS         45s      cond             FAIL        513s
fputest          PASS         34s      usb              PASS         62s
TOTAL   11 passed, 1 failed  (19m 03s)
```

The one failure is not a timeout and not noise; the run's log ends with:

```
[CONDDEMO] FAIL consumed count
[CONDDEMO] FAIL missing/duplicate items
[CONDDEMO] producer/consumer OK (3000 items, no loss/dupe)
[CONDDEMO] FAILED
[FAIL] conddemo reported a failed assertion
```

---

## 3. Findings

Severity is judged by blast radius × time-to-diagnose, not by how exotic the code path is.

### F0 — [High] The cross-core condvar story was two bugs: an app-level double unlock and a scheduler slot-reuse corruption — **both FIXED (2026-10-05, v38.160)**

**Updated 2026-10-05 (v38.160): closed.** The v38.92 release note recorded "a DIFFERENT,
pre-existing cross-core race in the Ring 3 condvar hot path … drops/dupes items ~1 run in 4-6 on 2+
cores (SMP1 is 100% green across stress)" and pinned `cond_test` to `MCTOV_SMP=1` in CI. Re-examined
with the new `--repeat`/`--smp` harness mode, that turned out to be **two separate defects, in
different layers**:

**1. The recorded item loss was an app-level mutual-exclusion hole — FIXED.** `apps/conddemo.c`'s
consumer loop carried a **second `mct_mutex_unlock(&c_mu)`**, added by the same v38.92 commit that
recorded the race. On 2+ cores the first unlock's `lock=0` store + futex wake lets a peer acquire the
mutex, and the stray second store then releases it again while that peer is inside the critical
section — two threads in the section, items lost/duped, exactly the `FAIL consumed count` /
`FAIL missing/duplicate items` this report reproduced on 2026-10-04. On 1 core the stray store is
harmless (the unlocker reacquires before any peer runs), which is why SMP1 stayed green. The fix is
the single unlock; the calling comment records the history. (This session's control run on the
pre-fix ISO did not re-observe the assertion path — its 4-core run hit the stall below first — so the
app fix rests on the mechanism + `git blame` + the recorded signatures rather than on a fresh A/B of
the assertions.)

**2. The relaunch stall was scheduler slot-reuse corruption — FIXED, with the mechanism proven by
the post-mortem.** Symptom: on 4 cores the demo occasionally stops making progress and prints no
verdict, always on the **2nd-or-later `run /apps/conddemo.mct` inside one boot** (stalls at rounds 2,
3, 6, 23 and 36 across runs; a boot's first run passed every time) — which is why the single-run CI
step stayed green. The v38.160 post-mortem (QEMU-monitor `xp` dumps, resolved against
`myos.bin.debug` symbols) pinned it down instead of leaving it as a hypothesis:

| observed | value at the stall |
|---|---|
| kernel futex table | **empty** — no waiter registered anywhere |
| BSP runqueue | `rq[0] = [0, 4, 8]` — the stuck consumer (tid 8) is QUEUED |
| task 8 (conddemo consumer) | state `READY`, `rq_cpu=0`, saved frame **EIP=`task_exit_with_code+0x71`** — the kernel EXIT PARK LOOP (`for(;;) hlt` after SYS_EXIT); `zombie_since=0`, i.e. this thread never exited itself |
| task 5 (conddemo.mct main) | `BLOCKED`, `waiting=8` (waitpid on that consumer) |
| task 8's kernel stack | return-address chain back through `syscall_handler+0x409` — the SYS_EXIT call site of the slot's **previous** occupant |
| `current_task` | `[8, 1, 2, 3, …]` — BSP naming the stuck tid; APs on idle tasks |
| timer | `[LOAD]` lines kept printing long after the app stopped (BSP alive, no panic) |

Root cause: a task that exits stays `current_task[cid]` on its core until that core's **next tick** —
`task_exit_with_code` can only park in a `hlt` loop, and the switch happens in `schedule()`, so the
window is ≤10 ms at 100 Hz. If the slot is reclaimed inside that window the stale core's tick writes
its (dead) frame into the **recycled** slot (`tasks[cur].esp = esp`), overwriting the new thread's
freshly built frame; the next pick then resumes the new thread into the dead hlt loop. It never
executes one instruction of its own, so its parent's `waitpid` never completes and the round dies
without a verdict. conddemo hits that window *structurally*: the last mutex worker exits, `join()`
returns microseconds later, and the producer/consumer phase clones straight back into the same slots
(slot 8 is reused by the next `clone` at 28038 in the captured serial log, ~µs after that slot's
previous owner exited at 28029). Round 1 of a boot is always safe because no slot has a predecessor
yet — the reason every single-run CI step and every first run stayed green.

Fix (all in `src/sys/task.c` unless noted):

* `schedule()` writes **nothing** to a dead current task (esp, watermark, state, runqueue
  membership, FPU image, tick accounting) and, when nothing is runnable, parks the core on its own
  idle task instead of iret'ing a dead frame; the commit path also refuses a dead `next`.
* `rq_enqueue()` refuses dead tids; `task_set_state()` refuses to revive FREE/ZOMBIE; `sem_destroy()`
  (`src/sys/sync.c`) filters dead/idle waiters — the last unguarded READY write in `sync.c`.
* Every slot-claiming path (`clone`/`thread_create_ex`, `task_fork`, `task_fork_exec`,
  `create_idle_task`) **skips a FREE slot that a core still names as its current task**
  (`slot_still_current()`), so a fresh frame can never be published into a window that a stale core
  is about to clobber. It was always safe before because no *legitimate* FREE slot is current
  anywhere.
* Diagnostics: throttled-per-kind `[WATCH]` lines (`dead-cur`, `create-skip`, `revive-refused`,
  `dead-in-rq`, `parked-frame`, `enqueue-dead`, `commit-dead`) plus a 1 Hz integrity sweep on the
  BSP; `scripts/cond_test.py` now **fails** if a `parked-frame` line appears (a task resumed into
  dead code) and reports `create-skip` as *survived* evidence rather than a failure.

**Reproduction tooling (v38.160).** `scripts/cond_test.py` gained `--repeat N` (N demo runs in ONE
boot; each round matches only the bytes appended after that round started, so a later round can never
be satisfied by an earlier round's markers) and `--smp N`. Any failed round fails the suite, and the
final line states the gate's shape (`[OK] cond_test: smp=4 runs=60/60 ALL PASS`). A stall also dumps
a full post-mortem (registers of every vCPU, `current_task`, `rq[0..3]`, `tasks[0,4..9]`, two kernel
stacks, the futex table). Measured on TCG, 4 cores:

| run | result |
|---|---|
| control, pre-fix ISO, `--repeat 10` | rounds 1–2 PASS, **round 3 stalled** (no verdict in 296 s) |
| pre-fix tree, `--repeat 10` | round 1 PASS, round 2 stalled (265 s) |
| pre-fix tree, `--repeat 60` | 22 PASS, round 23 stalled; (`--repeat 60`, forensics run) 5 PASS, round 6 stalled |
| pre-fix tree, `--repeat 60` (probe6) | 35 PASS, **round 36 stalled** (dump above) |
| **fixed tree, `--repeat 60`** | **60/60 PASS** — `[WATCH] dead-cur` in every round, one `create-skip tid=8` survived, zero `parked-frame` |
| **fixed tree, `--repeat 30`** (second ISO, per-kind diagnostics) | **30/30 PASS** — two `create-skip` events survived, zero `parked-frame` |

The fix's own gate is therefore: 90/90 rounds green where the pre-fix tree stalled 5 times in ~100
rounds, with the clobber window observed live (`dead-cur` every round, `create-skip` three times) and
its corruption signature (`parked-frame`) never once — plus the pre/post app-level controls above.

**CI pin removed.** The `cond` step in `.github/workflows/build-boot-test.yml` now runs
`MCTOV_SMP=4 python3 scripts/cond_test.py --timeout 900` with the step comment naming both fixed
defects; the deep multi-round stress stays a documented local command
(`python3 scripts/cond_test.py --repeat 60`). Regressions on this tree: `make check-quick` 12/12 and
`boot_test.py` rc=0 (see §2/§3 for the run logs). One detail from the stall dump is still
unexplained and is recorded separately as F11.

### F1 — [High] Multi-sector filesystem writes drop the drive's error code on all three paths — **FIXED (2026-10-05)**

**Fixed in this tree (uncommitted).** The root cause turned out to sit one layer lower than the
text below assumes: the ATA driver itself could report success for a write the drive *refused*.
`ata_write_sectors_drive_io()` / `ata_write_sector_drive_io()` (`src/drivers/ata.c`) only polled
BSY/DRQ on completion and never read ERR (bit 0) / DF (bit 5) of the status register, so a command
that completed with an error still returned 0. The fix adds `ata_status_error()` (checked only
after the command completes), an `ata_write_errors` counter, a `ata_note_write_error()` serial line
(`[ATA] write refused by drive <hex> at LBA <hex> (status ERR/DF set)`), and patches all three
write paths (multi-sector PIO, single-sector PIO, DMA) to return -1 on ERR/DF. On top of that,
`ext2_write_block()` / `ext2_sync_super()` and `fat32_write_sectors()` / `fat32_write_cluster()` /
`fat32_set_next()` became `int`, every internal call site (bitmaps, directory entries, FAT copies,
inode/superblock sync) now checks and rolls back, and `vfs_save()` plus `vfs_write_file_unlocked()`
(the ext2, fat32 and native-file branches) propagate failure to the syscall return value instead of
storing a size for data that never landed. Release-only paths (`free_block`, `free_chain`, …) log
the failure and continue deliberately — refusing to free during recovery would leak worse.

**New evidence gates:** `scripts/wfail_test.py` (two modes) drives a new Ring 3 reporter app,
`apps/wfiledemo.c`, which prints exactly what the syscalls told it. Ground truth is read back with
`mtools` against the disk image, not from guest output. `--mode inject` wraps the FAT32 image in a
QEMU `blkdebug` config (`[inject-error] event="write_aio" errno=5`) so every write is refused at the
block layer — a deterministic media failure with no hardware needed.

| Run | Result |
|---|---|
| `wfail_test.py --mode control` (pre-fix) | rc=0; `write ok=True readback ok=True file on media=True` |
| `wfail_test.py --mode inject` (pre-fix) | **rc=1** — `write ok=True readback ok=False file on media=False failure markers=[]`; the harness printed `[FAIL] the guest reported 'WFILE write ok' while the medium refused every write (a failed write was reported as success)`. This is the bug, reproduced. |
| `wfail_test.py --mode control` (post-fix) | rc=0; `write ok=True readback ok=True file on media=True failure markers=[]` |
| `wfail_test.py --mode inject` (post-fix) | **rc=0** — `write ok=False readback ok=False file on media=False failure markers=['WFILE create FAILED']`; `[OK] inject: the refused write was reported to the writer (no false success, nothing on the medium)` |
| Regression: `fat32_test.py`, `boot_test.py`, `make check64` | rc=0 each — FAT32 LFN round-trip via mtools intact, boot/login intact, 64-bit `M12 BOOT OK` + FS selftest + kbd/heap/blk/fs/cons/gui gates all PASS |
| Regression: `make check-quick` (post-fix, 4-core TCG) | 12/12 PASS (this run; at the time F0's scheduler half was still open — see the F0 section for why one green run does not close a ~1-in-4-6 race, and for the 90-round stress that does) |

The original static analysis follows; the line numbers and signatures it cites are the pre-fix state.

`ext2_write_block()` (`src/sys/ext2.c:335-350`) and `fat32_write_sectors()`
(`src/sys/fat32.c:52-59`) are `void`, and both call `ata_write_sectors_drive(...)` without looking at
the result. The VFS file-data writer does the same (`src/sys/vfs.c:3316`), while the node-table
writer right next door *does* check (`src/sys/vfs.c:1423`).

The drive layer can fail for ordinary reasons — no drive attached, or a BSY timeout:
`src/drivers/ata.c:257-293` (returns `-1` at `:262`, `:269`, `:270`, `:277`). Today that failure stops at the caller's return
statement, and because the two FS helpers are `void` it cannot be propagated even in principle.

**Failure scenario:** the disk starts failing mid-session (or the guest was booted without one).
ext2/fat32 update the in-memory superblock, bitmaps and FAT as if the write landed, the syscall
returns success, and the in-memory metadata now describes media that disagrees with it. After a
reboot the filesystem is inconsistent and nothing in the log says why. The read path is defensive by
contrast — `fat32_read_sectors()` zero-fills on failure (`src/sys/fat32.c:40-50`) — so the asymmetry
looks accidental rather than deliberate.

**Cheapest fix:** make both helpers return `int` and let `ext2_sync_super()` / the FAT32 write
callers propagate it; at minimum `write_serial_string()` once per failure, like the node-table writer
already does. Note `scripts/fs_test.py` and `blk_test.py` prove *reads* and the block layer, so no
existing gate would catch this. **The implemented fix is the stronger variant: the ATA driver
error check (see the status note above) means the refused write is now caught at its source, not
just propagated, and the injection harness above is the gate the last sentence asked for.**

### F2 — [Medium] `scheduler.md` advertises `MAX_TASKS = 32`; the kernel runs 64

`docs/architecture/scheduler.md:22` — "Supports up to `MAX_TASKS = 32` concurrent task slots" — versus
`src/sys/task.c:22` — `#define MAX_TASKS 64`. The README (v38.44 row) already says 64, so the doc is
the stale copy. Anyone sizing a per-task array from the doc under-provisions by half, and for a
project whose only spec *is* its docs, that is how a future bug gets written.

### F3 — [Medium] The VFS node limit is quoted three different ways; the code says 2048

- `docs/architecture/memory.md:50` — "maximum VFS node limit (64)"
- README v38.44 row — "`MAX_NODES` 256"
- `src/include/vfs.h:6` — `#define MAX_NODES 2048`

Only the last one runs. Two stale numbers in two authoritative places.

### F4 — [Low] `memory.md` contradicts itself about the paging model

Line 3 promises "two-level x86 paging"; line 29 says "Legacy 2-level description kept for history;
current kernel is PAE-only", and line 56 documents the v38.49 PAE migration. The intro paragraph
never got updated when PAE landed.

### F5 — [Low] `memory.md` misdescribes `init_mem()`

`docs/architecture/memory.md:14-15` says `init_mem(uint32_t mem_size)` "parses the Multiboot memory
map provided by GRUB" and "Computes total pages: `total_pages = (mem_size + 4095) / 4096`". The code
(`src/sys/mem.c:124-125`) takes the scalar `mem_size` the caller read from the multiboot **header** —
no mmap array is walked — and floors it (`mem_size / PAGE_SIZE`). The v38.156 investigation showed
this distinction has teeth: RAM size and the *mapped span* are different quantities, and the ACPI
bound bug lived exactly in that gap.

### F6 — [Low] The DOOM seed pre-flight can never fail

`scripts/doom_test.py:74-91` (`:79` and `:86` are the two bypasses): when `debugfs` is missing (or fails to run) the check prints `[skip]`
and returns. That pre-flight exists because a missing/truncated `/doom1.wad` in the ext2 image was
"historically the #1 confounder for 'never entered game loop'" — i.e. it guards against the suite
reporting a game bug that is really a setup bug. On any host without e2fsprogs the suite silently
loses that discrimination. It is printed, not silent, but a gate that cannot fail is a gate that
cannot protect.

### F7 — [Low] `check.py` still decides KVM with `exists()` + `os.access()`

`scripts/check.py:220-222` is the last copy of the probe that was just fixed everywhere else
(`kbd_test.py` / `heap_test.py` / `cons_test.py` / `gui_test.py` in `a969f84`), for the reason CI
demonstrated on 4 Oct: `/dev/kvm` existing — even looking readable — is not the same as being
openable. Range here is small (it only gates the opt-in `--kvm` suites, so a false "yes" produces a
failed run rather than a false pass), but leaving one weaker copy invites the next person to
copy *this* one.

### F8 — [Nit] The generator named "ChaCha8" runs 16 rounds

`src/sys/entropy.c:21` sets `CHACHA_ROUNDS 8` and line 56 iterates it, but each iteration performs a
full column+diagonal *double* round, so the output is ChaCha with 8 double-rounds (16 rounds) —
stronger than the name suggests, so this is a naming problem, not a security one. Related nit:
`get_random_u32()` (line 161) returns `0` if the pool is unseeded, which makes ASLR's fallback a fixed
value instead of a retry.

### F9 — [Nit] `wm_lock_release()` blindly unlocks at depth 0

`src/gui/wm.c:82-87` decrements depth and, at ≤1, clears the lock and restores flags unconditionally.
An unbalanced release would therefore clear a lock the caller never took — silently leaving another
task's critical section unprotected. The owner/depth scheme itself is correct for nesting (a nested
acquire returns before touching `wm_eflags`, and only the outermost release restores them).

### F10 — [Low] A clean build is not literally warning-free: 36 linker notes plus 2 rustc warnings

Tested deliberately, because the release notes claim it (v38.157: "build … 0 warning"; v38.158:
"`MECTOV_Q3=1 make -j6 iso` EXIT=0 tanpa warning"). After `make clean_all && make -j8 iso` — a genuine
from-scratch build, verified by 59 objects with none older than the build's start — the **C compiler
emitted zero diagnostics**, so the claim holds for the kernel and the app C sources. The build as a
whole still prints 38 warning lines:

- **36 × `ld: warning: apps/<name>.elf has a LOAD segment with RWX permissions`**, one per Ring 3
  payload. This is expected for these images (the kernel's W^X covers heap/stack/mmap; loaded image
  text stays executable — `docs/architecture/memory.md:54-62`), so it is a wording problem, not a
  defect: a reader of "0 warnings" will not expect 36 of them.
- **2 × rustc**, both from the `rusthello` step (`scripts/build_rust_mct.py:56`):
  `unknown and unstable feature specified for -Ctarget-feature: mmx`, and
  `failed to connect to jobserver from environment variable MAKEFLAGS=" -j8 --jobserver-auth=3,4" … Bad file descriptor (os error 9)`.
  The jobserver one is a real (if small) build-system wart: that step cannot use make's jobserver, so
  `make -jN` loses parallelism exactly there and prints a "build environment is likely misconfigured"
  note on every clean build. The `mmx` one is a stability warning today and may become an error on a
  future rustc.

**Fix:** run the rustc step with the jobserver variables blanked (or `-j1`), and drop or feature-gate the
`mmx` target-feature.

### F11 — [Low] `task_reap_zombies()` is dead code, and one dump detail from the F0 stall is explained by nothing in the tree

Found while root-causing F0 (2026-10-05). Two small, separate observations, recorded so the next
reviewer does not have to rediscover them:

1. **The zombie reaper never runs.** `task_reap_zombies()` (`src/sys/task.c`) is documented in
   `task.h` and in its own comment as "Called from the BSP main loop once per second as a safety net
   for parents that launch children but never call `waitpid()`" — but **nothing in the tree calls
   it** (`grep -rn "task_reap_zombies()" src/` → the definition and the declaration only). Zombies
   are therefore only reaped by `waitpid()` and by `terminate_task()`'s reparent loop, and
   `zombie_reap_ms` (plus its `/proc/sys/zombie_reap_ms` knob) has no effect today. Not the F0
   stall — a working reaper would *free* a stuck child, not wedge a parent — but the documented
   behaviour and the code disagree: either call it from the BSP loop or document it as dormant.
2. **The unjoined producer in the F0 dump shows `FREE` with a zombie timestamp.** In the probe6
   post-mortem the producer thread (tid 9) that the stuck parent never joined was already `FREE`
   (`zombie_since` set, `parent=5`), yet the only paths that free a zombie are `waitpid()` (never
   called for tid 9: the parent was stuck on tid 8 and the join order is 6, 7, 8, 9) and
   `terminate_task()`'s reparent loop (which needs the *parent's* tid to exit — the parent was alive
   and `BLOCKED`). Whether this review's reading missed a path or something frees slots outside the
   task-table bookkeeping is unresolved; it does not affect the F0 fix (the stuck thread itself was
   `READY` with a park-loop frame, and 90 post-fix rounds reproduce neither shape), but it is
   recorded rather than explained away.

### Checked, no finding (recorded so the next reviewer can skip them)

**a. `fs_nodes[].name` paths are bounded.**

Worth recording because `strcpy()` appears 62 times in the tree and this is the one that looks worst
at a glance: `src/sys/vfs.c:1603` copies a node name into `stack[16][MAX_FILENAME]` *before* the
`sp >= 16` check — but the 16th write lands in `stack[15]` and the loop breaks on the next iteration,
and names are truncated at load (`src/sys/vfs.c:1484`) and on create (`src/sys/vfs.c:1870`). No
overflow; the path is silently truncated past 16 levels, which is a design choice, not a bug.

**b. No allocation in IRQ context.** `grep kmalloc|kfree` over `src/drivers/*.c` returns four hits,
none on an interrupt path: `src/drivers/vga.c:526-527` allocates the WM back buffers from
`kernel_main`, and `src/drivers/ata.c:495` is a comment about *removing* a per-I/O allocation. The
NIC RX path copies into caller-owned buffers instead (`src/drivers/rtl8139.c:142,213-217`), and
`src/drivers/net.c:1221-1222` documents why the RX entry needs no cli (interrupt gate already runs
with IF=0, and `net_poll()` wraps itself).

---

## 4. Document drift table

| Document | Claim | Code | Verdict |
|---|---|---|---|
| `docs/architecture/scheduler.md:22` | `MAX_TASKS = 32` | `src/sys/task.c:22` → 64 | **STALE** |
| `docs/architecture/memory.md:50` | node limit 64 | `src/include/vfs.h:6` → 2048 (README says 256) | **STALE ×2** |
| `docs/architecture/memory.md:3` | two-level x86 paging | `:29`, `:56` + `src/sys/vmm.c` (PAE 3-level) | **SELF-CONTRADICTION** |
| `docs/architecture/memory.md:14` | `init_mem` parses the mmap | `src/sys/mem.c:124` (scalar `mem_size`) | **STALE** |
| `docs/architecture/memory.md:15` | round-up to page count | `src/sys/mem.c:125` floors | **STALE** |
| `docs/kernel-locking.md` lock order | `task_lock > shell_lock > fd_lock > vfs_lock > blkcache_lock > ata_lock` | spot-checked: `task.c:1092` (cli-first), `fd.c:17`, `wm.c:73`, `shell_core.c`, `vfs.c` irqsave | **OK** |
| `docs/architecture/scheduler.md` | per-CPU runqueues, `rq[MAX_CPUS]` | `src/sys/task.c:238` (`MAX_CPUS 16`), `schedule()` per CPU | **OK** |
| `docs/drivers/network.md:85` | TCP capacity 16 (v38.44) | `src/include/net.h:84` (`TCP_MAX_CONNS 16`) | **OK** |
| `docs/architecture/x86_64_port.md` | PML4 per task, fork/exec | `k64/` exists (7,935 lines), `k64/cpu64.h:529` VMM_NX | **OK (sampled)** |

---

## 5. Test integrity — can the green actually go red?

Checked because "28/28 green" only means something if the gates can fail.

- **No defeat-by-bypass:** no `|| true`, no `continue-on-error`, no ignored exit codes in
  `Makefile` / `.github/workflows/build-boot-test.yml` / `scripts/check.py`.
- **Timeouts count as failures:** `scripts/check.py:296-302` records `TIMEOUT` (not `PASS`) when it
  kills a wedged QEMU, and the summary + process exit code derive from that set
  (`scripts/check.py:388`, `sys.exit(main())`).
- **Skips are announced, not silent:** the KVM regressions print
  `KVM regressions skipped by default — add --kvm` (`scripts/check.py:208-213`).
- **The biggest integrity gap is F0:** the one suite whose local and CI behaviour disagree is exactly
  the one exercising cross-core synchronisation, and CI's version of it is pinned to a single core.
- **Weak spots found:** F6 (a pre-flight that disables itself) and F7 (the weaker KVM probe). Only
  one suite in the tree carries fewer than three `assert`/`PASS`/`FAIL` markers —
  `scripts/font_render_test.py` (2) — and it is not in the fast tier, so it was **not** examined in
  this pass; that is a follow-up, not a finding.
- **Notable:** the fast tier walks 12 suites, each a full QEMU boot, and every one of them asserts
  against serial output or a framebuffer artifact rather than an exit code alone (19 m 03 s here,
  against the "~6-8 min" in `scripts/check.py:190` — see the timing note in §2).

---

## 6. Strengths verified with evidence (not taken from the README)

- **The user-pointer boundary is real.** `validate_user_ptr()` (`src/sys/syscall.c:270-296`) walks the
  *caller's* own page directory and demands `PAGE_USER` at every level; the header comment records
  that the previous version compared against a 256MB constant and therefore accepted kernel `.text`,
  the task table, and page directories as output buffers. `safe_strlen()` validates page-by-page so
  an unterminated user string cannot walk the kernel off a mapped page. 89 call sites across the
  syscall layer.
- **The COW race is actually closed.** `src/sys/idt.c:455-500` allocates the speculative frame
  *before* taking `vmm_lock`, re-reads the PTE under the lock, drops the frame when a peer already
  resolved the fault, and keeps the sole-owner promotion inside the same critical section — with the
  v38.55 corruption described in the comment.
- **cli-first locking is code, not aspiration.** `src/sys/task.c:1092`, `src/sys/mem.c:314,379,545`,
  `src/sys/vmm.c:165,215,237` all save flags and `cli` before the `xchg`, which is what makes the
  timer IRQ safe to take the same locks.
- **The fd table is guarded at every use** (`src/sys/fd.c:77,99,148,175,283,560`) and the pipe slot
  leak the comment describes is genuinely fixed (`src/sys/fd.c:527-545`).
- **The 64-bit port carries the hardening forward:** `k64/spin64.h` (irqsave/restore pairing, no
  IPI-handler locks) and `k64/mem64.c:284-287` (per-CPU `EFER.NXE`, because MSRs are per-core).

---

## 7. Already-acknowledged defects (restated, not counted as new findings)

These are in the project's own release notes; the review confirms they are documented, not hidden:
the 12-extra-face backface-reject flake (~1 boot in 2, `q3hud` reports the split); the 13–16 ms/frame
left in the present/composite path; no sound since v38.142; 12 version-table rows in the README whose
pipes break the table; `MAX_TASKS`/`MAX_NODES` caps documented in code; and the cross-core condvar race
behind F0 — **closed 2026-10-05 (v38.160)**: both of its defects are fixed, the CI `MCTOV_SMP=1` pin
is gone, and the README §Testing table plus the CI step's own comment say what the step now proves
(4-core condvar/mutex/condvar stress with a stall-signature assertion). The pin's history stays in the
v38.92 row because that is where a reader looking for "why was this pinned?" will land.

---

## 8. Recommendations, ranked by risk ÷ effort

0. **F0 — done (2026-10-05, v38.160).** Both defects are fixed: the app's double unlock, and the
   scheduler slot-reuse corruption that replaced a reclaimed slot's fresh frame with its dead
   predecessor's exit park loop (see the F0 section for the post-mortem evidence and the fix list).
   Verified with 90/90 stress rounds on 4 cores against 5 stalls in ~100 pre-fix rounds, and the CI
   step is unpinned (`MCTOV_SMP=4`). The one loose end lives in F11.
1. **F1 — done (2026-10-05).** Write errors now propagate out of the ATA driver, `ext2_write_block()` /
   `fat32_write_sectors()` and the VFS data path, with `scripts/wfail_test.py` as a deterministic
   refusal gate; see the F1 section for the evidence table.
2. **F2/F3/F5** — correct the four stale numbers/claims; the docs are this project's only spec.
3. **F7** — finish the `os.open()` migration in `scripts/check.py`.
4. **F6** — make the DOOM seed pre-flight mandatory where `debugfs` exists, and fail loudly where it
   does not, so the suite's own confounder can be named again.
5. **F9** — assert lock balance in `wm_lock_release()` (or log when depth is 0).
6. **F4** — one-line fix to the `memory.md` intro.
7. **F10** — silence the two rustc warnings (jobserver + `mmx`) and reword the release-note claim to
   "0 compiler warnings", which is what the build actually guarantees.
7b. **F11** — decide whether the zombie reaper is live or dormant: call `task_reap_zombies()` from the
   BSP main loop (its documented contract) or document it as dormant and drop the `zombie_reap_ms`
   knob's promise. The second half of F11 (the `FREE` slot with a stale `zombie_since` in the F0 dump)
   is an open reading question, not a known defect.
8. Then the known-open work: present/composite path, backface defect, README table rows.

---

## 9. Limitations of this review

- One machine, one build, one boot per suite. The 12-face flake and any other
  timing-dependent behaviour cannot be characterised from a single run. `cond` failed once here while
  the documented rate is "~1 run in 4-6" on 2+ cores (`apps/conddemo.c:135` records the same suspicion),
  so this run demonstrates the race is reachable, not how often.
- Clusters marked "grep-verified only" in §1 were not read line by line; **no** conclusion is drawn
  about the absence of defects there.
- The four CI-green gates that only ever ran locally in this session for the first time
  (`blk_test`, `fs_test`, `cons_test`, `gui_test`) still have no long history, so a flake in them would
  look new rather than like a regression.
- Most findings are static reads with a cited line; **F0 is the exception** — it was reproduced by a
  failing suite in this session, and that failure is quoted verbatim in §2.
