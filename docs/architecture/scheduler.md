# Task Scheduler Architecture

Mectov OS features a preemptive priority Round-Robin task scheduler capable of managing kernel threads (Ring 0) and user processes (Ring 3). Since v36.7 the scheduler is **per-CPU**: every core owns a runqueue and runs its own scheduling, so all four SMP cores execute user tasks instead of idling.

---

## ⚙️ Scheduler Overview (`src/sys/task.c`)

1. **Task States**:
   - `TASK_STATE_FREE` (0): Slot is unallocated.
   - `TASK_STATE_RUNNING` (1): Currently executing on a CPU core.
   - `TASK_STATE_READY` (2): Runnable and queued for execution.
   - `TASK_STATE_SLEEP` (3): Blocked waiting for timer ticks (`task_sleep`).
   - `TASK_STATE_BLOCKED` (4): Parked on a kernel primitive (waitpid, semaphore, futex).
   - `TASK_STATE_STOPPED` (5): Suspended by SIGSTOP/SIGTSTP/SIGTTIN.
   - `TASK_STATE_ZOMBIE` (6): Exited, awaiting reaping.

2. **Per-Core Runqueues**:
   - `rq[MAX_CPUS]`, one FIFO array of tids per core, guarded by `task_lock`.
   - A task is queued iff `tasks[tid].rq_cpu >= 0`. RUNNING members are only ever the current task of that CPU; pickers/stealers only take READY members, so a task can never execute on two CPUs at once.
   - `current_task[MAX_CPUS]` tracks the active task per core (`get_cid()`).
   - Supports up to `MAX_TASKS = 64` concurrent task slots (`src/sys/task.c`; the 32 published here until v38.161 was stale — the limit was raised with the GDT TLS coupling, and the README's v38.44 row already said 64). `MAX_CPUS` is 16.

3. **Idle Tasks**: Task 0 (the kernel main loop) is the BSP's idle, pinned to runqueue 0. Each Application Processor gets its own pinned Ring 0 idle task (`create_idle_task`, `ap_idle` hlt-park) during `init_tasking()`, so an empty queue parks the core instead of stealing the BSP's main loop.

4. **Migration / Load Balancing**: New tasks are enqueued on the least-loaded real core (`rq_least_loaded`, aware of `smp_cpu_count` — never phantom CPU slots). A core whose own queue is empty steals the best READY task from a peer (`rq_steal`); task 0 and idle tasks are never stolen.

---

## ⏱️ Preemptive Context Switch

1. **Timer Interrupt Trigger**:
   - The PIT interrupts the BSP at 1000 Hz; every AP runs its own **LAPIC timer** at ~1 kHz (rate calibrated ONCE on the BSP against the PIT before the APs wake — see `smp_and_apic.md`).
   - Interrupt Stub (`interrupt_entry.asm` -> `irq0`) pushes register frame (`registers_t`) to kernel stack and invokes `irq_handler(esp)`.

2. **Schedule Dispatch (`schedule(uint32_t esp)`, per CPU)**:
   - Takes `task_lock` (always cli-first: every other holder disables interrupts before locking, so the timer IRQ can never self-deadlock).
   - Sleep upkeep runs on the BSP only (`cid == 0`): decrements `sleep_ticks` and re-enqueues expired sleepers — one global clock, not four.
   - Saves the preempted frame (`tasks[cur].esp = esp`; RUNNING -> READY stays in the queue).
   - Picks from its own runqueue with priority+aging; steals from a peer if empty; keeps the current task if still runnable; otherwise parks (returns to the idle loop).
   - Updates the per-CPU TSS (`tss_set_kernel_stack`) so a Ring 3 interrupt on this core lands on the next task's kernel stack top.
   - Switches CR3 via `vmm_switch_page_dir` if the next task has a custom page directory.
   - Delivers pending signals (under the lock) to the Ring 3 task about to be resumed; if the signal's default action kills it, re-picks.
   - Returns the new task's saved ESP.

3. **Assembly Restoration (`irq_common_stub`)**:
   - `mov esp, eax` switches CPU stack pointer to the newly selected task's stack frame.
   - `popad` and `iret` restore registers and resume execution seamlessly in Ring 0 or Ring 3.

---

## 🛡️ Interrupt Safety & Deadlock Prevention

1. **One Lock, cli-First**: every runqueue transition (fork, thread-create, exec, sleep, wake, block, stop, continue, exit, signal, zombie reap) mutates `rq`/`tasks` under `task_lock` with interrupts disabled first. `schedule()` runs inside the timer IRQ (interrupt gate, IF=0) and takes the same lock safely because no holder is ever preemptible.
2. **Lock Ordering**: `sync_lock` (semaphores/futexes) -> `task_lock` is the only nesting; nothing takes `task_lock` and then a sync lock. Signal delivery on the syscall-return path (`syscall.c`) acquires `task_lock` via the exported `task_lock_acquire/release` cli-first, because `terminate_task` mutates the runqueues.
3. **Cross-CPU Kills**: `task_cleanup` checks `current_task[c]` on every core before freeing a dying task's page directory — a task killed while still mid-flight on another core (up to one tick later) causes a bounded leak instead of corrupting that core's CR3 page walks.
4. **SMP-Safe Serial**: serial output is serialized per call (`write_serial_string`/`write_serial_buffer`) so log lines from four CPUs cannot byte-interleave; app fd-1/2 writes are one locked buffer write per line.
---

## 🔎 Diagnosing a stall — the v38.160 slot-reuse bug (tutorial)

A stalled app on this tree has one dominant cause so far, and it is a scheduler-lifecycle bug rather
than a synchronization bug. The recipe below is what pinned it, and it is worth keeping because the
same shape will hide again the moment the invariant is relaxed.

**The invariant.** A task that has exited (`TASK_STATE_ZOMBIE`) or been freed (`TASK_STATE_FREE`)
must never be written to again, and a FREE slot must not be handed to a new task while any core still
names it as its `current_task[]`. The exit path (`task_exit_with_code`) can only park in a kernel
`hlt` loop — the switch away happens in the *next* `schedule()` on that core, i.e. up to 10 ms later
at 100 Hz. Inside that window the core's identity still names the slot.

**What used to go wrong.** If a create claimed that slot inside the window, the stale core's next tick
performed `tasks[cur].esp = esp` (`schedule()`, step 2) — writing the dead task's park-loop frame over
the new task's freshly built frame. The next pick then iret'd the new task *into dead code*: it never
executed one instruction of its own entry, its parent's `waitpid()` never saw it exit, and the parent
parked forever with no verdict. A boot's first run of an app was always safe (no slot had a
predecessor yet), which is exactly why single-run tests stayed green while repeated runs inside one
boot stalled.

