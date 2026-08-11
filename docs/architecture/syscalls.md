# System Call Subsystem Architecture

Mectov OS implements a modular System Call interface via software interrupt `int 0x80` (DPL=3 gate), allowing Ring 3 user applications to securely invoke kernel services.

---

## 🛰️ Modular Dispatch Architecture (`src/sys/syscall.c`)

Syscall handling is refactored into domain-specific sub-handlers to keep the kernel monolithic architecture structured and maintainable:

```
+--------------------------------------------------------------------+
|                         int 0x80 (isr128)                          |
+--------------------------------------------------------------------+
|                      syscall_handler(registers_t*)                 |
+--------------------------------------------------------------------+
         |                 |                 |                 |
  +--------------+  +--------------+  +--------------+  +--------------+
  | syscall_gui  |  | syscall_vfs  |  | syscall_proc |  | syscall_net  |
  +--------------+  +--------------+  +--------------+  +--------------+
```

---

## 📋 Syscall Register ABI

* **Syscall Number**: Passed in `EAX`.
* **Arguments**: Passed in `EBX`, `ECX`, `EDX`, `ESI`, `EDI`.
* **Return Value**: Returned in `EAX`.

---

## 🗂️ Core System Call Table

| Syscall ID | Name | Submodule | Description |
|---|---|---|---|
| `1` | `SYS_EXIT` | `syscall_proc` | Terminate current user task and reclaim resources |
| `2` | `SYS_WRITE` | `syscall_vfs` | Write text buffer to serial debug or virtual terminal |
| `3` | `SYS_MALLOC` | `syscall_proc` | Dynamically allocate pages to user task heap |
| `4` | `SYS_FREE` | `syscall_proc` | Release user heap allocation |
| `5` | `SYS_OPEN` | `syscall_vfs` | Open a file descriptor on Ext2/VFS |
| `6` | `SYS_READ` | `syscall_vfs` | Read data from open file descriptor |
| `7` | `SYS_CLOSE` | `syscall_vfs` | Close open file descriptor |
| `10` | `SYS_GET_TICKS` | `syscall_proc` | Return system uptime ticks (1000Hz) |
| `20` | `SYS_WM_CREATE` | `syscall_gui` | Create a new Z-ordered managed window |
| `21` | `SYS_WM_DRAW_PIXEL`| `syscall_gui` | Render pixel to window double buffer |
| `22` | `SYS_WM_FLUSH` | `syscall_gui` | Present window buffer to linear VBE framebuffer |
| `23` | `SYS_WM_GET_EVENT` | `syscall_gui` | Fetch keyboard/mouse input events for window |
| `30` | `SYS_NET_SEND` | `syscall_net` | Transmit raw network packet via RTL8139 NIC |
| `31` | `SYS_NET_RECV` | `syscall_net` | Receive incoming network packet |
| `32` | `SYS_DNS_LOOKUP` | `syscall_net` | Resolve domain name to IP address |
| `33` | `SYS_TCP_CONNECT` | `syscall_net` | Establish TCP stream connection |

> Note: the table above predates the modern syscall split — the authoritative
> numbering lives in `src/include/syscall.h` (current numbers differ; e.g. 82 =
> `SYS_MMAP`, 83 = `SYS_MUNMAP`). New syscalls are added there.

---

## 🗺️ File-Backed mmap (`SYS_MMAP_FILE` 93 / `SYS_MSYNC` 94)

`SYS_MMAP_FILE` maps an open VFS `FILE` fd into the task's mmap window
(`0x40000000..0x80000000`), completing the Unix memory model: a file lives in
memory and its pages are demand-paged *from the disk*.

- **Lazy fault-in**: no bytes are read at mmap time. Each page faults in on
  first access; the `#PF` handler reads that page's sectors straight off the
  ATA disk via `vfs_read_file_offset()` — an *unlocked* offset-aware reader
  that takes only `ata_lock` (innermost). A user fault can fire while the
  same CPU holds `vfs_lock` (e.g. `SYS_READ` copying into an mmap'd buffer),
  so the fault path must never take `vfs_lock` itself.
- **Dirty tracking**: pages fault in read-only; the first write faults again
  (RO→RW upgrade), marks the page dirty in a per-region kmalloc'd bitmap, and
  remaps it writable. The bitmap is per-task: fork() deep-copies it, since
  file-backed pages carry `PAGE_SHARED` and stay shared (never COW'd).
- **Write-back**: `SYS_MSYNC` and `SYS_MUNMAP` flush dirty pages back to the
  file with a whole-file read-modify-write (files ≤ disk size). The file's
  CURRENT size is the write-back bound: bytes written past EOF are dropped
  (POSIX — a MAP_SHARED mapping never grows a file; growth comes from
  write()/ftruncate), and a concurrent writer is never clobbered. The mapping
  records the VFS node itself, so closing the fd after mmap is legal.
- **Fork**: file-backed PTEs are `PAGE_SHARED` so `vmm_clone_address_space`
  leaves them shared between parent and child (true MAP_SHARED semantics);
  exec/exit discard mappings without writeback (POSIX-style).
- Demo: `run /apps/mmapfiledemo.mct` (see `apps/mmapfiledemo.c`).

---

## 📍 POSIX File Positioning & Metadata (`SYS_LSEEK` 95 / `SYS_FSTAT` 96)

Completes the file I/O model next to file-backed mmap: descriptors now track a
read/write **offset**, reads honor it, writes can **append**, and apps can
query metadata without re-resolving a path.

- **Offset-aware reads**: `SYS_READ` no longer starts from byte 0 every time.
  The fd layer reads at the descriptor's current offset via
  `vfs_read_file_offset()` (the unlocked offset reader built for mmap faults
  — takes only `ata_lock`, safe under `fd_lock`) and advances the offset by
  the bytes read, POSIX-style. `FS_FILE` nodes get full offset semantics;
  dev/proc nodes keep the legacy whole-read path.
- **`SYS_LSEEK`**: `SEEK_SET`/`SEEK_CUR`/`SEEK_END` reposition the
  descriptor offset and return the new value. Negative results and non-file
  descriptors (pipes) return -1; seeking a pipe is rejected.
- **`O_APPEND`**: `SYS_OPEN`'s third argument (mode) is now stored on the
  descriptor. Writes on an `O_APPEND` fd always land at the end of the file
  regardless of the current offset — matching POSIX.
- **`SYS_FSTAT`**: fills a `stat_t {size, type, node_idx, parent,
  data_sector, name}` from the fd's VFS node — no path resolution needed, no
  race on rename. `type` mirrors `fs_type_t` (0=file, 1=dir, 2=dev).
- Writes remain whole-file read-modify-write (files ≤ 4 KB), with the
  descriptor offset / O_APPEND selecting the splice point.
- Demo: `run /apps/lseekfiledemo.mct` (see `apps/lseekfiledemo.c`).
