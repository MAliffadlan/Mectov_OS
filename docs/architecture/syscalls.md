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

---

## 🔌 POSIX Socket API (`SYS_SOCKET` 108 … `SYS_RECVFROM` 114, v38.43)

An fd-integrated socket layer: `socket()` and `accept()` return ordinary
file descriptors, so `read`/`write`/`close`/`poll`/`select` are
socket-aware and apps can treat a socket like any other fd.

- **`SYS_SOCKET` (domain, type)**: allocates an `FD_TYPE_SOCKET`
  descriptor (`SOCK_STREAM`/`SOCK_DGRAM` stored in the flags field). The
  TCP conn id is attached later by `listen`/`connect` (`sock_conn` = -1
  until then).
- **`SYS_BIND` (fd, sockaddr_t {family, port, ip})**: records the local
  port on the descriptor (the TCP stack assigns ephemeral ports itself for
  clients); a DGRAM bind also arms the stack's single global UDP binding.
- **`SYS_LISTEN` (fd, backlog)**: `net_tcp_listen(port)`. Backlog is the
  free-slot count: a LISTEN slot **never mutates into a connection** — each
  inbound SYN spawns a fresh child slot that completes the handshake on its
  own (`listen_parent` links it to the listener).
- **`SYS_ACCEPT` (fd)**: non-blocking — returns an fd for one ESTABLISHED
  child (marked `accepted`, one call per child) or -1. Poll the listener
  for POLLIN (an accept-able child exists).
- **`SYS_CONNECT` (fd, sockaddr_t*)**: returns once the SYN is out; poll
  the fd for POLLOUT (= ESTABLISHED).
- **`SYS_SENDTO`/`SYS_RECVFROM`**: with an address → UDP datagram
  (`recvfrom` fills the last peer's ip/port via `net_udp_peer`); without →
  stream send/recv on the attached conn.
- Poll readiness: listener → POLLIN when accept would return; stream →
  POLLOUT when established, POLLIN when `rx_len > 0` or EOF, POLLHUP when
  closed/reset.
- Demo: `run /apps/tcpserver.mct` — client phase (connect to a host echo
  server at 10.0.2.2:9999) then server phase (listen :8080, accept the
  host's hostfwd connection, PING→PONG). CI: `scripts/socktest.py`.

---

## 🗻 Runtime Mount Table (`SYS_MOUNT` 115 / `SYS_UMOUNT` 116, v38.42)

A mount point is an empty VFS directory node retyped to the backend's
`FS_*_DIR` — the existing type-based dispatch then routes read/write/
create/delete to the real filesystem — and `src/sys/vfs_mount.c` records
which drive + root key backs it so umount can undo the mapping.

- Boot mounts (`/ext2` drive 1, `/fat32` drive 3) are the table's first
  entries, registered from both `vfs_init` paths (fresh + loaded disk).
- **`SYS_MOUNT` (path, "ext2"|"fat32", drive)** — root only: creates the
  mount point when missing (must be an empty dir when present), runs
  `ext2_init`/`fat32_init` for the drive, populates the subtree from the
  real disk, registers the mount. Shell: `mount /mnt fat32 3`.
- **`SYS_UMOUNT` (path)** — root only: drops the subtree (data on disk is
  untouched), restores a plain `FS_DIR`, persists the node table. Shell:
  `umount /mnt`; `mount` with no args lists active mounts over serial.
- Limitation (until the backends grow per-volume state): ext2.c/fat32.c
  keep one global superblock each, so a runtime mount must target the
  drive the backend is initialized for (the boot drives or a remount).
- CI: `scripts/mount_test.py` — umount → refuse-non-mount → remount →
  fat32demo passes on the remounted filesystem.

---

## ⚡ FPU/SSE Context Switching (v38.41)

Every `task_t` carries a 16-byte-aligned 512-byte `fxsave` image
(`src/sys/fpu.c`); `schedule()` swaps it eagerly (fxsave out / fxrstor in)
with IF=0 under `task_lock`, so any number of FPU users — DOOM's x87
(kernel-mode, inside the shell task) and Ring 3 float/SSE code — can be
preempted against each other safely.

- Per-CPU bring-up (`fpu_init_cpu`) in `kernel_main` + every AP's
  `ap_main`: CR0 EM=0/MP=1/TS=0, CR4 OSFXSR|OSXMMEXCPT, CPUID-checked with
  a no-op fallback; builds the canonical `fninit` clean-state template.
- Task lifecycle: creation seeds the clean template, `fork()` inherits the
  parent's live state (flushed first — POSIX), `exec()` resets to clean.
- Exceptions: #NM is a logged safety net (the eager scheme never sets TS);
  #XM from Ring 3 delivers `SIGFPE` (default action terminates, 128+8).
- Demo: `run /apps/fputest.mct` — two forked processes accumulating in the
  live x87 register stack AND an SSE register across preemptions and
  across fork; both must stay exact. CI: `scripts/fputest.py`.
