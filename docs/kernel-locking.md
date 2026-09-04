# Kernel Locking Audit (v38.4)

Audit of every piece of shared mutable kernel state, who touches it, and the
lock that protects it. Goal: remove the global `cli()` at the syscall entry
so syscalls become preemptible and SMP cores can run different syscalls in
parallel, replacing the "one big interrupt-disable" with per-structure
irqsave spinlocks.

## The problem with the old model

The syscall handler (int 0x80) began with `cli()` and ran the whole syscall
non-preemptible. That only protects against IRQ preemption on the **local**
core — `cli` is per-CPU — so on 4 cores two syscalls were already racing on
shared state. The global `cli()` also made every syscall a serialization
point: core 2's `SYS_READ` waits for nothing, but two cores touching the same
VFS node could corrupt the node table.

## Lock discipline (this audit)

- **Process context** (syscalls, main loop, shell): `spin_lock_irqsave()` /
  `spin_unlock_irqrestore()` — disables local IRQs while holding, so a timer
  IRQ can never re-enter the same structure on this core, and a cross-core
  holder is never preemptible either (no self-deadlock).
- **IRQ / exception context** (keyboard, mouse, timer, #PF): plain
  `spin_lock()` — IF is already 0 there. A holder on another core releases
  within a bounded critical section; the faulting task's context (user mode)
  never holds kernel locks, so a #PF can spin safely.
- **Reentrant locks** (`vfs_lock`, `wm_lock`, `shell_lock`): the public
  functions call each other (e.g. `vfs_write_file` → `vfs_get_node`,
  `wm_handle_key` → `win_key_cb` → `push_event` → `get_win_index`, and
  `run_script` → `ex_cmd` nested). Each lock tracks `owner tid + depth` so a
  nested call on the same task is a no-op.

### Global lock ordering (never acquire in reverse)

```
task_lock  >  shell_lock  >  fd_lock  >  vfs_lock  >  blkcache_lock  >  ata_lock
task_lock  >  wm_lock     >  gui_lock
task_lock  >  vmm_lock            (fork clones address space under task_lock)
vmm / heap / sync / kbd / clipboard / net (rtl_lock) / shm / pcache — leaf locks
```

`blkcache_lock` (v38.62, blkcache.c) sits between vfs_lock and ata_lock:
its read-miss path in the ata.c wrappers holds it across the real ATA op
(nests blkcache_lock > ata_lock; the ATA layer serializes every transfer
under ata_lock anyway, and no code takes blkcache_lock while holding
ata_lock — write invalidation runs AFTER the ATA call returns). It is a
leaf in every other respect: cache data lives in static .bss, so the
#PF handler's lock-free VFS read path (vfs_read_file_offset → ATA) can
take it safely — no fault can recur while it is held.

Justification, from the code:
- `terminate_task()` (task.c:507) calls `wm_cleanup_task()` → wm_lock, while
  the caller (`task_signal`) holds task_lock.
- `fork_common()` holds task_lock and calls `vmm_clone_address_space()` →
  frame_alloc → vmm_lock.
- `do_sys_open()` (fd_lock) calls `vfs_get_node()` → vfs_lock; VFS calls
  `ata_read/write_sector()` → ata_lock.
- `SYS_EXEC_CMD` (shell_lock) runs shell builtins that call fd.c and vfs.c.

## Shared state inventory

| Structure | File | Access contexts | Protection |
|---|---|---|---|
| `tasks[]`, runqueues, `current_task[]` | task.c | timer IRQ (schedule), syscalls (kill/waitpid/setpgid), fork/exec | `task_lock`, cli-first |
| frame bitmap, `frame_ref_count[]` | vmm.c | syscalls, fork/exec, **#PF handler (COW)** | `vmm_lock`; #PF reads locked via `vmm_lock_acquire_irq()` |
| kmalloc heap | mem.c | syscalls, loader, any kernel code | `heap_lock`, cli-first |
| sem/futex tables | sync.c | syscalls | `sync_lock`, cli-first |
| shared-memory segments | shm.c | syscalls | `shm_lock`, cli-first |
| NIC TX/RX, ARP/TCP state | net.c, rtl8139.c | IRQ 43, main loop, net syscalls, shell fetch | `rtl_lock` + cli/sti wrappers |
| `win_queues[]`, `win_canvases[]` | syscall.c | main loop replay, GUI syscalls, input dispatch | `gui_canvas_lock` (irqsave) |
| `wm_wins[]`, `wm_focused`, z-order | wm.c | main loop draw, GUI syscalls, task-exit cleanup | `wm_lock` (reentrant, NEW) |
| `global_fds[]`, `pipes[]` | fd.c | VFS/file syscalls, fork fd-rewire, shell redirection | `fd_lock` (NEW) |
| `fs_nodes[]`, current dir, disk image | vfs.c, ext2.c | VFS/file syscalls, shell builtins, loader, passwd, boot | `vfs_lock` (reentrant, NEW) |
| ATA controller (IDE ports) | ata.c | VFS save/load, ext2, loader | `ata_lock` (NEW) |
| sector read cache (`blkcache[]`) | blkcache.c | ata.c read/write wrappers, all FS reads incl. the #PF path | `blkcache_lock` (v38.62; read-miss nests > ata_lock) |
| `kbd_buffer`, head/tail, `esc_pressed` | keyboard.c | IRQ 33 (push), main loop + SYS_GET_KEY (pop) | `kbd_lock` (NEW) |
| clipboard | clipboard.c | syscalls | `clipboard_lock` (NEW) |
| shell globals (`cmd_b`, env, alias, history) | shell/ | SYS_EXEC_CMD (any terminal task) | `shell_lock` (reentrant, NEW) |

### Known remaining gaps (documented, not fixed this session)

- `mouse_x/y/btn` and keyboard modifier flags (`shift_p`, `keyboard_ctrl_held`)
  are aligned 32-bit ints written by IRQ and read by process context — atomic
  on x86, may be one tick stale. Benign, matches Linux input handling.
- Task-table read accessors (`task_get_fd`, `task_get_launch_arg`, …) read
  fields without task_lock. Aligned-int reads are atomic and a slot's fields
  remain valid after ZOMBIE; worst case a stale snapshot, never corruption.
- Raw framebuffer drawing (syscall `SYS_DRAW_RECT`/`SYS_DRAW_TEXT` vs main
  loop `full_redraw`) is not lock-protected; the draw commands are queued
  under gui_lock but the final blit can tear. Pre-existing, cosmetic.
- `vfs_save()` holds vfs_lock across the 256-sector write-back. Correct;
  a snapshot-then-write optimization is possible later.

## Syscalls that stay non-preemptible

`SYS_EXEC` and `SYS_SIGRETURN` keep the entry `cli()`: they rewrite the
task's register frame / address space in place and are cheap. Everything else
runs preemptible under its structure locks. `SYS_FORK` and
`SYS_THREAD_CREATE` already take their own `cli()` internally, so they need
no entry cli.
