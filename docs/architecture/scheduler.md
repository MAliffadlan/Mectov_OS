# Task Scheduler Architecture

Mectov OS features a preemptive priority Round-Robin task scheduler capable of managing kernel threads (Ring 0) and user processes (Ring 3).

---

## ⚙️ Scheduler Overview (`src/sys/task.c`)

1. **Task States**:
   - `TASK_STATE_FREE` (0): Slot is unallocated.
   - `TASK_STATE_RUNNING` (1): Currently executing on a CPU core.
   - `TASK_STATE_READY` (2): Runnable and queued for execution.
   - `TASK_STATE_SLEEP` (3): Blocked waiting for timer ticks (`task_sleep`).

2. **Per-Core Task Tracking**:
   - Maintains `current_task[MAX_CORES]` array to track the active task index running on each specific CPU core (`get_cid()`).
   - Supports up to `MAX_TASKS = 32` concurrent task slots.

---

## ⏱️ Preemptive Context Switch

1. **Timer Interrupt Trigger**:
   - Hardware PIT timer interrupts the CPU at 1000 Hz (1ms per tick).
   - Interrupt Stub (`interrupt_entry.asm` -> `irq0`) pushes register frame (`registers_t`) to kernel stack and invokes `irq_handler(esp)`.

2. **Schedule Dispatch (`schedule(uint32_t esp)`)**:
   - Saves current task's ESP pointer: `tasks[current_task[cid]].esp = esp`.
   - Decrements `sleep_ticks` for sleeping tasks; transitions expired tasks from `TASK_STATE_SLEEP` to `TASK_STATE_READY`.
   - Scans task array using Round-Robin algorithm for next `TASK_STATE_READY` task.
   - Updates Task State Segment (`TSS.esp0`) with new task's kernel stack top to guarantee safe Ring 3 -> Ring 0 stack switches.
   - Switches page directory (`CR3`) via `vmm_switch_page_dir` if the next task has a custom page directory.
   - Returns the new task's saved ESP stack pointer.

3. **Assembly Restoration (`irq_common_stub`)**:
   - `mov esp, eax` switches CPU stack pointer to the newly selected task's stack frame.
   - `popad` and `iret` restore registers and resume execution seamlessly in Ring 0 or Ring 3.

---

## 🛡️ Interrupt Safety & Deadlock Prevention

1. **Re-entrant Lock-Free Execution**:
   - `schedule()` executes directly inside the timer interrupt handler context.
   - Because IDT gate `0x8E` automatically clears the Interrupt Flag (`IF`), `schedule()` runs with nested interrupts disabled by hardware.
   - `schedule()` avoids acquiring spinlocks (`task_lock`) to eliminate self-deadlock conditions during timer interrupts.

2. **CLI/STI Synchronization in Task Operations**:
   - Helper functions (`create_task`, `create_user_task`, `thread_create`) explicitly disable interrupts (`cli`) *before* acquiring `task_lock` to prevent an incoming timer interrupt handler from attempting to acquire a lock already held by the same CPU.