**Reproduce it (4 vCPUs, TCG or KVM).**

```sh
make iso
python3 scripts/cond_test.py --repeat 60 --timeout 3000     # deep stress, one boot
python3 scripts/cond_test.py --repeat 1  --timeout 480      # the CI-shaped single round
```

`--repeat N` matches each round only against the bytes appended after that round started, so round
k>1 cannot be satisfied by round k-1's markers; any failed round fails the suite. A round with no
verdict is a stall: the harness prints a full post-mortem and exits 1.

**Reading the automatic post-mortem.** On a stall the harness dumps, over the QEMU monitor,
`info registers` for every vCPU, `current_task`, `timer_ticks`, `rq[0..3]`, `tasks[0,4..9]`, two
kernel stacks and the futex table. Resolve addresses with:

```sh
nm -n myos.bin.debug > /tmp/nm.txt     # globals keep their addresses across rebuilds
```

Facts the dump needs (verified against `src/include/task.h`, `sizeof(task_t) = 1520`):
`tasks[] = 0xa61140` (slot `t` at `0xa61140 + 0x5F0*t`), `rq[] = 0xa600e0` (65 dwords: 64 tids +
count, stride `0x104`), `current_task = 0x001cd140`, `timer_ticks = 0x002ec048`. Per-slot offsets:
`esp` +0x0, `stack_watermark` +0x4, `state` +0x8, `priority` +0x10, `wait_ticks` +0x18, `rq_cpu`
+0x1c, `is_idle` +0x20, `name` +0x128, `parent` +0x154, `exit_code` +0x158, `waiting` +0x164,
`zombie_since` +0x2f8, `cpu_ticks` +0x300. A saved interrupt frame is a `registers_t` at
`tasks[t].esp`, with `int_no` at +0x2c and `eip` at +0x34; `EIP` inside `task_exit_with_code`
(the `for(;;) hlt` tail) or `task_dead_park` means *this task's last saved context was a dead park
loop* — the signature.

**The `[WATCH]` tags** (`schedule()` / the 1 Hz integrity sweep, throttled per kind to ~1 line / 2 s):

| tag | meaning | action |
|---|---|---|
| `dead-cur` | a core ticked with a ZOMBIE/FREE task as `cur`; all writes to that slot were skipped | expected during normal exits; frequent lines + a stall means the window below is being hit |
| `create-skip` | a create wanted a FREE slot that a core still names as current, and moved to the next slot | informative: the v38.160 collision *was* hit and survived. Zero lines over a long stress means the window was never hit, not that it is gone |
| `revive-refused` | a wake/signal tried to flip FREE/ZOMBIE back to runnable | a real bug wherever it comes from: find the caller (stale waiter list, recycled tid) |
| `dead-in-rq` | a dead tid sat in a runqueue; the sweep removed the entry | a wake path reached a task after it died — audit that caller |
| `enqueue-dead`, `commit-dead` | the low-level enqueue / the switch commit refused a dead tid | same class as above, one layer lower |
| `parked-frame` | a READY/RUNNING task's saved frame is an exit/dead park loop — i.e. it was resumed into dead code | **this is the corruption itself**; `scripts/cond_test.py` fails the run on it |

**The rules that keep it fixed** (re-check them in any future scheduler work): writes to
`tasks[cur]` in `schedule()` are skipped when `cur` is dead; with nothing runnable, a dead core parks
on its own idle task instead of iret'ing a dead frame; `rq_enqueue`/the commit path refuse dead tids;
`task_set_state` refuses to revive FREE/ZOMBIE; every slot claim (`clone`, `fork`, `fork_exec`,
`create_idle_task`) skips a slot that is still current on a core. The fix's own evidence: 90 rounds
green on 4 vCPUs after the change (`--repeat 60` + `--repeat 30`), three `create-skip` events
survived, zero `parked-frame`.
