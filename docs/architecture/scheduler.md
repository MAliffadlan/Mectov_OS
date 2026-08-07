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
   - Supports up to `MAX_TASKS = 32` concurrent task slots.

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
