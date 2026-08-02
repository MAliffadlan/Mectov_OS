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
